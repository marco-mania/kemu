/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "editvmdialog.h"

#include "const.h"
#include "dialogs/diskdialog.h"
#include "dialogs/settingsdialog.h"
#include "utils/diskkeystore.h"
#include "utils/externaltools.h"
#include "utils/qemucapabilities.h"
#include "utils/qemucommandbuilder.h"
#include "utils/qemuimgprocess.h"
#include "utils/sandboxcapabilities.h"
#include "utils/snapshotmanager.h"
#include "utils/tpmstate.h"
#include "utils/utils.h"
#include "utils/vmconfig.h"
#include "widgets/contextualhelp.h"
#include "widgets/inlinemessage.h"

#include <KContextualHelpButton>
#include <KMessageBox>
#include <KMessageWidget>

#include <QButtonGroup>
#include <QFontDatabase>
#include <QMenu>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QStyle>
#include <QTabBar>

#include <bit>

namespace
{
// Marks a button that shows or hides part of the dialog, which makeReadOnly()
// leaves working.
constexpr char DisclosureProperty[] = "kemuDisclosure";

// Select the combo entry whose userData matches the given enum value.
template<typename E>
void setCombo(QComboBox *combo, E enumValue)
{
    const int idx = combo->findData(static_cast<int>(enumValue));
    if (idx >= 0)
        combo->setCurrentIndex(idx);
}
// Takes no input while it keeps its look. In the read-only view a setting
// stays as legible as where it can be changed, and grey keeps meaning that
// it does not apply.
class InputLock : public QObject
{
public:
    using QObject::QObject;

    static void apply(QWidget *widget)
    {
        widget->installEventFilter(new InputLock(widget));
        widget->setFocusPolicy(Qt::NoFocus);
        widget->setAttribute(Qt::WA_Hover, false); // no highlight inviting a click
    }

protected:
    bool eventFilter(QObject * /*watched*/, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::Shortcut:
        case QEvent::ContextMenu:
            return true;
        default:
            return false;
        }
    }
};

// Text stays selectable, settings keep their look but take no input, and
// actions are disabled.
void makeReadOnly(QWidget *widget)
{
    if (auto *combo = qobject_cast<QComboBox *>(widget)) {
        InputLock::apply(combo);
        if (QLineEdit *edit = combo->lineEdit())
            edit->setReadOnly(true);
        return;
    }
    if (auto *spin = qobject_cast<QAbstractSpinBox *>(widget)) {
        spin->setReadOnly(true);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setFocusPolicy(Qt::NoFocus);
        return;
    }
    if (auto *edit = qobject_cast<QLineEdit *>(widget)) {
        edit->setReadOnly(true);
        return;
    }
    if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) {
        edit->setReadOnly(true);
        return;
    }
    if (auto *view = qobject_cast<QAbstractItemView *>(widget)) {
        view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        // Settings in cells, like the port forwarding rules.
        for (QWidget *child : view->viewport()->findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
            makeReadOnly(child);
        return;
    }
    if (qobject_cast<KContextualHelpButton *>(widget))
        return; // explains, changes nothing
    if (qobject_cast<QTabBar *>(widget) || qobject_cast<QScrollBar *>(widget))
        return; // moves around, changes nothing
    if (widget->property(DisclosureProperty).toBool())
        return; // shows and hides, changes nothing
    if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
        // A check box shows a setting; any other button starts an action.
        if (button->isCheckable())
            InputLock::apply(button);
        else
            button->setEnabled(false);
        return;
    }
    if (qobject_cast<QAbstractSlider *>(widget)) {
        InputLock::apply(widget);
        return;
    }
    // The check box in its title; the walk goes on into its content.
    if (auto *group = qobject_cast<QGroupBox *>(widget); group && group->isCheckable())
        InputLock::apply(group);
    for (QWidget *child : widget->findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
        makeReadOnly(child);
}

KMessageWidget *createInlineMessage(KMessageWidget::MessageType type)
{
    auto *message = new KMessageWidget();
    InlineMessage::setType(message, type);
    message->setCloseButtonVisible(false);
    message->setWordWrap(true);
    message->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    message->setMinimumWidth(0);
    message->hide();
    return message;
}

void updateInlineMessage(KMessageWidget *message, const QString &text, KMessageWidget::MessageType type)
{
    if (!message)
        return;

    message->setAccessibleName(text);
    if (text.isEmpty()) {
        if (!message->isHidden() && !message->isHideAnimationRunning())
            message->animatedHide();
        return;
    }

    InlineMessage::setType(message, type);
    if (message->text() != text)
        message->setText(text);
    if (message->isHidden() || message->isHideAnimationRunning())
        message->animatedShow();
}

QString accel3dToolTip()
{
    return i18n("Renders the guest's 3D graphics on the host GPU.");
}

// Any size can be typed; the arrow buttons, keys and wheel move between
// powers of two, the sizes adapters usually have.
class PowerOfTwoSpinBox : public QSpinBox
{
public:
    using QSpinBox::QSpinBox;

    void stepBy(int steps) override
    {
        auto value = static_cast<unsigned>(qMax(1, this->value()));
        for (; steps > 0 && value < static_cast<unsigned>(maximum()); --steps)
            value = std::bit_floor(value) * 2;
        for (; steps < 0; ++steps)
            value = std::has_single_bit(value) ? value / 2 : std::bit_floor(value);
        setValue(qBound(minimum(), static_cast<int>(value), maximum()));
    }
};

// The field with a button beside it listing the powers of two within the
// field's range, the typical sizes.
QWidget *withSizeMenu(QSpinBox *spin, QToolButton **button)
{
    auto *presetButton = new QToolButton();
    presetButton->setIcon(QIcon::fromTheme(u"arrow-down"_s));
    presetButton->setPopupMode(QToolButton::InstantPopup);
    presetButton->setToolTip(i18n("Typical sizes"));
    auto *menu = new QMenu(presetButton);
    presetButton->setMenu(menu);
    // Built when opened, for the range the field has at that moment.
    QObject::connect(menu, &QMenu::aboutToShow, spin, [spin, menu]() {
        menu->clear();
        for (int mb = qMax(1, spin->minimum()); mb <= spin->maximum(); mb = static_cast<int>(std::bit_floor(static_cast<unsigned>(mb))) * 2) {
            QAction *action = menu->addAction(mb < 1024 ? i18n("%1 MiB", mb) : i18n("%1 GiB", mb / 1024));
            action->setCheckable(true);
            action->setChecked(mb == spin->value());
            QObject::connect(action, &QAction::triggered, spin, [spin, mb]() {
                spin->setValue(mb);
            });
        }
    });

    auto *field = new QWidget();
    auto *layout = new QHBoxLayout(field);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(spin, 1);
    layout->addWidget(presetButton);
    *button = presetButton;
    return field;
}

// A line under an option, smaller and dimmer than the option, as in System
// Settings.
QLabel *createDescription()
{
    auto *label = new QLabel();
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setFont(QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont));
    // Secondary information, and setForegroundRole() follows colour scheme
    // changes.
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

// Sets the line under @p option, which screen readers announce with it, and
// hides it while empty. Only for lines already in the dialog: a line without
// a parent would show as a window of its own.
void describe(QAbstractButton *option, QLabel *description, const QStringList &sentences)
{
    const QString text = sentences.join(u' ').trimmed();
    description->setText(text);
    description->setHidden(text.isEmpty());
    option->setAccessibleDescription(text);
}

// Help in paragraphs: how an option works, what it needs, and what it does
// not do.
QString helpParagraphs(const QStringList &paragraphs)
{
    QString html;
    for (const QString &paragraph : paragraphs)
        html += u"<p>"_s + paragraph.toHtmlEscaped() + u"</p>"_s;
    return html;
}

// Where the text of @p option starts: after its box or circle.
int optionTextIndent(const QAbstractButton *option)
{
    const QStyle *style = option->style();
    if (qobject_cast<const QRadioButton *>(option))
        return style->pixelMetric(QStyle::PM_ExclusiveIndicatorWidth, nullptr, option)
            + style->pixelMetric(QStyle::PM_RadioButtonLabelSpacing, nullptr, option);
    return style->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, option) + style->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, nullptr, option);
}

// @p content moved in by @p indent, on the right in right-to-left layouts.
QLayout *indented(QWidget *content, int indent)
{
    auto *row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->addSpacing(indent);
    row->addWidget(content, 1);
    return row;
}

QLayout *indented(QLayout *content, int indent)
{
    auto *row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->addSpacing(indent);
    row->addLayout(content, 1);
    return row;
}

// @p option with the line saying what it does under it, lined up with its
// text. @p row: the option with what goes beside it, if anything.
QLayout *withDescription(QAbstractButton *option, QLayout *row, QLabel *description)
{
    auto *block = new QVBoxLayout();
    block->setSpacing(0);
    if (row)
        block->addLayout(row);
    else
        block->addWidget(option);
    block->addLayout(indented(description, optionTextIndent(option)));
    return block;
}

void setDisclosed(QToolButton *button, QWidget *panel, bool open)
{
    // Drawn by the style, so it needs no icon theme.
    const Qt::ArrowType closed = button->layoutDirection() == Qt::RightToLeft ? Qt::LeftArrow : Qt::RightArrow;
    button->setArrowType(open ? Qt::DownArrow : closed);
    panel->setVisible(open);
}

// A button that shows and hides @p panel, for settings few need. Not
// checkable, so that it does not look pressed while the panel shows.
QToolButton *createDisclosure(const QString &text, QWidget *panel)
{
    auto *button = new QToolButton();
    button->setText(text);
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setProperty(DisclosureProperty, true);
    QObject::connect(button, &QToolButton::clicked, panel, [button, panel]() {
        setDisclosed(button, panel, panel->isHidden());
    });
    setDisclosed(button, panel, false);
    return button;
}

// The help of the 3D options. Each says how its option works, then what it
// needs on the host and in the guest, then what it does not do. "6.13+":
// later versions do as well.
QString universalHelp()
{
    return helpParagraphs({i18n("Forwards guest OpenGL calls to the host, which runs them via its own GPU driver."),
                           i18n("Host requirements: any GPU with an OpenGL driver. Guest: Mesa (includes the virgl driver)."),
                           i18n("Limitations: forwarding every call reduces performance, especially in games. Without direct GPU "
                                "memory access, Linux guests are limited to OpenGL 4.3. Windows only has experimental community "
                                "drivers.")});
}

QString nativeContextHelp()
{
    return helpParagraphs({i18n("Gives the guest direct, low-level access to the host GPU driver for near-native performance. "
                                "The guest runs the GPU's own Mesa driver."),
                           i18n("Host requirements: those of direct GPU memory access, plus QEMU 11.0+ and an Intel (i915), AMD, "
                                "or Qualcomm GPU supported by virglrenderer. Guest: Linux 6.14+ and Mesa 26.1+ (Intel) or 25.0+ "
                                "(AMD) built with native context support."),
                           i18n("Limitations: Linux guests only. Debian and Ubuntu currently build Mesa without Intel support. "
                                "KEMU cannot verify virglrenderer or the guest; if unsupported, the guest falls back to "
                                "Universal.")});
}

QString vulkanHelp()
{
    return helpParagraphs({i18n("Forwards guest Vulkan calls to the host, just as Universal does for OpenGL."),
                           i18n("Host requirements: those of direct GPU memory access, plus a Vulkan driver and virglrenderer "
                                "built with Venus. Guest: Linux 5.16+ and Mesa's Venus driver."),
                           i18n("Limitations: disables QEMU's seccomp sandbox for this VM. Windows only has experimental drivers "
                                "(such as vgpu3d), which recommend a 4 GiB memory limit.")});
}

QString hostBlobsHelp()
{
    return helpParagraphs({i18n("Lets the guest access host GPU memory directly instead of copying every change, improving 3D "
                                "performance and raising Universal to OpenGL 4.6 (from 4.3)."),
                           i18n("Host requirements: QEMU 9.2+ with virglrenderer 1.0+, and Linux 6.13+ (required to map GPU memory "
                                "into a VM). Guest: Mesa."),
                           i18n("Limitations: currently used only by Linux guests. Automatically enabled when Native or Vulkan "
                                "is selected.")});
}

QString hostBlobsMissingText()
{
    // The capability overview names the missing piece.
    return i18n("Not supported on this system. Settings → System → Capabilities shows why.");
}

QString missingInQemuText()
{
    return i18n("The QEMU in use does not provide this. Settings → System → Capabilities lists what it lacks.");
}
} // namespace

EditVmDialog::EditVmDialog(Mode mode, QWidget *parent)
    : QDialog(parent)
    , m_mode(mode)
{
    setWindowTitle(mode == Mode::Edit ? i18n("Edit Virtual Machine") : i18n("Customize Virtual Machine"));
    resize(640, 560);

    auto *mainLayout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget();
    m_tabs = tabs;

    // --- General Tab ---
    auto *generalTab = new QWidget();
    auto *genMainLayout = new QVBoxLayout(generalTab);

    auto *contentWidget = new QWidget();
    contentWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(0, 0, 0, 0);

    // Top section: Name + Comment on left, Icon on right
    auto *topLayout = new QHBoxLayout();

    auto *topFormLayout = new QFormLayout();
    topFormLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    nameEdit = new QLineEdit();
    nameEdit->setPlaceholderText(i18n("e.g. Win11_Dev"));
    nameEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(u"^[a-zA-Z0-9_\\-. ]*$"_s), this));

    topFormLayout->addRow(i18n("Name:"), nameEdit);

    nameWarningMessage = createInlineMessage(KMessageWidget::Error);
    topFormLayout->addRow({}, nameWarningMessage);

    commentEdit = new QPlainTextEdit();
    commentEdit->setPlaceholderText(i18n("Optional notes about this VM"));
    commentEdit->setFixedHeight(90);
    topFormLayout->addRow(i18n("Comment:"), commentEdit);

    topLayout->addLayout(topFormLayout, 1);

    topLayout->addSpacing(12);

    iconSelector = new VmIconSelector();
    topLayout->addWidget(iconSelector, 0, Qt::AlignTop);

    // Remaining form fields
    auto *genLayout = new QFormLayout();
    genLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    osFamilyCombo = new QComboBox();
    osFamilyCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    osFamilyCombo->addItem(i18n("GNU/Linux"), static_cast<int>(VMSpecs::OsFamily::Linux));
    osFamilyCombo->addItem(i18n("Microsoft Windows"), static_cast<int>(VMSpecs::OsFamily::Windows));
    osFamilyCombo->addItem(i18n("BSD"), static_cast<int>(VMSpecs::OsFamily::BSD));
    osFamilyCombo->addItem(i18n("DOS"), static_cast<int>(VMSpecs::OsFamily::DOS));
    osFamilyCombo->addItem(i18n("Other"), static_cast<int>(VMSpecs::OsFamily::Other));
    // Shown while nothing is chosen; no item, so it cannot be chosen again.
    osFamilyCombo->setPlaceholderText(i18nc("@info:placeholder", "Select an OS family"));
    osFamilyCombo->setCurrentIndex(-1);
    genLayout->addRow(i18n("OS family:"), osFamilyCombo);

    osNameCombo = new QComboBox();
    osNameCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    osNameCombo->setEnabled(false);
    osNameCombo->setEditable(true);
    osNameCombo->completer()->setFilterMode(Qt::MatchContains);
    osNameCombo->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    osNameCombo->completer()->setCompletionMode(QCompleter::PopupCompletion);
    osNameCombo->completer()->setModel(osNameCombo->model());
    osNameCombo->setInsertPolicy(QComboBox::NoInsert);

    auto *nameLayout = new QHBoxLayout();
    nameLayout->addWidget(osNameCombo);

    genLayout->addRow(i18n("OS name:"), nameLayout);

    osNameLabel = qobject_cast<QLabel *>(genLayout->labelForField(nameLayout));

    isoPathRequester = new KUrlRequester();
    isoPathRequester->lineEdit()->setPlaceholderText(i18n("Leave empty for HDD boot"));
    isoPathRequester->lineEdit()->setClearButtonEnabled(true);
    isoPathRequester->setMode(KFile::File | KFile::ExistingOnly | KFile::LocalOnly);
    isoPathRequester->setNameFilter(i18n("ISO Images (*.iso)"));
    genLayout->addRow(i18n("ISO image:"), isoPathRequester);
    isoPathLabel = qobject_cast<QLabel *>(genLayout->labelForField(isoPathRequester));

    bootFromIsoCheck = new QCheckBox(i18n("Boot from ISO"));
    bootFromIsoCheck->setToolTip(i18n("If checked, the VM boots from the ISO image instead of the hard disk."));
    bootFromIsoCheck->setEnabled(!isoPathRequester->text().isEmpty());
    genLayout->addRow({}, bootFromIsoCheck);

    floppyPathRequester = new KUrlRequester();
    floppyPathRequester->lineEdit()->setPlaceholderText(i18n("Leave empty if not needed"));
    floppyPathRequester->lineEdit()->setClearButtonEnabled(true);
    floppyPathRequester->setMode(KFile::File | KFile::ExistingOnly | KFile::LocalOnly);
    floppyPathRequester->setNameFilter(i18n("Floppy Images (*.img *.ima *.flp *.vfd);;All Files (*)"));
    genLayout->addRow(i18n("Floppy image:"), floppyPathRequester);
    floppyPathLabel = qobject_cast<QLabel *>(genLayout->labelForField(floppyPathRequester));

    bootFromFloppyCheck = new QCheckBox(i18n("Boot from floppy"));
    bootFromFloppyCheck->setToolTip(i18n("If checked, the VM boots from the floppy image (drive A:)."));
    bootFromFloppyCheck->setEnabled(false);
    genLayout->addRow({}, bootFromFloppyCheck);

    auto *shareLayout = new QHBoxLayout();
    sharedPathEdit = new QLineEdit();
    sharedPathEdit->setPlaceholderText(i18n("Host directory"));
    auto *btnBrowseShare = new QPushButton(QIcon::fromTheme(u"document-open"_s), {});
    sharedPathEdit->setClearButtonEnabled(true);
    shareLayout->addWidget(sharedPathEdit);
    shareLayout->addWidget(btnBrowseShare);
    genLayout->addRow(i18n("Shared folder (VirtFS):"), shareLayout);
    shareToolWarning = createInlineMessage(KMessageWidget::Warning);
    genLayout->addRow({}, shareToolWarning);

    sharedNameEdit = new QLineEdit();
    sharedNameEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(u"^[a-zA-Z0-9]*$"_s), this));
    sharedNameEdit->setClearButtonEnabled(true);
    genLayout->addRow(i18n("Shared name (for guest):"), sharedNameEdit);

    sharedNameEdit->setEnabled(!sharedPathEdit->text().isEmpty());

    contentLayout->addLayout(topLayout);
    contentLayout->addLayout(genLayout);
    genMainLayout->addWidget(contentWidget);
    genMainLayout->addStretch();

    tabs->addTab(generalTab, QIcon::fromTheme(u"computer"_s), i18n("General"));

    // --- System Tab ---
    // Three groups, set apart by space: processor and memory, the platform
    // as the cards in the details view name it, and what happens around the VM.
    auto *sysTab = new QWidget();
    auto *sysLayout = new QFormLayout(sysTab);
    sysLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    const auto addGroupGap = [sysLayout, this] {
        sysLayout->addItem(new QSpacerItem(0, fontMetrics().height(), QSizePolicy::Minimum, QSizePolicy::Fixed));
    };

    cpuCombo = new QComboBox();
    {
        const QMetaEnum metaEnum = QMetaEnum::fromType<VMSpecs::CpuModel>();
        for (int i = 0; i < metaEnum.keyCount(); ++i) {
            auto model = static_cast<VMSpecs::CpuModel>(metaEnum.value(i));
            cpuCombo->addItem(VMConfig::toUIString(model), static_cast<int>(model));
        }
    }
    sysLayout->addRow(i18n("CPU model:"), cpuCombo);

    cpuSpin = new QSpinBox();
    cpuSpin->setRange(1, 64);
    cpuSpin->setValue(2);
    cpuSpin->setSuffix(i18n(" Cores"));
    sysLayout->addRow(i18n("CPU:"), cpuSpin);

    kvmCheck = new QCheckBox(i18n("Enable KVM hardware acceleration"));
    kvmCheck->setChecked(true);
    ContextualHelp::addRow(sysLayout,
                           i18n("Acceleration:"),
                           kvmCheck,
                           i18n("Uses KVM for near-native performance. Disable for pure "
                                "software emulation (TCG), which is much slower but does "
                                "not require KVM and supports all CPU models."));

    // RAM
    auto *ramContainer = new QVBoxLayout();
    auto *ramSliderLayout = new QHBoxLayout();

    const QList<int> ramSteps = RamSpinBox::steps();

    ramSlider = new QSlider(Qt::Horizontal);
    ramSlider->setRange(0, ramSteps.size() - 1);
    ramSlider->setTickPosition(QSlider::TicksBelow);
    ramSlider->setTickInterval(1);

    ramSpin = new RamSpinBox();
    ramSpin->setValue(GlobalDefaults.ramMb);
    ramSliderLayout->addWidget(ramSlider);
    ramSliderLayout->addWidget(ramSpin);

    balloonCheck = new QCheckBox(i18n("VirtIO memory ballooning"));
    balloonCheck->setChecked(true);
    preallocCheck = new QCheckBox(i18n("Pre-allocate memory"));

    ramContainer->addLayout(ramSliderLayout);
    ramContainer->addLayout(ContextualHelp::beside(balloonCheck,
                                                   i18n("Allows the host system to dynamically adjust the VM's memory based "
                                                        "on its actual usage.\nEnable this to efficiently share memory "
                                                        "(RAM) among multiple virtual machines.")));
    ramContainer->addLayout(ContextualHelp::beside(preallocCheck,
                                                   i18n("Allocates the full amount of configured memory (RAM) immediately "
                                                        "when the VM starts.\nThis improves guest performance but strictly "
                                                        "reserves the memory on the host.")));
    sysLayout->addRow(i18n("Memory:"), ramContainer);

    addGroupGap();

    // The chipset, with ACPI, which mostly old guests on old chipsets go without.
    auto *chipsetLayout = new QVBoxLayout();
    machineCombo = new QComboBox();
    machineCombo->addItem(VMConfig::toUIString(VMSpecs::MachineType::Q35), static_cast<int>(VMSpecs::MachineType::Q35));
    machineCombo->addItem(VMConfig::toUIString(VMSpecs::MachineType::Pc), static_cast<int>(VMSpecs::MachineType::Pc));
    machineCombo->addItem(VMConfig::toUIString(VMSpecs::MachineType::PcISA), static_cast<int>(VMSpecs::MachineType::PcISA));
    chipsetLayout->addWidget(machineCombo);

    acpiCheck = new QCheckBox(i18n("Enable ACPI"));
    acpiCheck->setChecked(true);
    acpiCheck->setToolTip(
        i18n("Advanced Configuration and Power Interface. Disable for "
             "legacy guest OSes (e.g. Windows NT4, DOS) that do not "
             "support ACPI."));
    chipsetLayout->addWidget(acpiCheck);
    sysLayout->addRow(i18n("Chipset:"), chipsetLayout);

    // Firmware and TPM; Secure Boot sits below UEFI, which it needs.
    auto *bootLayout = new QVBoxLayout();
    uefiCheck = new QCheckBox(i18n("Use UEFI firmware (OVMF)"));
    uefiCheck->setToolTip(i18n("Required for Windows 11. Creates a separate VARS file."));
    bootLayout->addWidget(uefiCheck);

    secureBootCheck = new QCheckBox(i18n("Enable Secure Boot"));
    secureBootCheck->setToolTip(
        i18n("Requires UEFI and Q35 chipset. Uses OVMF "
             "firmware with enrolled Microsoft keys."));
    secureBootCheck->setEnabled(false); // Initially disabled until UEFI+Q35
    auto *secureBootLayout = new QHBoxLayout();
    secureBootLayout->addSpacing(style()->pixelMetric(QStyle::PM_IndicatorWidth) + style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing));
    secureBootLayout->addWidget(secureBootCheck);
    bootLayout->addLayout(secureBootLayout);

    tpmCheck = new QCheckBox(i18n("Emulate TPM 2.0 (swtpm)"));
    tpmCheck->setToolTip(i18n("Required for Windows 11. Uses swtpm as software TPM emulator."));
    bootLayout->addWidget(tpmCheck);
    tpmToolWarning = createInlineMessage(KMessageWidget::Warning);
    bootLayout->addWidget(tpmToolWarning);
    sysLayout->addRow(i18n("Boot and security:"), bootLayout);

    localTimeCheck = new QCheckBox(i18n("Use local time"));
    localTimeCheck->setToolTip(
        i18n("Sets the hardware clock to local time instead of UTC.\nThis should "
             "be set for Windows/DOS guests."));
    sysLayout->addRow(i18n("Clock:"), localTimeCheck);

    addGroupGap();

    exitActionCombo = new QComboBox(this);
    for (const auto action : {VMSpecs::ExitAction::SaveState, VMSpecs::ExitAction::Shutdown, VMSpecs::ExitAction::PowerOff}) {
        exitActionCombo->addItem(VMConfig::toUIString(action), QVariant::fromValue(action));
    }
    exitActionCombo->setToolTip(
        i18n("What to do with this machine if it is still running when KEMU quits. "
             "A confirmation dialog lets you override this."));
    sysLayout->addRow(i18n("When KEMU exits:"), exitActionCombo);

    guestShutdownActionCombo = new QComboBox(this);
    for (const auto action : {VMSpecs::GuestShutdownAction::None, VMSpecs::GuestShutdownAction::ShutdownHost}) {
        guestShutdownActionCombo->addItem(VMConfig::toUIString(action), QVariant::fromValue(action));
    }
    const auto &caps = SandboxCapabilities::instance();
    if (caps.has(SandboxCapabilities::HostPowerOff)) {
        guestShutdownActionCombo->setToolTip(
            i18n("Shuts down the host when the guest OS powers off. "
                 "Includes a cancellation countdown. Can be overridden per run in the VM menu."));
    } else {
        // Keep the stored value: the VM may move to a host where this works.
        guestShutdownActionCombo->setEnabled(false);
        guestShutdownActionCombo->setToolTip(caps.unavailableReason(SandboxCapabilities::HostPowerOff));
    }
    sysLayout->addRow(i18n("When the guest shuts down:"), guestShutdownActionCombo);

    tabs->addTab(sysTab, QIcon::fromTheme(u"preferences-system"_s), i18n("System"));

    // --- Hardware Tab ---
    // Storage comes first, across the full width for the disk list. Below,
    // graphics and display sit side by side: the number of monitors depends
    // on both.
    auto *hwTab = new QWidget();
    auto *hwMainLayout = new QVBoxLayout(hwTab);

    const auto createFormGroup = [](const QString &title) {
        auto *group = new QGroupBox(title);
        auto *layout = new QFormLayout(group);
        layout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        return std::pair{group, layout};
    };

    // Storage
    const auto [storageGroup, storageLayout] = createFormGroup(i18n("Storage"));

    // The system disk first, then the further ones.
    auto *extraDisksWidget = new QWidget();
    auto *extraDisksLayout = new QVBoxLayout(extraDisksWidget);
    extraDisksLayout->setContentsMargins(0, 0, 0, 0);

    diskTree = new QTreeWidget();
    diskTree->setHeaderLabels({i18n("Name"), i18n("Size"), i18n("Controller")});
    diskTree->setRootIsDecorated(false);
    diskTree->setUniformRowHeights(true);
    diskTree->setSelectionMode(QAbstractItemView::SingleSelection);
    diskTree->setMaximumHeight(diskTree->fontMetrics().height() * 8);
    diskTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    diskTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    diskTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    extraDisksLayout->addWidget(diskTree);

    auto *diskButtonsLayout = new QHBoxLayout();
    addDiskButton = new QPushButton(QIcon::fromTheme(u"list-add"_s), i18n("Add Disk…"));
    editDiskButton = new QPushButton(QIcon::fromTheme(u"document-properties"_s), i18n("Properties…"));
    removeDiskButton = new QPushButton(QIcon::fromTheme(u"list-remove"_s), i18n("Remove"));
    diskButtonsLayout->addWidget(addDiskButton);
    diskButtonsLayout->addWidget(editDiskButton);
    diskButtonsLayout->addWidget(removeDiskButton);
    diskButtonsLayout->addStretch();
    extraDisksLayout->addLayout(diskButtonsLayout);

    storageWarningMessage = createInlineMessage(KMessageWidget::Error);
    extraDisksLayout->addWidget(storageWarningMessage);

    storageLayout->addRow(extraDisksWidget);

    connect(addDiskButton, &QPushButton::clicked, this, &EditVmDialog::addDisk);
    connect(editDiskButton, &QPushButton::clicked, this, &EditVmDialog::editSelectedDisk);
    connect(removeDiskButton, &QPushButton::clicked, this, &EditVmDialog::removeSelectedDisk);
    connect(diskTree, &QTreeWidget::itemSelectionChanged, this, &EditVmDialog::updateDiskButtons);
    connect(diskTree, &QTreeWidget::itemDoubleClicked, this, &EditVmDialog::editSelectedDisk);

    hwMainLayout->addWidget(storageGroup);

    auto *hwColumns = new QHBoxLayout();
    auto *hwLeftColumn = new QVBoxLayout();
    auto *hwRightColumn = new QVBoxLayout();
    hwColumns->addLayout(hwLeftColumn, 1);
    hwColumns->addLayout(hwRightColumn, 1);
    hwMainLayout->addLayout(hwColumns);

    // Graphics
    const auto [graphicsGroup, graphicsLayout] = createFormGroup(i18n("Graphics"));

    gpuCombo = new QComboBox();
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::Vga), static_cast<int>(VMSpecs::GpuType::Vga));
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::Virtio), static_cast<int>(VMSpecs::GpuType::Virtio));
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::Qxl), static_cast<int>(VMSpecs::GpuType::Qxl));
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::CirrusGD5446), static_cast<int>(VMSpecs::GpuType::CirrusGD5446));
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::CirrusGD5430), static_cast<int>(VMSpecs::GpuType::CirrusGD5430));
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::VgaISA), static_cast<int>(VMSpecs::GpuType::VgaISA));
    graphicsLayout->addRow(i18n("Adapter:"), gpuCombo);

    // The adapter's VRAM. Typed sizes stay as typed where the adapter takes
    // them; the arrows step through powers of two, and the button next to it
    // offers those. With 3D, the host GPU provides it instead.
    gpuMemSpin = new PowerOfTwoSpinBox();
    gpuMemSpin->setSuffix(i18n(" MiB"));
    gpuMemSpin->setValue(GlobalDefaults.gpuMemMb);
    connect(gpuMemSpin, &QSpinBox::valueChanged, this, [this](int value) {
        if (!m_updatingMemory)
            m_vramMb = value;
    });
    connect(gpuMemSpin, &QSpinBox::editingFinished, this, [this]() {
        const auto gpu = static_cast<VMSpecs::GpuType>(gpuCombo->currentData().toInt());
        gpuMemSpin->setValue(VMConfig::normalizedVgaMemMb(gpu, gpuMemSpin->value()));
    });
    auto *vgaMemLabel = new QLabel(i18n("Video memory:"));
    vgaMemLabel->setBuddy(gpuMemSpin);
    graphicsLayout->addRow(vgaMemLabel,
                           ContextualHelp::beside(withSizeMenu(gpuMemSpin, &gpuMemPresetButton),
                                                  i18n("Specifies the amount of video memory for the virtual graphics adapter. If 3D acceleration is active, "
                                                       "this setting is ignored and the host GPU allocates memory dynamically.")));

    monitorCountSpin = new QSpinBox();
    monitorCountSpin->setRange(1, KEMUConst::SupportedMonitorCount);
    monitorCountSpin->setValue(GlobalDefaults.monitorCount);
    monitorCountSpin->setToolTip(
        i18n("Multiple monitors require VirtIO graphics. "
             "The current implementation supports up to two monitors."));
    graphicsLayout->addRow(i18n("Monitors:"), monitorCountSpin);
    monitorCountLabel = qobject_cast<QLabel *>(graphicsLayout->labelForField(monitorCountSpin));

    monitorMessage = createInlineMessage(KMessageWidget::Information);
    graphicsLayout->addRow({}, monitorMessage);

    hwLeftColumn->addWidget(graphicsGroup);

    // 3D acceleration in a group of its own, switched by the check box in its
    // title. Ordered by what the guest gets: its OpenGL driver, and Vulkan on
    // top. What they need comes along on its own, under "Advanced".
    accel3dGroup = new QGroupBox(i18n("3D acceleration"));
    accel3dGroup->setCheckable(true);
    auto *accelLayout = new QVBoxLayout(accel3dGroup);

    // One OpenGL driver or the other.
    accelLayout->addWidget(new QLabel(i18n("Guest OpenGL driver:")));
    universalRadio = new QRadioButton(i18n("Universal (virgl)"));
    nativeContextRadio = new QRadioButton(i18n("Native (native context)"));
    auto *openGlDriver = new QButtonGroup(this);
    openGlDriver->addButton(universalRadio);
    openGlDriver->addButton(nativeContextRadio);
    universalRadio->setChecked(true);

    // Help buttons rather than tool tips: the names alone say little.
    auto *openGlLayout = new QVBoxLayout();
    openGlLayout->addLayout(ContextualHelp::beside(universalRadio, universalHelp()));
    // Only says why, when it cannot be chosen.
    nativeContextDescription = createDescription();
    openGlLayout->addLayout(withDescription(nativeContextRadio, ContextualHelp::beside(nativeContextRadio, nativeContextHelp()), nativeContextDescription));
    // Set in under their heading, which they belong to.
    accelLayout->addLayout(indented(openGlLayout, optionTextIndent(universalRadio)));

    vulkanCheck = new QCheckBox(i18n("Vulkan (Venus)"));
    vulkanDescription = createDescription();
    accelLayout->addLayout(withDescription(vulkanCheck, ContextualHelp::beside(vulkanCheck, vulkanHelp()), vulkanDescription));

    // Rarely changed: Vulkan and the native context switch it on themselves.
    advanced3dPanel = new QWidget();
    auto *advancedLayout = new QVBoxLayout(advanced3dPanel);
    advancedLayout->setContentsMargins(0, 0, 0, 0);

    // Only says why, when it cannot be changed.
    hostBlobsCheck = new QCheckBox(i18n("Direct GPU memory access"));
    hostBlobsDescription = createDescription();
    advancedLayout->addLayout(withDescription(hostBlobsCheck, ContextualHelp::beside(hostBlobsCheck, hostBlobsHelp()), hostBlobsDescription));

    // The memory window of host blobs. A PCI BAR, so a power of two.
    hostmemSpin = new PowerOfTwoSpinBox();
    hostmemSpin->setSuffix(i18n(" MiB"));
    hostmemSpin->setRange(KEMUConst::VirglHostmemMinMb, KEMUConst::VirglHostmemMaxMb);
    hostmemSpin->setValue(GlobalDefaults.gpuHostmemMb);
    hostmemSpin->setToolTip(i18n("%1 MiB to %2 GiB", KEMUConst::VirglHostmemMinMb, KEMUConst::VirglHostmemMaxMb / 1024));
    connect(hostmemSpin, &QSpinBox::editingFinished, this, [this]() {
        hostmemSpin->setValue(static_cast<int>(std::bit_ceil(static_cast<unsigned>(hostmemSpin->value()))));
    });
    hostmemLabel = new QLabel(i18n("Memory limit:"));
    hostmemLabel->setBuddy(hostmemSpin);
    auto *hostmemRow = new QHBoxLayout();
    hostmemRow->addWidget(hostmemLabel);
    hostmemRow->addLayout(ContextualHelp::beside(withSizeMenu(hostmemSpin, &hostmemPresetButton),
                                                 i18n("Maximum host GPU memory the guest can access at once. Experimental Windows "
                                                      "drivers recommend 4 GiB.")),
                          1);
    advancedLayout->addLayout(indented(hostmemRow, optionTextIndent(hostBlobsCheck)));

    advanced3dButton = createDisclosure(i18nc("@action:button settings few need", "Advanced"), advanced3dPanel);
    accelLayout->addWidget(advanced3dButton, 0, Qt::AlignLeft);
    accelLayout->addLayout(indented(advanced3dPanel, optionTextIndent(hostBlobsCheck)));

    gpuMessage = createInlineMessage(KMessageWidget::Information);
    accelLayout->addWidget(gpuMessage);

    hwLeftColumn->addWidget(accel3dGroup);

    // Input and USB: mouse and keyboard depend on the USB controller.
    const auto [inputGroup, inputLayout] = createFormGroup(i18n("Input and USB"));

    usbCombo = new QComboBox();
    usbCombo->addItem(VMConfig::toUIString(VMSpecs::UsbController::Xhci), static_cast<int>(VMSpecs::UsbController::Xhci));
    usbCombo->addItem(VMConfig::toUIString(VMSpecs::UsbController::Ehci), static_cast<int>(VMSpecs::UsbController::Ehci));
    usbCombo->addItem(VMConfig::toUIString(VMSpecs::UsbController::Default), static_cast<int>(VMSpecs::UsbController::Default));
    usbCombo->addItem(VMConfig::toUIString(VMSpecs::UsbController::None), static_cast<int>(VMSpecs::UsbController::None));
    inputLayout->addRow(i18n("USB controller:"), usbCombo);
    usbControllerLabel = qobject_cast<QLabel *>(inputLayout->labelForField(usbCombo));

    mouseCombo = new QComboBox();
    mouseCombo->addItem(VMConfig::toUIString(VMSpecs::PointerDevice::Ps2), static_cast<int>(VMSpecs::PointerDevice::Ps2));
    mouseCombo->addItem(VMConfig::toUIString(VMSpecs::PointerDevice::UsbMouse), static_cast<int>(VMSpecs::PointerDevice::UsbMouse));
    mouseCombo->addItem(VMConfig::toUIString(VMSpecs::PointerDevice::UsbTablet), static_cast<int>(VMSpecs::PointerDevice::UsbTablet));
    mouseCombo->addItem(VMConfig::toUIString(VMSpecs::PointerDevice::VirtioTablet), static_cast<int>(VMSpecs::PointerDevice::VirtioTablet));
    setCombo(mouseCombo, GlobalDefaults.pointerDevice);
    ContextualHelp::addRow(inputLayout,
                           i18n("Mouse:"),
                           mouseCombo,
                           i18n("PS/2 and USB mouse are relative pointers; tablets give absolute "
                                "positioning (seamless, no grab). USB options need a USB controller, "
                                "VirtIO needs PCI and a virtio input driver in the guest."));

    keyboardCombo = new QComboBox();
    keyboardCombo->addItem(VMConfig::toUIString(VMSpecs::KeyboardDevice::Ps2), static_cast<int>(VMSpecs::KeyboardDevice::Ps2));
    keyboardCombo->addItem(VMConfig::toUIString(VMSpecs::KeyboardDevice::Usb), static_cast<int>(VMSpecs::KeyboardDevice::Usb));
    keyboardCombo->setToolTip(i18n("USB keyboard needs a USB controller. Legacy guests require PS/2."));
    setCombo(keyboardCombo, GlobalDefaults.keyboardDevice);
    inputLayout->addRow(i18n("Keyboard:"), keyboardCombo);

    hwRightColumn->addWidget(inputGroup);

    // Audio
    const auto [audioGroup, audioLayout] = createFormGroup(i18n("Audio"));

    audioCombo = new QComboBox();
    {
        const QMetaEnum metaEnum = QMetaEnum::fromType<VMSpecs::AudioDevice>();
        for (int i = 0; i < metaEnum.keyCount(); ++i) {
            auto model = static_cast<VMSpecs::AudioDevice>(metaEnum.value(i));
            audioCombo->addItem(VMConfig::toUIString(model), static_cast<int>(model));
        }
    }
    audioCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    audioCombo->setCurrentIndex(1);

    sb16ConfigButton = new QPushButton(QIcon::fromTheme(u"configure"_s), {});
    sb16ConfigButton->setToolTip(i18n("Configure SB16 ISA resources"));
    sb16ConfigButton->setFixedWidth(sb16ConfigButton->sizeHint().height());
    sb16ConfigButton->setEnabled(false);

    auto *audioRow = new QHBoxLayout();
    audioRow->addWidget(audioCombo);
    audioRow->addWidget(sb16ConfigButton);
    audioRow->addStretch();

    audioLayout->addRow(i18n("Device:"), audioRow);

    hwRightColumn->addWidget(audioGroup);

    hwLeftColumn->addStretch();
    hwRightColumn->addStretch();

    hwMainLayout->addStretch();

    tabs->addTab(hwTab, QIcon::fromTheme(u"preferences-desktop-peripherals"_s), i18n("Hardware"));

    // --- Network Tab ---
    auto *netTab = new QWidget();
    auto *netMainLayout = new QVBoxLayout(netTab);

    auto *netFormLayout = new QFormLayout();
    netFormLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    netCombo = new QComboBox();
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::None), static_cast<int>(VMSpecs::NetworkCard::None));
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::Virtio), static_cast<int>(VMSpecs::NetworkCard::Virtio));
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::E1000), static_cast<int>(VMSpecs::NetworkCard::E1000));
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::Rtl8139), static_cast<int>(VMSpecs::NetworkCard::Rtl8139));
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::Pcnet), static_cast<int>(VMSpecs::NetworkCard::Pcnet));
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::Rtl8029), static_cast<int>(VMSpecs::NetworkCard::Rtl8029));
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::Ne2000), static_cast<int>(VMSpecs::NetworkCard::Ne2000));
    netFormLayout->addRow(i18n("Network adapter:"), netCombo);

    networkModeCombo = new QComboBox();
    networkModeCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkMode::UserMode), static_cast<int>(VMSpecs::NetworkMode::UserMode));
    networkModeCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkMode::VirtualNetwork), static_cast<int>(VMSpecs::NetworkMode::VirtualNetwork));
    netFormLayout->addRow(i18n("Network mode:"), networkModeCombo);

    auto *macLayout = new QHBoxLayout();
    macEdit = new QLineEdit();
    macEdit->setPlaceholderText(u"52:54:00:xx:xx:xx"_s);
    macEdit->setClearButtonEnabled(false);

    regenerateMacButton = new QPushButton(QIcon::fromTheme(u"view-refresh"_s), {});
    regenerateMacButton->setToolTip(i18n("Generate a new random MAC address"));
    regenerateMacButton->setFixedWidth(regenerateMacButton->sizeHint().height());

    macLayout->addWidget(macEdit);
    macLayout->addWidget(regenerateMacButton);

    netFormLayout->addRow(i18n("MAC address:"), macLayout);

    macWarningMessage = createInlineMessage(KMessageWidget::Error);
    netFormLayout->addRow({}, macWarningMessage);

    netMainLayout->addLayout(netFormLayout);

    // --- User Mode Section (Port Forwarding) ---
    userModeGroup = new QGroupBox(i18n("Port Forwarding (Host → Guest)"));
    auto *pfLayout = new QVBoxLayout(userModeGroup);

    portForwardTable = new QTableWidget(0, 3);
    portForwardTable->setHorizontalHeaderLabels({i18n("Protocol"), i18n("Host Port"), i18n("Guest Port")});
    portForwardTable->horizontalHeader()->setStretchLastSection(true);
    portForwardTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    portForwardTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    portForwardTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    portForwardTable->verticalHeader()->setVisible(false);
    portForwardTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    portForwardTable->setSelectionMode(QAbstractItemView::SingleSelection);
    pfLayout->addWidget(portForwardTable);

    auto *pfButtonLayout = new QHBoxLayout();
    addPortFwdButton = new QPushButton(QIcon::fromTheme(u"list-add"_s), i18n("Add Rule"));
    removePortFwdButton = new QPushButton(QIcon::fromTheme(u"list-remove"_s), i18n("Remove"));
    removePortFwdButton->setEnabled(false);
    pfButtonLayout->addWidget(addPortFwdButton);
    pfButtonLayout->addWidget(removePortFwdButton);
    pfButtonLayout->addStretch();
    pfLayout->addLayout(pfButtonLayout);

    netMainLayout->addWidget(userModeGroup);

    // --- Virtual Network Section ---
    vdeGroup = new QGroupBox(i18n("Virtual Network"));
    auto *vdeLayout = new QFormLayout(vdeGroup);
    vdeLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    virtualNetworkCombo = new QComboBox();
    vdeLayout->addRow(i18n("Network:"), virtualNetworkCombo);

    hostnameEdit = new QLineEdit();
    hostnameEdit->setClearButtonEnabled(false);
    hostnameEdit->setToolTip(i18n("DNS hostname of this VM inside the virtual network (\"<hostname>.<DNS domain of the network>\")"));
    vdeLayout->addRow(i18n("Hostname:"), hostnameEdit);

    hostnameWarningMessage = createInlineMessage(KMessageWidget::Error);
    vdeLayout->addRow({}, hostnameWarningMessage);

    netMainLayout->addWidget(vdeGroup);

    userModeGroup->setVisible(true);
    vdeGroup->setVisible(false);

    netMainLayout->addStretch();

    tabs->addTab(netTab, QIcon::fromTheme(u"network-wired"_s), i18n("Network"));

    // --- Advanced Tab ---
    auto *advTab = new QWidget();
    auto *advMainLayout = new QVBoxLayout(advTab);
    auto *advLayout = new QFormLayout();
    advLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    extraArgsEdit = new QLineEdit();
    extraArgsEdit->setPlaceholderText(i18n("Additional QEMU parameters"));
    extraArgsEdit->setClearButtonEnabled(true);
    advLayout->addRow(i18n("Extra arguments:"), extraArgsEdit);

    guestAgentCheck = new QCheckBox(i18n("Connect the QEMU guest agent"));
    advLayout->addRow({},
                      ContextualHelp::beside(guestAgentCheck,
                                             i18n("Adds a channel for the QEMU guest agent, so KEMU can see whether it runs in the guest. "
                                                  "Linux guests need the package qemu-guest-agent, Windows guests get it with the virtio-win guest tools.")));
    advMainLayout->addLayout(advLayout);

    auto *identityGroup = new QGroupBox(i18n("System Identity (SMBIOS)"));
    auto *identityLayout = new QFormLayout(identityGroup);
    identityLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    // On top of all fields it is about: -uuid ends up in the SMBIOS as well.
    auto *identityHint = createInlineMessage(KMessageWidget::Information);
    identityHint->setText(i18n(
        "These values define the guest's hardware identity; modifying them on an existing setup may trigger Windows reactivation or break software licenses."));
    identityHint->show();
    identityLayout->addRow(identityHint);

    auto *uuidLayout = new QHBoxLayout();
    uuidEdit = new QLineEdit();
    uuidEdit->setPlaceholderText(m_mode == Mode::Edit ? i18n("Not set (all zeros)") : i18n("Assigned when the VM is created"));
    uuidEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(u"^\\{?[0-9A-Fa-f-]{0,36}\\}?$"_s), this));

    regenerateUuidButton = new QPushButton(QIcon::fromTheme(u"view-refresh"_s), {});
    regenerateUuidButton->setToolTip(i18n("Generate a new random UUID"));
    regenerateUuidButton->setFixedWidth(regenerateUuidButton->sizeHint().height());

    uuidLayout->addWidget(uuidEdit);
    uuidLayout->addWidget(regenerateUuidButton);
    identityLayout->addRow(i18n("UUID:"), uuidLayout);

    uuidWarningMessage = createInlineMessage(KMessageWidget::Error);
    identityLayout->addRow({}, uuidWarningMessage);

    const QList<std::pair<QString SmbiosSystemInfo::*, QString>> smbiosLabels = {
        {&SmbiosSystemInfo::manufacturer, i18n("Manufacturer:")},
        {&SmbiosSystemInfo::product, i18n("Product:")},
        {&SmbiosSystemInfo::version, i18n("Version:")},
        {&SmbiosSystemInfo::serial, i18n("Serial number:")},
        {&SmbiosSystemInfo::sku, i18n("SKU:")},
        {&SmbiosSystemInfo::family, i18n("Family:")},
    };
    const QRegularExpression smbiosRx(u"^[\\x20-\\x7e]*$"_s);
    for (const auto &[member, label] : smbiosLabels) {
        auto *edit = new QLineEdit();
        edit->setPlaceholderText(i18nc("@info:placeholder SMBIOS field", "QEMU default"));
        edit->setMaxLength(KEMUConst::MaxSmbiosStringLength);
        edit->setValidator(new QRegularExpressionValidator(smbiosRx, edit));
        identityLayout->addRow(label, edit);
        m_smbiosEdits.append({member, edit});
    }

    advMainLayout->addWidget(identityGroup);
    advMainLayout->addStretch();

    tabs->addTab(advTab, QIcon::fromTheme(u"configure"_s), i18n("Advanced"));

    mainLayout->addWidget(tabs);

    connect(nameEdit, &QLineEdit::textChanged, this, &EditVmDialog::onNameChanged);
    connect(osFamilyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::onOsFamilyChanged);
    connect(osFamilyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::updateToolWarnings);
    connect(tpmCheck, &QCheckBox::toggled, this, &EditVmDialog::updateToolWarnings);
    connect(sharedPathEdit, &QLineEdit::textChanged, this, &EditVmDialog::updateToolWarnings);
    connect(osNameCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::onOsNameChanged);
    connect(osNameCombo, &QComboBox::editTextChanged, this, [this]() {
        updateIconForOs();
        validateForm();
    });

    connect(isoPathRequester, &KUrlRequester::textChanged, this, [this](const QString &text) {
        const bool hasIso = !text.trimmed().isEmpty();
        bootFromIsoCheck->setEnabled(hasIso);
        bootFromIsoCheck->setChecked(hasIso);
    });

    connect(floppyPathRequester, &KUrlRequester::textChanged, this, [this]() {
        const bool hasFloppy = !floppyPathRequester->text().trimmed().isEmpty();
        bootFromFloppyCheck->setEnabled(hasFloppy);
        if (!hasFloppy)
            bootFromFloppyCheck->setChecked(false);
        if (hasFloppy)
            bootFromFloppyCheck->setChecked(true);
        validateForm();
    });

    connect(bootFromFloppyCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked)
            bootFromIsoCheck->setChecked(false);
    });
    connect(bootFromIsoCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked)
            bootFromFloppyCheck->setChecked(false);
    });

    connect(btnBrowseShare, &QPushButton::clicked, this, &EditVmDialog::browseSharedDir);
    connect(sharedPathEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        sharedNameEdit->setEnabled(!text.isEmpty());
        if (!text.isEmpty() && sharedNameEdit->text().isEmpty()) {
            QString folderName = QFileInfo(text).fileName();
            if (folderName.isEmpty()) {
                folderName = u"exchange"_s;
            }
            sharedNameEdit->setText(folderName);
        }
        validateForm();
    });
    connect(sharedNameEdit, &QLineEdit::textChanged, this, [this] {
        validateForm();
    });

    connect(kvmCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        auto *model = qobject_cast<QStandardItemModel *>(cpuCombo->model());
        if (!model)
            return;

        const int hostIdx = cpuCombo->findData(static_cast<int>(VMSpecs::CpuModel::Host));
        if (hostIdx >= 0) {
            QStandardItem *item = model->item(hostIdx);
            item->setEnabled(enabled);
            if (!enabled && cpuCombo->currentIndex() == hostIdx) {
                const int fallback = cpuCombo->findData(static_cast<int>(VMSpecs::CpuModel::Qemu64));
                if (fallback >= 0)
                    cpuCombo->setCurrentIndex(fallback);
            }
        }
    });

    connect(cpuCombo, &QComboBox::currentIndexChanged, this, [this]() {
        const auto selectedCpu = static_cast<VMSpecs::CpuModel>(cpuCombo->currentData().toInt());
        cpuSpin->setMaximum(VMConfig::maxCoresForModel(selectedCpu));

        const auto currentMachine = static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt());

        // Only switch the chipset automatically if the new CPU is incompatible with
        // the currently selected chipset.
        if (!VMConfig::isCpuCompatible(selectedCpu, currentMachine)) {
            const auto recommendedMachine = VMConfig::recommendedMachineType(selectedCpu);
            const int idx = machineCombo->findData(static_cast<int>(recommendedMachine));
            if (idx >= 0) {
                machineCombo->setCurrentIndex(idx);
            }
        }
    });

    connect(ramSlider, &QSlider::valueChanged, this, [this, ramSteps](int index) {
        if (index >= 0 && index < ramSteps.size()) {
            const QSignalBlocker blocker(ramSpin);
            ramSpin->setValue(ramSteps[index]);
        }
    });
    auto updateRamSlider = [this, ramSteps](int mb) {
        int bestIndex = 0;
        int minDiff = std::numeric_limits<int>::max();

        for (int i = 0; i < ramSteps.size(); ++i) {
            const int diff = std::abs(ramSteps[i] - mb);
            if (diff < minDiff) {
                minDiff = diff;
                bestIndex = i;
            }
        }
        const QSignalBlocker blocker(ramSlider);
        ramSlider->setValue(bestIndex);
    };
    connect(ramSpin, &RamSpinBox::valueChanged, this, updateRamSlider);

    connect(machineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::onMachineTypeChanged);
    connect(machineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::validateForm);

    connect(usbCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        updateInputDeviceAvailability();
    });

    auto updateSecureBootState = [this]() {
        const bool uefiOn = uefiCheck->isChecked();
        const bool q35 = (machineCombo->currentData().toInt() == static_cast<int>(VMSpecs::MachineType::Q35));
        secureBootCheck->setEnabled(uefiOn && q35);
        if (!uefiOn || !q35) {
            secureBootCheck->setChecked(false);
        }
    };
    connect(uefiCheck, &QCheckBox::toggled, this, updateSecureBootState);
    connect(machineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateSecureBootState);
    connect(gpuCombo, &QComboBox::currentIndexChanged, this, &EditVmDialog::onGpuDisplayChanged);
    connect(accel3dGroup, &QGroupBox::toggled, this, &EditVmDialog::onGpuDisplayChanged);
    // Only a click is the user's choice; onGpuDisplayChanged() sets it, too.
    connect(hostBlobsCheck, &QCheckBox::clicked, this, [this](bool checked) {
        m_hostBlobsChoice = checked;
        onGpuDisplayChanged();
    });
    connect(vulkanCheck, &QCheckBox::toggled, this, &EditVmDialog::onGpuDisplayChanged);
    // The other one of the pair toggles with it.
    connect(nativeContextRadio, &QRadioButton::toggled, this, &EditVmDialog::onGpuDisplayChanged);
    connect(monitorCountSpin, &QSpinBox::valueChanged, this, &EditVmDialog::onGpuDisplayChanged);
    // The hints depend on the OS family set on the General tab.
    connect(osFamilyCombo, &QComboBox::currentIndexChanged, this, &EditVmDialog::onGpuDisplayChanged);
    connect(audioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        sb16ConfigButton->setEnabled(static_cast<VMSpecs::AudioDevice>(audioCombo->currentData().toInt()) == VMSpecs::AudioDevice::Sb16);
    });
    connect(sb16ConfigButton, &QPushButton::clicked, this, [this]() {
        QDialog dlg(this);
        dlg.setWindowTitle(i18n("Sound Blaster 16 Configuration"));

        auto *form = new QFormLayout(&dlg);

        auto *irqCombo = new QComboBox;
        for (const int v : {2, 3, 5, 7, 9, 10, 11})
            irqCombo->addItem(QString::number(v), v);
        irqCombo->setCurrentIndex(irqCombo->findData(m_sb16Config.irq));
        form->addRow(i18n("IRQ:"), irqCombo);

        auto *dmaCombo = new QComboBox;
        for (const int v : {0, 1, 3})
            dmaCombo->addItem(QString::number(v), v);
        dmaCombo->setCurrentIndex(dmaCombo->findData(m_sb16Config.dma));
        form->addRow(i18n("DMA (8-bit):"), dmaCombo);

        auto *dma16Combo = new QComboBox;
        for (const int v : {5, 6, 7})
            dma16Combo->addItem(QString::number(v), v);
        dma16Combo->setCurrentIndex(dma16Combo->findData(m_sb16Config.dma16));
        form->addRow(i18n("DMA (16-bit):"), dma16Combo);

        auto *iobaseCombo = new QComboBox;
        for (const int v : {0x220, 0x240, 0x260, 0x280})
            iobaseCombo->addItem(u"0x%1"_s.arg(v, 0, 16), v);
        iobaseCombo->setCurrentIndex(iobaseCombo->findData(m_sb16Config.iobase));
        form->addRow(i18n("I/O base:"), iobaseCombo);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

        if (dlg.exec() == QDialog::Accepted) {
            m_sb16Config.irq = irqCombo->currentData().toInt();
            m_sb16Config.dma = dmaCombo->currentData().toInt();
            m_sb16Config.dma16 = dma16Combo->currentData().toInt();
            m_sb16Config.iobase = iobaseCombo->currentData().toInt();
        }
    });
    connect(addPortFwdButton, &QPushButton::clicked, this, &EditVmDialog::addPortForwardRow);
    connect(removePortFwdButton, &QPushButton::clicked, this, &EditVmDialog::removePortForwardRow);
    connect(portForwardTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        removePortFwdButton->setEnabled(portForwardTable->currentRow() >= 0);
    });
    connect(networkModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        auto mode = static_cast<VMSpecs::NetworkMode>(networkModeCombo->currentData().toInt());
        userModeGroup->setVisible(mode == VMSpecs::NetworkMode::UserMode);
        vdeGroup->setVisible(mode == VMSpecs::NetworkMode::VirtualNetwork);
        validateForm();
    });

    connect(virtualNetworkCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        validateForm();
    });

    connect(hostnameEdit, &QLineEdit::textEdited, this, [this]() {
        validateForm();
    });

    connect(netCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::updateNetworkCardDependencies);

    connect(regenerateMacButton, &QPushButton::clicked, this, [this]() {
        macEdit->setText(Utils::generateRandomMac(usedMacAddresses()));
    });

    connect(macEdit, &QLineEdit::textEdited, this, [this]() {
        validateForm();
    });

    connect(regenerateUuidButton, &QPushButton::clicked, this, [this]() {
        VMConfig scratch;
        scratch.regenerateUuid(m_vmUuids.values());
        uuidEdit->setText(scratch.uuid().toString(QUuid::WithoutBraces));
        validateForm();
    });
    connect(uuidEdit, &QLineEdit::textEdited, this, &EditVmDialog::validateForm);

    buttonBox = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults | QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttonBox->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, &EditVmDialog::resetToFactoryDefaults);
    mainLayout->addWidget(buttonBox);

    updateNetworkCardDependencies();

    onGpuDisplayChanged();
    updateDeviceAvailability();

    updateRamSlider(GlobalDefaults.ramMb);
}

void EditVmDialog::setConfig(const VMConfig &config)
{
    m_original = config;

    m_disks.clear();
    m_removedDisks.clear();
    for (const DiskConfig &disk : config.extraDisks())
        m_disks.append({disk, true, 0, {}});

    if (m_mode == Mode::Edit)
        setWindowTitle(i18n("Edit VM: %1", config.name()));

    nameEdit->setText(config.name());

    m_newSystemDiskGb = config.diskSizeGb();

    commentEdit->setPlainText(config.comment());

    setCombo(osFamilyCombo, config.osFamily());
    const int variantIdx = osNameCombo->findData(config.distroId());
    if (variantIdx >= 0)
        osNameCombo->setCurrentIndex(variantIdx);
    else if (!config.distroId().isEmpty())
        osNameCombo->setEditText(config.distroId());

    if (config.osFamily() != VMSpecs::OsFamily::Other) {
        osNameCombo->setEnabled(true);
        if (osNameLabel)
            osNameLabel->setEnabled(true);
    }

    // In PreCreate the icon was derived by NewVmDialog, so it should keep
    // following the operating system until the user picks one.
    iconSelector->loadFrom(config, m_mode == Mode::Edit);

    setCombo(cpuCombo, config.cpuModel());
    kvmCheck->setChecked(config.useKvm());
    cpuSpin->setValue(config.cpuCoreCount());
    ramSpin->setValue(config.ramMb());
    balloonCheck->setChecked(config.useMemoryBallooning());
    preallocCheck->setChecked(config.preallocMemory());
    isoPathRequester->setText(config.isoPath());
    bootFromIsoCheck->setChecked(config.bootFromIso());
    floppyPathRequester->setText(config.floppyPath());
    bootFromFloppyCheck->setChecked(config.bootFromFloppy());
    macEdit->setText(config.macAddress());

    uuidEdit->setText(config.uuid().isNull() ? QString() : config.uuid().toString(QUuid::WithoutBraces));
    const SmbiosSystemInfo smbios = config.smbiosSystemInfo();
    const bool smbiosSupported = VMConfig::supportsSmbios(config.architecture());
    for (const auto &[member, edit] : std::as_const(m_smbiosEdits)) {
        edit->setText(smbios.*member);
        edit->setEnabled(smbiosSupported);
        edit->setToolTip(smbiosSupported ? QString() : i18n("Not supported for this architecture."));
    }

    for (const auto &rule : config.portForwards()) {
        addPortForwardRow();
        const int row = portForwardTable->rowCount() - 1;

        auto *protocolCombo = qobject_cast<QComboBox *>(portForwardTable->cellWidget(row, 0));
        auto *hostPortSpin = qobject_cast<QSpinBox *>(portForwardTable->cellWidget(row, 1));
        auto *guestPortSpin = qobject_cast<QSpinBox *>(portForwardTable->cellWidget(row, 2));

        if (protocolCombo) {
            const int idx = protocolCombo->findData(static_cast<int>(rule.protocol));
            if (idx >= 0)
                protocolCombo->setCurrentIndex(idx);
        }
        if (hostPortSpin)
            hostPortSpin->setValue(rule.hostPort);
        if (guestPortSpin)
            guestPortSpin->setValue(rule.guestPort);
    }
    extraArgsEdit->setText(config.extraArgs());
    guestAgentCheck->setChecked(config.useGuestAgent());
    sharedPathEdit->setText(config.sharedPath());
    sharedNameEdit->setText(config.sharedName());

    setCombo(machineCombo, config.machineType());

    updateDeviceAvailability();

    if (!VMConfig::isGpuTypeAvailable(config.gpuType())) {
        const auto fallbackGpu = VMConfig::fallbackGpuType();
        KMessageBox::information(this,
                                 i18n("The configured graphics adapter '%1' is not available in this QEMU.\n"
                                      "KEMU will use '%2' instead.",
                                      VMConfig::toUIString(config.gpuType()),
                                      VMConfig::toUIString(fallbackGpu)),
                                 i18n("Graphics Adapter Unavailable"));
        setCombo(gpuCombo, fallbackGpu);
    } else {
        setCombo(gpuCombo, config.gpuType());
    }
    monitorCountSpin->setValue(config.monitorCount());
    accel3dGroup->setChecked(config.gpu3dAcceleration());
    m_hostBlobsChoice = config.gpuHostBlobs();
    hostmemSpin->setValue(config.gpuHostmemMb());
    vulkanCheck->setChecked(config.gpuVulkan());
    (config.gpuNativeContext() ? nativeContextRadio : universalRadio)->setChecked(true);
    // Open where it holds a choice of the user's own.
    const bool hostBlobsNeeded = config.gpuVulkan() || config.gpuNativeContext();
    setDisclosed(advanced3dButton,
                 advanced3dPanel,
                 (!hostBlobsNeeded && config.gpuHostBlobs() != GlobalDefaults.gpuHostBlobs) || config.gpuHostmemMb() != GlobalDefaults.gpuHostmemMb);
    setCombo(audioCombo, config.audioDevice());
    m_sb16Config = config.sb16Config();
    setCombo(netCombo, config.networkCard());
    updateNetworkCardDependencies();
    setCombo(networkModeCombo, config.networkMode());
    if (virtualNetworkCombo->count() > 0) {
        const int vnIdx = virtualNetworkCombo->findData(config.virtualNetworkName());
        if (vnIdx >= 0)
            virtualNetworkCombo->setCurrentIndex(vnIdx);
    }
    m_lastAutoHostname = Utils::vmNameToHostname(config.name());
    hostnameEdit->setText(config.dnsHostname().isEmpty() ? m_lastAutoHostname : config.dnsHostname());
    setSystemDiskInterface(config.diskInterface());
    setCombo(usbCombo, config.usbController());
    setCombo(mouseCombo, config.pointerDevice());
    setCombo(keyboardCombo, config.keyboardDevice());

    setCombo(exitActionCombo, config.exitAction());
    applyEncryptionConstraints();
    refreshEncryptionState();
    updateToolWarnings();
    setCombo(guestShutdownActionCombo, config.guestShutdownAction());

    uefiCheck->setChecked(config.useUefi());
    localTimeCheck->setChecked(config.useLocalTime());
    tpmCheck->setChecked(config.useTpm());
    secureBootCheck->setChecked(config.useSecureBoot());
    acpiCheck->setChecked(config.useAcpi());
    // As QEMU will get it: within the adapter's range, Cirrus snapped.
    m_vramMb = VMConfig::normalizedVgaMemMb(static_cast<VMSpecs::GpuType>(gpuCombo->currentData().toInt()), config.gpuMemMb());
    onGpuDisplayChanged();

    applyModeConstraints();
    refreshDiskList();
}

void EditVmDialog::showSetting(Setting setting, int diskIndex)
{
    QWidget *field = nullptr;
    switch (setting) {
    case Setting::General:
        field = nameEdit;
        break;
    case Setting::Cores:
        field = cpuSpin;
        break;
    case Setting::Memory:
        field = ramSpin;
        break;
    case Setting::Chipset:
        field = machineCombo;
        break;
    case Setting::Firmware:
        field = uefiCheck;
        break;
    case Setting::Disk:
        field = diskTree;
        if (QTreeWidgetItem *item = diskTree->topLevelItem(diskIndex))
            diskTree->setCurrentItem(item);
        break;
    case Setting::Display:
        field = gpuCombo;
        break;
    case Setting::Sound:
        field = audioCombo;
        break;
    case Setting::Usb:
        field = usbCombo;
        break;
    case Setting::Network:
        field = netCombo;
        break;
    case Setting::PortForwarding:
        field = portForwardTable;
        break;
    case Setting::SharedFolder:
        field = sharedPathEdit;
        break;
    }
    if (!field)
        return;

    for (int i = 0; i < m_tabs->count(); ++i) {
        if (m_tabs->widget(i)->isAncestorOf(field)) {
            m_tabs->setCurrentIndex(i);
            break;
        }
    }
    // Takes effect when the dialog shows; a read-only field shows without it.
    field->setFocus(Qt::OtherFocusReason);
}

void EditVmDialog::setMinimumSystemDiskGb(int sizeGb)
{
    m_minSystemDiskGb = sizeGb;
}

void EditVmDialog::setReadOnly(bool readOnly, bool frozen)
{
    m_readOnly = readOnly;
    if (!readOnly)
        return;

    makeReadOnly(this);

    // A button, but the icon it shows is a setting like the others.
    iconSelector->setEnabled(true);
    InputLock::apply(iconSelector);

    // setStandardButtons() replaces the buttons the walk above disabled.
    buttonBox->setStandardButtons(QDialogButtonBox::Close);
    buttonBox->setEnabled(true);

    if (!m_original.name().isEmpty()) {
        setWindowTitle(frozen ? i18n("Specification of VM: %1 (frozen)", m_original.name()) : i18n("Specification of VM: %1 (running)", m_original.name()));
    }

    // Says at once that nothing here can be changed, and why.
    auto *notice = createInlineMessage(KMessageWidget::Information);
    notice->setIcon(QIcon::fromTheme(u"object-locked"_s));
    notice->setText(frozen ? i18n("Linked clones are based on the disks of this virtual machine, so its settings are locked.")
                           : i18n("This virtual machine is running, so its settings are locked."));
    if (auto *box = qobject_cast<QBoxLayout *>(layout()))
        box->insertWidget(0, notice);
    notice->show();
}

void EditVmDialog::onMachineTypeChanged(int /*index*/)
{
    updateDeviceAvailability();
}

void EditVmDialog::onRamSpinChanged(int mb)
{
    const QSignalBlocker blockRAMSlider(ramSlider);
    ramSlider->setValue(mb / 1024);
}

void EditVmDialog::browseSharedDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, i18n("Select shared folder"), QDir::homePath());
    if (!dir.isEmpty())
        sharedPathEdit->setText(dir);
}

void EditVmDialog::onOsFamilyChanged(int /*index*/)
{
    const int familyVal = osFamilyValue();

    if (familyVal == -1) {
        // No family yet: nothing to choose from.
        osNameCombo->clear();
        osNameCombo->setEnabled(false);
        osNameLabel->setEnabled(false);

    } else if (static_cast<VMSpecs::OsFamily>(familyVal) == VMSpecs::OsFamily::Other) {
        // special case "Other"
        osNameCombo->clear();
        osNameCombo->addItem(i18n("Other"), u"other"_s);
        osNameCombo->setCurrentIndex(0);
        osNameCombo->setEnabled(false);
        osNameLabel->setEnabled(false);

    } else {
        osNameCombo->setEnabled(true);
        osNameLabel->setEnabled(true);
        auto osFamily = static_cast<VMSpecs::OsFamily>(familyVal);
        filterOsVariants(osFamily);
    }

    updateIconForOs();
    validateForm();
}

void EditVmDialog::onOsNameChanged(int /*index*/)
{
    updateIconForOs();

    const QString distroId = osNameCombo->currentData().toString();

    validateForm();
}

void EditVmDialog::onGpuDisplayChanged()
{
    const auto gpu = static_cast<VMSpecs::GpuType>(gpuCombo->currentData().toInt());
    const auto osFamily = static_cast<VMSpecs::OsFamily>(osFamilyValue());

    const bool isVirtio = (gpu == VMSpecs::GpuType::Virtio);
    const bool isWindows = (osFamily == VMSpecs::OsFamily::Windows);

    const bool multiheadAvailable = isVirtio;
    monitorCountSpin->setEnabled(multiheadAvailable);
    monitorCountLabel->setEnabled(multiheadAvailable);
    // The Windows virtio-gpu driver drives a single output. Elsewhere only
    // the guest knows how it arranged the monitors; kemu-agent tells KEMU on
    // Linux.
    QString monitorText;
    QString monitorToolTip;
    if (multiheadAvailable && monitorCountSpin->value() > 1) {
        if (isWindows) {
            monitorText = i18n("Multiple monitor support is experimental. Windows guests currently support only a single monitor.");
        } else if (osFamily == VMSpecs::OsFamily::Linux) {
            monitorText = i18n("Multiple monitor support is experimental. For accurate pointer tracking, install kemu-agent in the guest.");
            monitorToolTip = i18n(
                "When using multiple monitors, kemu-agent reports the monitor arrangement of the guest, so the pointer lands where it "
                "should. Without it, KEMU estimates the guest display layout.");
        } else {
            monitorText = i18n("Multiple monitor support is experimental. KEMU estimates how the guest arranged the monitors.");
        }
    }
    monitorMessage->setToolTip(monitorToolTip);
    updateInlineMessage(monitorMessage, monitorText, KMessageWidget::Information);

    // Unchecked, the group disables what it holds; the rules below come on
    // top. They leave the choices alone while 3D is off, so that they return
    // with it.
    const bool can3d = isVirtio && VMConfig::isGpu3dAvailable();
    accel3dGroup->setEnabled(can3d);
    accel3dGroup->setToolTip(!isVirtio ? i18n("Requires VirtIO graphics.") : VMConfig::isGpu3dAvailable() ? accel3dToolTip() : missingInQemuText());
    if (!can3d)
        accel3dGroup->setChecked(false);
    const bool with3d = accel3dGroup->isChecked();

    // The rules of QemuCommandBuilder::hostBlobsAvailable(),
    // nativeContextAvailable() and venusAvailable(). An option that cannot be
    // changed says why in the line below it.
    const bool hostBlobs = QemuCommandBuilder::hostBlobsAvailable();

    QString nativeContextMissing;
    if (isWindows)
        nativeContextMissing = i18n("Linux guests only."); // no Mesa driver to run
    else if (!hostBlobs)
        nativeContextMissing = hostBlobsMissingText();
    else if (!QemuCommandBuilder::nativeContextAvailable())
        nativeContextMissing = missingInQemuText();
    nativeContextRadio->setEnabled(nativeContextMissing.isEmpty());
    if (!nativeContextMissing.isEmpty())
        universalRadio->setChecked(true);
    describe(nativeContextRadio, nativeContextDescription, {nativeContextMissing});
    const bool nativeContext = nativeContextRadio->isChecked();

    const bool venus = QemuCommandBuilder::venusAvailable();
    vulkanCheck->setEnabled(venus);
    if (!venus)
        vulkanCheck->setChecked(false);
    QStringList vulkanText{i18n("For games and Proton.")};
    if (!venus) {
        vulkanText << hostBlobsMissingText();
    } else if (SettingsDialog::qemuSeccompSandbox() && QemuCapabilities::instance().hasSeccompSandbox()) {
        // QemuCommandBuilder::sandboxArgs() leaves it out for Venus.
        vulkanText << i18n("Disables QEMU's seccomp sandbox for this VM.");
    }
    describe(vulkanCheck, vulkanDescription, vulkanText);
    const bool vulkan = vulkanCheck->isChecked();

    // On while Vulkan or the native context needs it, the user's choice
    // otherwise.
    const bool hostBlobsNeeded = nativeContext || vulkan;
    hostBlobsCheck->setEnabled(hostBlobs && !hostBlobsNeeded);
    hostBlobsCheck->setChecked(hostBlobs && (hostBlobsNeeded || m_hostBlobsChoice));
    QString hostBlobsLocked;
    if (!hostBlobs)
        hostBlobsLocked = hostBlobsMissingText();
    else if (nativeContext && vulkan)
        hostBlobsLocked = i18n("Required by Native and Vulkan.");
    else if (nativeContext)
        hostBlobsLocked = i18n("Required by Native.");
    else if (vulkan)
        hostBlobsLocked = i18n("Required by Vulkan.");
    describe(hostBlobsCheck, hostBlobsDescription, {hostBlobsLocked});
    const bool blobs = hostBlobsCheck->isChecked();
    hostmemLabel->setEnabled(blobs);
    hostmemSpin->setEnabled(blobs);
    hostmemPresetButton->setEnabled(blobs);

    showVideoMemory(gpu, with3d);

    // A warning wins over information when both apply.
    QString hint;
    auto type = KMessageWidget::Information;
    if (with3d && isWindows) {
        hint = i18n("Windows has no official VirtIO 3D driver; only experimental community drivers exist.");
        type = KMessageWidget::Warning;
    } else if (with3d && blobs && !hostBlobsNeeded && osFamily != VMSpecs::OsFamily::Linux) {
        hint = i18n("Direct GPU memory access currently only benefits Linux guests.");
    }
    updateInlineMessage(gpuMessage, hint, type);
}

void EditVmDialog::showVideoMemory(VMSpecs::GpuType gpu, bool automatic)
{
    const QScopedValueRollback guard(m_updatingMemory, true);
    gpuMemSpin->setEnabled(!automatic);
    gpuMemPresetButton->setEnabled(!automatic);
    if (automatic) {
        // The special text shows while the value sits at the minimum.
        gpuMemSpin->setRange(0, 0);
        gpuMemSpin->setSpecialValueText(i18nc("@item video memory size", "Automatic"));
        gpuMemSpin->setToolTip(i18n("Managed automatically by the host GPU."));
        return;
    }
    gpuMemSpin->setSpecialValueText({});
    gpuMemSpin->setRange(VMConfig::vgaMemMbLowerLimit(gpu), VMConfig::vgaMemMbUpperLimit(gpu));
    gpuMemSpin->setValue(VMConfig::normalizedVgaMemMb(gpu, m_vramMb));
    const bool cirrus = gpu == VMSpecs::GpuType::CirrusGD5430 || gpu == VMSpecs::GpuType::CirrusGD5446;
    gpuMemSpin->setToolTip(cirrus ? i18n("4, 8 or 16 MiB") : i18n("%1 to %2 MiB", gpuMemSpin->minimum(), gpuMemSpin->maximum()));
}

void EditVmDialog::onNameChanged(const QString &text)
{
    // OS guessing
    bool found;
    auto [family, variantId] = VMConfig::guessOSFromText(text, &found);
    if (found) {
        const int familyIndex = osFamilyCombo->findData(static_cast<int>(family));
        if (familyIndex >= 0 && osFamilyCombo->currentIndex() != familyIndex)
            osFamilyCombo->setCurrentIndex(familyIndex);
        const int variantIndex = osNameCombo->findData(variantId);
        if (variantIndex >= 0 && osNameCombo->currentIndex() != variantIndex)
            osNameCombo->setCurrentIndex(variantIndex);
        updateIconForOs();
    }

    // Keep the DNS hostname in sync with the VM name as long as the user
    // has not changed it away from the auto-generated value.
    if (hostnameEdit && (hostnameEdit->text().isEmpty() || hostnameEdit->text() == m_lastAutoHostname)) {
        m_lastAutoHostname = Utils::vmNameToHostname(text);
        hostnameEdit->setText(m_lastAutoHostname);
    }

    validateForm();
}

void EditVmDialog::addPortForwardRow()
{
    const int row = portForwardTable->rowCount();
    portForwardTable->insertRow(row);

    auto *protocolCombo = new QComboBox();
    protocolCombo->addItem(u"TCP"_s, static_cast<int>(PortForwardRule::Protocol::Tcp));
    protocolCombo->addItem(u"UDP"_s, static_cast<int>(PortForwardRule::Protocol::Udp));
    portForwardTable->setCellWidget(row, 0, protocolCombo);

    auto *hostPortSpin = new QSpinBox();
    hostPortSpin->setRange(1, 65535);
    hostPortSpin->setValue(8080);
    hostPortSpin->setFrame(false);
    portForwardTable->setCellWidget(row, 1, hostPortSpin);

    auto *guestPortSpin = new QSpinBox();
    guestPortSpin->setRange(1, 65535);
    guestPortSpin->setValue(8080);
    guestPortSpin->setFrame(false);
    portForwardTable->setCellWidget(row, 2, guestPortSpin);

    portForwardTable->setCurrentCell(row, 0);
    removePortFwdButton->setEnabled(true);
}

void EditVmDialog::removePortForwardRow()
{
    const int row = portForwardTable->currentRow();
    if (row >= 0)
        portForwardTable->removeRow(row);
    removePortFwdButton->setEnabled(portForwardTable->rowCount() > 0);
}

void EditVmDialog::filterOsVariants(VMSpecs::OsFamily osFamily)
{
    osNameCombo->clear();
    osNameCombo->setCurrentIndex(-1);

    if (osNameCombo->lineEdit() && osNameCombo->lineEdit()->actions().isEmpty())
        osNameCombo->lineEdit()->addAction(QIcon::fromTheme(u"edit-find"_s), QLineEdit::LeadingPosition);

    if (osNameCombo->lineEdit()) {
        osNameCombo->lineEdit()->setPlaceholderText(i18n("Type or select"));
        osNameCombo->lineEdit()->clear();
    }

    if (m_imageDatabase) {
        for (const auto &d : m_imageDatabase->templatesForFamily(osFamily))
            osNameCombo->addItem(d.displayName, d.id);
    }

    if (auto *comp = osNameCombo->completer())
        comp->setCompletionMode(QCompleter::PopupCompletion);

    if (osNameCombo->count() > 0)
        osNameCombo->insertSeparator(osNameCombo->count());
    osNameCombo->addItem(i18n("Other"), u"other"_s);

    osNameCombo->setCurrentIndex(-1);
}

int EditVmDialog::osFamilyValue() const
{
    return osFamilyCombo->currentIndex() < 0 ? -1 : osFamilyCombo->currentData().toInt();
}

void EditVmDialog::updateIconForOs()
{
    QString distroId = osNameCombo->currentData().toString();
    if (distroId.isEmpty())
        distroId = osNameCombo->currentText().trimmed();

    iconSelector->followOs(static_cast<VMSpecs::OsFamily>(osFamilyValue()), distroId);
}

void EditVmDialog::setExistingVmNames(const QStringList &names)
{
    existingVmNames = names;
    onNameChanged(nameEdit->text());
}

void EditVmDialog::setMacUsageInfo(const QList<MacUsageInfo> &info)
{
    m_macUsageInfo = info;
}

void EditVmDialog::setVmUuids(const QMap<QString, QUuid> &vmUuids)
{
    m_vmUuids = vmUuids;
}

void EditVmDialog::setImageDatabase(ImageDatabase *db)
{
    m_imageDatabase = db;

    const int familyVal = osFamilyValue();
    if (familyVal >= 0 && static_cast<VMSpecs::OsFamily>(familyVal) != VMSpecs::OsFamily::Other)
        filterOsVariants(static_cast<VMSpecs::OsFamily>(familyVal));
}

void EditVmDialog::setNetworkManager(VirtualNetworkManager *mgr)
{
    m_networkManager = mgr;
    virtualNetworkCombo->clear();
    if (!mgr)
        return;
    for (const auto &net : mgr->networks())
        virtualNetworkCombo->addItem(net.name, net.name);

    // Virtual networks run in-process; they are unavailable only while no
    // network has been defined yet.
    const bool disableVirtualNetwork = mgr->networks().isEmpty();

    if (disableVirtualNetwork) {
        auto *model = qobject_cast<QStandardItemModel *>(networkModeCombo->model());
        if (model) {
            const int idx = networkModeCombo->findData(static_cast<int>(VMSpecs::NetworkMode::VirtualNetwork));
            if (idx >= 0) {
                QStandardItem *item = model->item(idx);
                item->setEnabled(false);
                item->setToolTip(i18n("Create a virtual network first in Settings"));
            }
        }
    }

    if (disableVirtualNetwork && static_cast<VMSpecs::NetworkMode>(networkModeCombo->currentData().toInt()) == VMSpecs::NetworkMode::VirtualNetwork) {
        const int userModeIdx = networkModeCombo->findData(static_cast<int>(VMSpecs::NetworkMode::UserMode));
        if (userModeIdx >= 0)
            networkModeCombo->setCurrentIndex(userModeIdx);
    }
}

bool EditVmDialog::iconManuallySet() const
{
    return iconSelector->isManuallySet();
}

VMConfig EditVmDialog::getResult() const
{
    VMConfig c = m_original;
    c.setName(nameEdit->text().trimmed());
    c.setComment(commentEdit->toPlainText());
    c.setOsFamily(static_cast<VMSpecs::OsFamily>(osFamilyValue()));
    QString distroId = osNameCombo->currentData().toString();
    if (distroId.isEmpty())
        distroId = osNameCombo->currentText().trimmed();
    c.setDistroId(distroId);

    iconSelector->applyTo(c);

    c.setCpuModel(static_cast<VMSpecs::CpuModel>(cpuCombo->currentData().toInt()));

    // Architecture: no dedicated selector yet -- currently only x86 vs x86_64.
    // The CPU model decides when it implies one (i486 and friends); otherwise
    // keep what the caller supplied, so an existing value is never silently
    // overwritten.
    const QString archHint = VMConfig::x86ArchHint(c.cpuModel());
    c.setArchitecture(archHint.isEmpty() ? m_original.architecture() : VMConfig::parseArchitecture(archHint));

    c.setUseKvm(kvmCheck->isChecked());
    c.setRamMb(ramSpin->value());
    c.setUseBalloon(balloonCheck->isChecked());
    c.setUsePrealloc(preallocCheck->isChecked());
    c.setCpuCoreCount(cpuSpin->value());
    // A larger, not yet applied value is not the size of the disk.
    c.setDiskSizeGb(m_mode == Mode::Edit && m_systemDiskGb > 0 ? m_systemDiskGb : m_newSystemDiskGb);
    c.setISOPath(isoPathRequester->text());
    c.setBootFromIso(bootFromIsoCheck->isChecked());
    c.setFloppyPath(floppyPathRequester->text());
    c.setBootFromFloppy(bootFromFloppyCheck->isChecked());
    c.setExtraArgs(extraArgsEdit->text());
    c.setUseGuestAgent(guestAgentCheck->isChecked());
    c.setMachineType(static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt()));
    c.setUseUefi(uefiCheck->isChecked());
    c.setUseLocalTime(localTimeCheck->isChecked());
    c.setExitAction(static_cast<VMSpecs::ExitAction>(exitActionCombo->currentData().toInt()));
    c.setGuestShutdownAction(static_cast<VMSpecs::GuestShutdownAction>(guestShutdownActionCombo->currentData().toInt()));
    c.setUseTpm(tpmCheck->isChecked());
    c.setUseSecureBoot(secureBootCheck->isChecked());
    c.setUseAcpi(acpiCheck->isChecked());
    c.setSharedPath(sharedPathEdit->text());
    c.setSharedName(sharedNameEdit->text());
    c.setGpuType(static_cast<VMSpecs::GpuType>(gpuCombo->currentData().toInt()));
    c.setMonitorCount(monitorCountSpin->value());
    c.setGpu3dAcceleration(accel3dGroup->isChecked());
    c.setGpuHostBlobs(hostBlobsCheck->isChecked());
    c.setGpuVulkan(vulkanCheck->isChecked());
    c.setGpuNativeContext(nativeContextRadio->isChecked());
    c.setGpuMemMb(VMConfig::normalizedVgaMemMb(c.gpuType(), m_vramMb));
    c.setGpuHostmemMb(hostmemSpin->value());
    c.setAudioDevice(static_cast<VMSpecs::AudioDevice>(audioCombo->currentData().toInt()));
    c.setSb16Config(m_sb16Config);
    c.setNetworkCard(static_cast<VMSpecs::NetworkCard>(netCombo->currentData().toInt()));
    c.setMacAddress(macEdit->text());
    c.setNetworkMode(static_cast<VMSpecs::NetworkMode>(networkModeCombo->currentData().toInt()));
    c.setVirtualNetworkName(virtualNetworkCombo->currentData().toString());
    if (static_cast<VMSpecs::NetworkMode>(networkModeCombo->currentData().toInt()) == VMSpecs::NetworkMode::VirtualNetwork) {
        // Store only explicit overrides; empty means "derive from the VM name".
        const QString hostname = hostnameEdit->text().trimmed().toLower();
        c.setDnsHostname(hostname == Utils::vmNameToHostname(nameEdit->text().trimmed()) ? QString() : hostname);
    }
    QList<PortForwardRule> rules;
    for (int row = 0; row < portForwardTable->rowCount(); ++row) {
        auto *protocolCombo = qobject_cast<QComboBox *>(portForwardTable->cellWidget(row, 0));
        auto *hostPortSpin = qobject_cast<QSpinBox *>(portForwardTable->cellWidget(row, 1));
        auto *guestPortSpin = qobject_cast<QSpinBox *>(portForwardTable->cellWidget(row, 2));
        if (!protocolCombo || !hostPortSpin || !guestPortSpin)
            continue;
        PortForwardRule rule;
        rule.protocol = static_cast<PortForwardRule::Protocol>(protocolCombo->currentData().toInt());
        rule.hostPort = static_cast<quint16>(hostPortSpin->value());
        rule.guestPort = static_cast<quint16>(guestPortSpin->value());
        rules.append(rule);
    }
    c.setPortForwards(rules);

    c.setUuid(QUuid::fromString(uuidEdit->text().trimmed()));
    // Not editable here, but the dialog must not drop it: a linked clone
    // would lose track of its source and unfreeze it.
    c.setLinkedSource(m_original.linkedSourceName(), m_original.linkedSourceUuid());
    c.setDiskKeyId(m_original.diskKeyId());
    SmbiosSystemInfo smbios;
    for (const auto &[member, edit] : m_smbiosEdits)
        smbios.*member = edit->text().trimmed();
    c.setSmbiosSystemInfo(smbios);

    c.setDiskInterface(m_systemDiskInterface);

    // Imported images join the configuration once their conversion is done.
    QList<DiskConfig> extraDisks;
    for (const DiskEntry &entry : m_disks) {
        if (entry.sourcePath.isEmpty())
            extraDisks << entry.disk;
    }
    c.setExtraDisks(extraDisks);

    c.setUsbController(static_cast<VMSpecs::UsbController>(usbCombo->currentData().toInt()));
    c.setPointerDevice(static_cast<VMSpecs::PointerDevice>(mouseCombo->currentData().toInt()));
    c.setKeyboardDevice(static_cast<VMSpecs::KeyboardDevice>(keyboardCombo->currentData().toInt()));

    return c;
}

void EditVmDialog::updateDeviceAvailability()
{
    const auto machine = static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt());
    const bool supportsIsa = VMConfig::supportsIsaBus(machine);
    const bool supportsPci = VMConfig::supportsPciBus(machine);

    // Requirement-based tooltip messages
    const QString isaReqMsg = i18n("Requires an ISA bus (e.g., Legacy PC or ISA-only).");
    const QString pciReqMsg = i18n("Requires a PCI/PCIe bus (e.g., Q35 or Legacy PC).");

    // Helper lambda to process ISA/PCI dependent combo boxes (Enable/Disable,
    // Tooltips, Fallback)
    auto updateCombo = [&](QComboBox *combo, auto isIsaChecker, int noneValue = -1) {
        auto *model = qobject_cast<QStandardItemModel *>(combo->model());
        if (!model)
            return;

        bool currentValid = false;
        int firstValidIdx = -1;

        for (int i = 0; i < combo->count(); ++i) {
            const int dataVal = combo->itemData(i).toInt();

            // Handle "None" or implicitly always-available items
            if (dataVal == noneValue) {
                model->item(i)->setEnabled(true);
                combo->setItemData(i, QString(), Qt::ToolTipRole);
                if (firstValidIdx == -1)
                    firstValidIdx = i;
                if (i == combo->currentIndex())
                    currentValid = true;
                continue;
            }

            const bool isIsa = isIsaChecker(dataVal);
            const bool shouldShow = (supportsIsa && isIsa) || (supportsPci && !isIsa);

            model->item(i)->setEnabled(shouldShow);

            if (shouldShow) {
                combo->setItemData(i, QString(), Qt::ToolTipRole);
                if (firstValidIdx == -1)
                    firstValidIdx = i;
                if (i == combo->currentIndex())
                    currentValid = true;
            } else {
                combo->setItemData(i, isIsa ? isaReqMsg : pciReqMsg, Qt::ToolTipRole);
            }
        }

        // Fallback to the first available item if current is disabled
        if (!currentValid && firstValidIdx != -1) {
            combo->setCurrentIndex(firstValidIdx);
        }
    };

    // --- Update CPU Availability ---
    if (auto *cpuModelItem = qobject_cast<QStandardItemModel *>(cpuCombo->model())) {
        bool currentCpuValid = false;
        int firstValidCpuIdx = -1;

        const QString cpuReqMsg = i18n("CPU is not compatible with the selected chipset.");

        for (int i = 0; i < cpuCombo->count(); ++i) {
            const auto cpuVal = static_cast<VMSpecs::CpuModel>(cpuCombo->itemData(i).toInt());
            bool shouldShow = VMConfig::isCpuCompatible(cpuVal, machine);

            // Respect KVM state for "Host" CPU
            if (cpuVal == VMSpecs::CpuModel::Host && !kvmCheck->isChecked()) {
                shouldShow = false;
            }

            cpuModelItem->item(i)->setEnabled(shouldShow);
            cpuCombo->setItemData(i, shouldShow ? QString() : cpuReqMsg, Qt::ToolTipRole);

            if (shouldShow) {
                if (firstValidCpuIdx == -1) {
                    firstValidCpuIdx = i;
                }
                if (i == cpuCombo->currentIndex()) {
                    currentCpuValid = true;
                }
            }
        }

        // Fallback if current CPU becomes invalid
        if (!currentCpuValid && firstValidCpuIdx != -1) {
            cpuCombo->setCurrentIndex(firstValidCpuIdx);
        }
    }

    // --- Update Standard Devices ---

    updateCombo(gpuCombo, [](int val) {
        return VMConfig::isIsaDevice(static_cast<VMSpecs::GpuType>(val));
    });

    // An adapter this QEMU lacks stays listed, greyed out with the reason.
    if (auto *gpuModel = qobject_cast<QStandardItemModel *>(gpuCombo->model())) {
        int firstUsable = -1;
        for (int i = 0; i < gpuCombo->count(); ++i) {
            if (!VMConfig::isGpuTypeAvailable(static_cast<VMSpecs::GpuType>(gpuCombo->itemData(i).toInt()))) {
                gpuModel->item(i)->setEnabled(false);
                gpuCombo->setItemData(i, missingInQemuText(), Qt::ToolTipRole);
            } else if (firstUsable == -1 && gpuModel->item(i)->isEnabled()) {
                firstUsable = i;
            }
        }
        const int current = gpuCombo->currentIndex();
        if (current >= 0 && !gpuModel->item(current)->isEnabled() && firstUsable != -1)
            gpuCombo->setCurrentIndex(firstUsable);
    }

    updateCombo(audioCombo, [](int val) {
        return VMConfig::isIsaDevice(static_cast<VMSpecs::AudioDevice>(val));
    });

    updateCombo(
        netCombo,
        [](int val) {
            return VMConfig::isIsaDevice(static_cast<VMSpecs::NetworkCard>(val));
        },
        static_cast<int>(VMSpecs::NetworkCard::None));

    // --- Audio Extras ---
    const auto currentAudio = static_cast<VMSpecs::AudioDevice>(audioCombo->currentData().toInt());
    sb16ConfigButton->setEnabled(currentAudio == VMSpecs::AudioDevice::Sb16 && supportsIsa);

    // --- USB (PCI only) ---
    if (auto *usbModel = qobject_cast<QStandardItemModel *>(usbCombo->model())) {
        for (int i = 0; i < usbCombo->count(); ++i) {
            auto usbCtrl = static_cast<VMSpecs::UsbController>(usbCombo->itemData(i).toInt());
            if (usbCtrl != VMSpecs::UsbController::None) {
                usbModel->item(i)->setEnabled(supportsPci);
                usbCombo->setItemData(i, supportsPci ? QString() : pciReqMsg, Qt::ToolTipRole);
            }
        }
        if (!supportsPci) {
            const int noneIdx = usbCombo->findData(static_cast<int>(VMSpecs::UsbController::None));
            if (noneIdx != -1)
                usbCombo->setCurrentIndex(noneIdx);
            usbCombo->setEnabled(false);
            usbControllerLabel->setEnabled(false);
        } else {
            usbCombo->setEnabled(true);
            usbControllerLabel->setEnabled(true);
        }
    }

    updateInputDeviceAvailability(); // keep mouse/keyboard combos in sync

    // --- Disk Interface ---
    setSystemDiskInterface(m_systemDiskInterface);

    // --- UEFI / TPM / Misc ---
    const bool canUseUefi = (machine == VMSpecs::MachineType::Q35);
    uefiCheck->setEnabled(canUseUefi);
    tpmCheck->setEnabled(canUseUefi);

    if (!canUseUefi) {
        uefiCheck->setChecked(false);
        tpmCheck->setChecked(false);
    }

    secureBootCheck->setEnabled(uefiCheck->isChecked() && canUseUefi);
    if (!secureBootCheck->isEnabled()) {
        secureBootCheck->setChecked(false);
    }

    if (!supportsPci) {
        balloonCheck->setChecked(false);
    }
    balloonCheck->setEnabled(supportsPci);
    // Kept as chosen; without PCI the channel is just left out.
    guestAgentCheck->setEnabled(supportsPci);

    const bool hasFloppy = (machine != VMSpecs::MachineType::Q35);
    floppyPathRequester->setEnabled(hasFloppy);
    floppyPathLabel->setEnabled(hasFloppy);
    bootFromFloppyCheck->setEnabled(hasFloppy && !floppyPathRequester->text().trimmed().isEmpty());
    if (!hasFloppy) {
        floppyPathRequester->clear();
        bootFromFloppyCheck->setChecked(false);
    }

    onGpuDisplayChanged();
}

void EditVmDialog::updateInputDeviceAvailability()
{
    const bool hasUsb = static_cast<VMSpecs::UsbController>(usbCombo->currentData().toInt()) != VMSpecs::UsbController::None;
    const auto machine = static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt());
    const bool hasPci = VMConfig::supportsPciBus(machine);

    const QString usbTip = i18n("Requires a USB controller.");
    const QString pciTip = i18n("Requires a PCI/PCIe bus (e.g., Q35 or Legacy PC).");

    const auto setItemState = [](QComboBox *combo, int enumValue, bool enabled, const QString &tip) {
        if (auto *model = qobject_cast<QStandardItemModel *>(combo->model())) {
            const int idx = combo->findData(enumValue);
            if (idx >= 0 && model->item(idx)) {
                model->item(idx)->setEnabled(enabled);
                combo->setItemData(idx, enabled ? QString() : tip, Qt::ToolTipRole);
            }
        }
    };

    setItemState(mouseCombo, static_cast<int>(VMSpecs::PointerDevice::UsbMouse), hasUsb, usbTip);
    setItemState(mouseCombo, static_cast<int>(VMSpecs::PointerDevice::UsbTablet), hasUsb, usbTip);
    setItemState(mouseCombo, static_cast<int>(VMSpecs::PointerDevice::VirtioTablet), hasPci, pciTip);
    setItemState(keyboardCombo, static_cast<int>(VMSpecs::KeyboardDevice::Usb), hasUsb, usbTip);

    // Fall back to PS/2 when the selected device is unavailable.
    const auto pointer = static_cast<VMSpecs::PointerDevice>(mouseCombo->currentData().toInt());
    const bool pointerOk = (pointer == VMSpecs::PointerDevice::Ps2) || (pointer == VMSpecs::PointerDevice::VirtioTablet ? hasPci : hasUsb);
    if (!pointerOk)
        setCombo(mouseCombo, VMSpecs::PointerDevice::Ps2);

    if (!hasUsb && static_cast<VMSpecs::KeyboardDevice>(keyboardCombo->currentData().toInt()) != VMSpecs::KeyboardDevice::Ps2)
        setCombo(keyboardCombo, VMSpecs::KeyboardDevice::Ps2);
}

void EditVmDialog::validateForm()
{
    QString warning;
    bool valid = true;

    QString nameError;
    const QString trimmed = nameEdit->text().trimmed();
    if (!VMConfig::isValidVmName(trimmed, &nameError)) {
        warning = nameError;
        valid = false;
    } else if (trimmed != m_original.name() && existingVmNames.contains(trimmed, Qt::CaseSensitive)) {
        warning = i18n("A virtual machine named '%1' already exists.", trimmed);
        valid = false;
    }

    // No OS family chosen yet?
    if (osFamilyValue() == -1)
        valid = false;

    // OS variant: accept combo selection OR free text (min. 2 chars)
    const QString osDistroId = osNameCombo->currentData().toString();
    const QString osText = osNameCombo->currentText().trimmed();
    if (osDistroId.isEmpty() && osText.length() < 2)
        valid = false;

    updateInlineMessage(nameWarningMessage, warning, KMessageWidget::Error);

    // Shared folder requires a mount tag
    if (valid && !sharedPathEdit->text().trimmed().isEmpty() && sharedNameEdit->text().trimmed().isEmpty()) {
        valid = false;
    }

    // Check for duplicate mac addresses
    QString macWarningText;
    auto macMessageType = KMessageWidget::Error;
    const QString mac = macEdit->text();
    if (!mac.isEmpty() && !Utils::isValidMacAddress(mac)) {
        macWarningText = i18n("Invalid MAC address format.");
        valid = false;
    } else if (!mac.isEmpty()) {
        const auto mode = static_cast<VMSpecs::NetworkMode>(networkModeCombo->currentData().toInt());
        const QString netName = virtualNetworkCombo->currentData().toString();

        // Every VM is checked: the first one holding this MAC may sit on
        // another network while a later one shares ours.
        for (const auto &info : m_macUsageInfo) {
            if (m_mode == Mode::Edit && info.vmName == m_original.name())
                continue;

            if (info.mac.compare(mac, Qt::CaseInsensitive) != 0)
                continue;

            if (mode == VMSpecs::NetworkMode::VirtualNetwork && info.networkMode == VMSpecs::NetworkMode::VirtualNetwork
                && info.virtualNetworkName == netName) {
                macWarningText = i18n("MAC conflict with '%1' on network '%2'.", info.vmName, netName);
                macMessageType = KMessageWidget::Error;
                valid = false;
                break;
            }

            // Duplicate elsewhere: harmless today, but it breaks as soon as
            // both VMs end up on the same network.
            if (macWarningText.isEmpty()) {
                macWarningText = i18n("MAC address is already used by '%1'.", info.vmName);
                macMessageType = KMessageWidget::Warning;
            }
        }
    }

    updateInlineMessage(macWarningMessage, macWarningText, macMessageType);

    // DNS hostname: only relevant (and visible) in VirtualNetwork mode.
    QString hostnameWarningText;
    if (static_cast<VMSpecs::NetworkMode>(networkModeCombo->currentData().toInt()) == VMSpecs::NetworkMode::VirtualNetwork) {
        const QString hostname = hostnameEdit->text().trimmed().toLower();
        if (hostname.isEmpty() || !Utils::isValidDnsHostname(hostname)) {
            hostnameWarningText = i18n("Invalid DNS hostname.");
            valid = false;
        } else {
            // Must be unique per virtual network.
            const QString netName = virtualNetworkCombo->currentData().toString();
            for (const auto &info : m_macUsageInfo) {
                if (m_mode == Mode::Edit && info.vmName == m_original.name())
                    continue;
                if (info.networkMode != VMSpecs::NetworkMode::VirtualNetwork || info.virtualNetworkName != netName)
                    continue;
                if (info.dnsHostname.compare(hostname, Qt::CaseInsensitive) == 0) {
                    hostnameWarningText = i18n("Hostname conflict with '%1' on network '%2'.", info.vmName, netName);
                    valid = false;
                    break;
                }
            }
        }
    }

    updateInlineMessage(hostnameWarningMessage, hostnameWarningText, KMessageWidget::Error);

    // A duplicate UUID only confuses guest software, so it does not block.
    QString uuidWarningText;
    auto uuidMessageType = KMessageWidget::Error;
    const QString uuidText = uuidEdit->text().trimmed();
    if (!uuidText.isEmpty()) {
        const QUuid uuid = QUuid::fromString(uuidText);
        if (uuid.isNull()) {
            uuidWarningText = i18n("Enter a valid, non-zero UUID or leave the field empty.");
            valid = false;
        } else {
            for (auto it = m_vmUuids.cbegin(); it != m_vmUuids.cend(); ++it) {
                if (it.value() == uuid && !(m_mode == Mode::Edit && it.key() == m_original.name())) {
                    uuidWarningText = i18n("This UUID is already used by '%1'.", it.key());
                    uuidMessageType = KMessageWidget::Warning;
                    break;
                }
            }
        }
    }
    updateInlineMessage(uuidWarningMessage, uuidWarningText, uuidMessageType);

    if (storageWarningMessage) {
        const QString storageError = storageLayoutError();
        updateInlineMessage(storageWarningMessage, storageError, KMessageWidget::Error);
        if (!storageError.isEmpty())
            valid = false;
    }

    if (auto *ok = buttonBox ? buttonBox->button(QDialogButtonBox::Ok) : nullptr)
        ok->setEnabled(valid);
}

void EditVmDialog::setVmDirectory(const QString &path, bool seedIsoPresent)
{
    m_vmDir = path;
    m_seedIsoPresent = seedIsoPresent;
    refreshDiskList();
}

QList<EditVmDialog::NewDisk> EditVmDialog::newDisks() const
{
    QList<NewDisk> result;
    for (const DiskEntry &entry : m_disks) {
        if (!entry.exists)
            result.append({entry.disk, entry.sizeGb, entry.sourcePath});
    }
    return result;
}

QList<EditVmDialog::RemovedDisk> EditVmDialog::removedDisks() const
{
    return m_removedDisks;
}

QString EditVmDialog::storageLayoutError() const
{
    VMConfig layout;
    layout.setMachineType(static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt()));
    layout.setDiskInterface(m_systemDiskInterface);

    QList<DiskConfig> disks;
    for (const DiskEntry &entry : m_disks)
        disks << entry.disk;
    layout.setExtraDisks(disks);

    return layout.storageLayoutError(m_seedIsoPresent);
}

QString EditVmDialog::nextFreeDiskFileName() const
{
    // Names of removed disks stay taken until the dialog is gone: their images
    // are only deleted or moved away after OK.
    QStringList taken;
    for (const DiskEntry &entry : m_disks)
        taken << entry.disk.fileName;
    for (const RemovedDisk &removed : m_removedDisks)
        taken << removed.fileName;
    if (!m_vmDir.isEmpty())
        taken << QDir(m_vmDir).entryList({u"disk-*"_s}, QDir::Files | QDir::Hidden);

    return VMConfig::nextExtraDiskFileName(taken);
}

void EditVmDialog::refreshDiskList()
{
    if (!diskTree)
        return;

    diskTree->clear();

    // Always the first row; the further disks follow in m_disks order.
    QString systemSize = i18n("%1 GB, new", m_newSystemDiskGb);
    if (m_mode == Mode::Edit) {
        const QemuImgProcess::ImageInfo info = systemDiskPath().isEmpty() ? QemuImgProcess::ImageInfo{} : QemuImgProcess::inspect(systemDiskPath());
        systemSize = info.isValid ? i18n("%1 GB", QString::number(info.virtualSizeGb(), 'f', 1)) : i18n("Missing");
    }
    auto *system = new QTreeWidgetItem(diskTree);
    system->setText(0, i18nc("@item name of the system disk", "System"));
    system->setText(1, systemSize);
    system->setText(2, VMConfig::toUIString(m_systemDiskInterface));
    updateSystemDiskItem();

    for (const DiskEntry &entry : std::as_const(m_disks)) {
        QString size;
        if (entry.exists && !m_vmDir.isEmpty()) {
            const QemuImgProcess::ImageInfo info = QemuImgProcess::inspect(QDir(m_vmDir).filePath(entry.disk.fileName));
            size = info.isValid ? i18n("%1 GB", QString::number(info.virtualSizeGb(), 'f', 1)) : i18n("Missing");
        } else if (!entry.sourcePath.isEmpty()) {
            const QemuImgProcess::ImageInfo info = QemuImgProcess::inspect(entry.sourcePath);
            size = info.isValid ? i18n("%1 GB, to be imported", QString::number(info.virtualSizeGb(), 'f', 1)) : i18n("To be imported");
        } else if (!entry.exists) {
            size = i18n("%1 GB, new", entry.sizeGb);
        }

        auto *item = new QTreeWidgetItem(diskTree);
        item->setIcon(0, QIcon::fromTheme(u"drive-harddisk"_s));
        item->setText(0, entry.disk.label.isEmpty() ? entry.disk.fileName : entry.disk.label);
        item->setToolTip(0, entry.disk.fileName);
        item->setText(1, size);
        item->setText(2, VMConfig::toUIString(entry.disk.diskInterface));
    }

    updateDiskButtons();
    validateForm();
}

int EditVmDialog::selectedDiskIndex() const
{
    QTreeWidgetItem *item = diskTree->currentItem();
    if (!item || !item->isSelected())
        return NoDiskSelected;
    // Row 0 is the system disk.
    const int row = diskTree->indexOfTopLevelItem(item);
    return row == 0 ? SystemDiskSelected : row - 1 < m_disks.size() ? row - 1 : NoDiskSelected;
}

void EditVmDialog::updateDiskButtons()
{
    if (!diskTree)
        return;

    const int selected = selectedDiskIndex();

    // A VM that does not exist yet has no folder to put further disks into.
    addDiskButton->setEnabled(!m_readOnly && m_mode == Mode::Edit && m_disks.size() < KEMUConst::MaxExtraDisks);
    addDiskButton->setToolTip(m_mode == Mode::Edit ? QString() : i18n("Further disks can be added once the virtual machine exists."));
    editDiskButton->setEnabled(!m_readOnly && selected != NoDiskSelected);
    removeDiskButton->setEnabled(!m_readOnly && selected >= 0);
    removeDiskButton->setToolTip(selected == SystemDiskSelected ? i18n("The system disk cannot be removed.") : QString());
}

void EditVmDialog::addDisk()
{
    if (m_readOnly || m_disks.size() >= KEMUConst::MaxExtraDisks)
        return;

    DiskDialog dlg(DiskDialog::Mode::Add, static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt()), this);
    dlg.setDiskInterface(m_systemDiskInterface);
    if (dlg.exec() != QDialog::Accepted)
        return;

    DiskEntry entry;
    entry.exists = false;
    entry.sizeGb = dlg.sizeGb();
    entry.sourcePath = dlg.sourcePath();
    entry.disk.fileName = nextFreeDiskFileName();
    entry.disk.label = dlg.label();
    entry.disk.diskInterface = dlg.diskInterface();
    entry.disk.serial = VMConfig::defaultDiskSerial(entry.disk.fileName);

    if (entry.disk.fileName.isEmpty())
        return;

    m_disks.append(entry);
    refreshDiskList();
}

void EditVmDialog::editSelectedDisk()
{
    const int row = selectedDiskIndex();
    if (m_readOnly || row == NoDiskSelected)
        return;
    if (row == SystemDiskSelected) {
        editSystemDisk();
        return;
    }

    DiskEntry &entry = m_disks[row];

    DiskDialog dlg(DiskDialog::Mode::Edit, static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt()), this);
    dlg.setLabel(entry.disk.label);
    dlg.setDiskInterface(entry.disk.diskInterface);
    if (m_mode == Mode::Edit && entry.exists && !m_vmDir.isEmpty()) {
        const QString diskPath = QDir(m_vmDir).filePath(entry.disk.fileName);
        const QemuImgProcess::ImageInfo info = QemuImgProcess::inspect(diskPath);
        dlg.setCurrentSize(info.isValid ? int((info.virtualSizeBytes + (1LL << 30) - 1) >> 30) : 0);
        dlg.setCompactAvailable(true);
        connect(&dlg, &DiskDialog::compactRequested, this, [this, diskPath, &dlg] {
            Q_EMIT compactDiskRequested(diskPath, &dlg);
        });
        connect(&dlg, &DiskDialog::expandRequested, this, [this, diskPath, &dlg](int sizeGb) {
            Q_EMIT expandDiskRequested(diskPath, sizeGb, &dlg);
        });
        m_openDiskDialog = &dlg;
        m_openDiskPath = diskPath;
    } else if (!entry.exists && entry.sourcePath.isEmpty()) {
        // Created on OK, so its size can still change freely.
        dlg.setNewDiskSize(entry.sizeGb);
    }
    const auto closed = qScopeGuard([this] {
        m_openDiskDialog = nullptr;
        m_openDiskPath.clear();
    });
    if (dlg.exec() != QDialog::Accepted)
        return;

    if (!entry.exists && entry.sourcePath.isEmpty())
        entry.sizeGb = dlg.sizeGb();

    entry.disk.label = dlg.label();
    entry.disk.diskInterface = dlg.diskInterface();
    refreshDiskList();
}

void EditVmDialog::editSystemDisk()
{
    const auto machine = static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt());
    DiskDialog dlg(DiskDialog::Mode::Edit, machine, this);
    dlg.setSystemDisk(static_cast<VMSpecs::OsFamily>(osFamilyValue()));
    dlg.setDiskInterface(m_systemDiskInterface);

    const QString diskPath = systemDiskPath();
    if (m_mode == Mode::Edit && !diskPath.isEmpty()) {
        // Expanding and compacting act at once, as for the other disks.
        dlg.setCurrentSize(m_systemDiskGb);
        dlg.setCompactAvailable(m_systemDiskGb > 0);
        connect(&dlg, &DiskDialog::compactRequested, this, [this, diskPath, &dlg] {
            Q_EMIT compactDiskRequested(diskPath, &dlg);
        });
        connect(&dlg, &DiskDialog::expandRequested, this, [this, diskPath, &dlg](int sizeGb) {
            Q_EMIT expandDiskRequested(diskPath, sizeGb, &dlg);
        });

        // A new VM chooses encryption when it is created.
        dlg.setEncryption(encryptionState());
        connect(&dlg, &DiskDialog::encryptRequested, this, [this, &dlg] {
            Q_EMIT encryptSystemDiskRequested(&dlg);
        });
        connect(&dlg, &DiskDialog::decryptRequested, this, [this, &dlg] {
            Q_EMIT decryptSystemDiskRequested(&dlg);
        });
        connect(&dlg, &DiskDialog::changePasswordRequested, this, [this, &dlg] {
            Q_EMIT changeDiskPasswordRequested(&dlg);
        });
        connect(&dlg, &DiskDialog::storePasswordRequested, this, [this, &dlg] {
            Q_EMIT storeDiskPasswordRequested(&dlg);
        });
        connect(&dlg, &DiskDialog::forgetPasswordRequested, this, [this, &dlg] {
            Q_EMIT forgetDiskPasswordRequested(&dlg);
        });
        m_openDiskDialog = &dlg;
        m_openDiskPath = diskPath;
    } else if (m_mode == Mode::PreCreate) {
        dlg.setNewDiskSize(m_newSystemDiskGb, m_minSystemDiskGb);
    }
    const auto closed = qScopeGuard([this] {
        m_openDiskDialog = nullptr;
        m_openDiskPath.clear();
    });
    if (dlg.exec() != QDialog::Accepted)
        return;

    if (m_mode == Mode::PreCreate)
        m_newSystemDiskGb = dlg.sizeGb();
    m_systemDiskInterface = dlg.diskInterface();
    refreshDiskList();
}

void EditVmDialog::setSystemDiskInterface(VMSpecs::DiskInterface diskInterface)
{
    // Without a PCI bus, only the IDE controller of the chipset is left.
    if (machineCombo && !VMConfig::supportsPciBus(static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt())))
        diskInterface = VMSpecs::DiskInterface::SataIde;
    if (diskInterface == m_systemDiskInterface)
        return;
    m_systemDiskInterface = diskInterface;
    refreshDiskList();
}

void EditVmDialog::removeSelectedDisk()
{
    const int row = selectedDiskIndex();
    if (m_readOnly || row < 0)
        return;

    const DiskEntry entry = m_disks.at(row);

    if (entry.exists) {
        const QString name = entry.disk.label.isEmpty() ? entry.disk.fileName : entry.disk.label;

        QString snapshotNote;
        if (!m_original.diskPath().isEmpty()) {
            const QList<SnapshotInfo> snapshots = SnapshotManager().listSnapshots(m_original.diskPath());
            if (std::any_of(snapshots.cbegin(), snapshots.cend(), [](const SnapshotInfo &snap) {
                    return snap.vmStateSizeBytes > 0;
                }))
                snapshotNote = u"<br/><br/>"_s + i18n("Snapshots with a saved running state can afterwards only be restored while the VM is shut down.");
        }

        // An overlay is useless without the source next to it.
        if (m_original.isLinkedClone()) {
            const auto answer = KMessageBox::warningTwoActions(this,
                                                               i18n("Remove the disk %1 from this virtual machine?<br/><br/>"
                                                                    "This image only contains changes specific to this linked clone "
                                                                    "and will be permanently deleted. This action takes effect when "
                                                                    "you save the settings.",
                                                                    name.toHtmlEscaped())
                                                                   + snapshotNote,
                                                               i18n("Remove Disk"),
                                                               KGuiItem(i18n("Delete Image"), u"edit-delete"_s),
                                                               KStandardGuiItem::cancel());
            if (answer != KMessageBox::PrimaryAction)
                return;
            m_removedDisks.append({entry.disk.fileName, {}});
            m_disks.removeAt(row);
            refreshDiskList();
            return;
        }

        const auto answer = KMessageBox::questionTwoActionsCancel(this,
                                                                  i18n("Remove the disk <b>%1</b> from this virtual machine?<br/><br/>"
                                                                       "Its image can be deleted or kept as a separate file. "
                                                                       "The action takes effect when you save the settings.",
                                                                       name.toHtmlEscaped())
                                                                      + snapshotNote,
                                                                  i18n("Remove Disk"),
                                                                  KGuiItem(i18n("Delete Image"), u"edit-delete"_s),
                                                                  KGuiItem(i18n("Keep Image File…"), u"document-save-as"_s));
        if (answer == KMessageBox::Cancel)
            return;

        RemovedDisk removed{entry.disk.fileName, {}};
        if (answer == KMessageBox::SecondaryAction) {
            const QString suggestion = QDir::home().filePath(u"%1-%2"_s.arg(m_original.name(), entry.disk.fileName));
            removed.keepPath = QFileDialog::getSaveFileName(this, i18n("Keep Disk Image As"), suggestion, i18n("QEMU Disk Images (*.qcow2)"));
            if (removed.keepPath.isEmpty())
                return;
            if (!m_vmDir.isEmpty() && QFileInfo(removed.keepPath).absoluteDir() == QDir(m_vmDir)) {
                KMessageBox::error(this, i18n("Please choose a location outside the folder of this virtual machine."), i18n("Remove Disk"));
                return;
            }
        }
        m_removedDisks.append(removed);
    }

    m_disks.removeAt(row);
    refreshDiskList();
}

void EditVmDialog::updateNetworkCardDependencies()
{
    const auto card = static_cast<VMSpecs::NetworkCard>(netCombo->currentData().toInt());
    const bool hasNetwork = (card != VMSpecs::NetworkCard::None);

    networkModeCombo->setEnabled(hasNetwork);
    macEdit->setEnabled(hasNetwork);
    regenerateMacButton->setEnabled(hasNetwork);
    userModeGroup->setEnabled(hasNetwork);
    vdeGroup->setEnabled(hasNetwork);
    portForwardTable->setEnabled(hasNetwork);
    addPortFwdButton->setEnabled(hasNetwork);

    if (hasNetwork && macEdit->text().isEmpty())
        macEdit->setText(Utils::generateRandomMac(usedMacAddresses()));

    validateForm();
}

void EditVmDialog::setDiskKeyId(const QString &keyId)
{
    m_original.setDiskKeyId(keyId);
    applyEncryptionConstraints();
    refreshEncryptionState();
}

void EditVmDialog::applyEncryptionConstraints()
{
    // The saved memory would be written unencrypted.
    auto *model = qobject_cast<QStandardItemModel *>(exitActionCombo->model());
    if (!model)
        return;
    const bool encrypted = m_original.isEncrypted();
    const int saveStateIndex = exitActionCombo->findData(QVariant::fromValue(VMSpecs::ExitAction::SaveState));
    if (QStandardItem *item = model->item(saveStateIndex)) {
        item->setEnabled(!encrypted);
        item->setToolTip(encrypted ? i18n("Not available for an encrypted system disk") : QString());
    }
    if (encrypted && exitActionCombo->currentIndex() == saveStateIndex)
        setCombo(exitActionCombo, VMSpecs::ExitAction::Shutdown);
}

void EditVmDialog::refreshEncryptionState()
{
    updateSystemDiskItem();
    if (m_openDiskDialog && !m_openDiskPath.isEmpty() && m_openDiskPath == systemDiskPath())
        m_openDiskDialog->setEncryption(encryptionState());
}

DiskDialog::Encryption EditVmDialog::encryptionState() const
{
    DiskDialog::Encryption state;
    state.encrypted = m_original.isEncrypted();
    state.walletAvailable = DiskKeyStore::isAvailable();
    state.passwordStored = state.encrypted && DiskKeyStore::hasKey(m_original.diskKeyId());

    // Changes move an existing TPM state to the new password.
    const bool tpmToolsMissing = !m_vmDir.isEmpty() && TpmState::encryptionOf(QDir(m_vmDir)) != TpmState::Encryption::NoState
        && !ExternalTools::instance().isAvailable({ExternalTools::Swtpm, ExternalTools::SwtpmIoctl});
    if (tpmToolsMissing)
        state.blockedReason = i18n(
            "Not available: Changing the TPM password requires 'swtpm' and 'swtpm_ioctl', which were not found. Install them or set their paths under "
            "Settings > Executables.");
    return state;
}

void EditVmDialog::updateSystemDiskItem()
{
    QTreeWidgetItem *system = diskTree ? diskTree->topLevelItem(0) : nullptr;
    if (!system)
        return;
    const bool encrypted = m_original.isEncrypted();
    const QIcon plain = QIcon::fromTheme(u"drive-harddisk"_s);
    system->setIcon(0, encrypted ? QIcon::fromTheme(u"drive-harddisk-encrypted"_s, plain) : plain);
    const QString tip = i18n("The disk the guest boots from. It cannot be removed.");
    system->setToolTip(0, encrypted ? tip + u'\n' + i18n("Encrypted") : tip);
}

void EditVmDialog::updateToolWarnings()
{
    // Settings stay usable, since the tool may be installed later; the VM
    // just does not start without it.
    const auto show = [](KMessageWidget *message, bool missing, const QString &text) {
        updateInlineMessage(message, missing ? text : QString(), KMessageWidget::Warning);
    };

    const ExternalTools &tools = ExternalTools::instance();
    show(tpmToolWarning,
         tpmCheck->isChecked() && !tools.isAvailable(ExternalTools::Swtpm),
         i18n("swtpm was not found. The virtual machine does not start until it is available."));

    const bool windows = static_cast<VMSpecs::OsFamily>(osFamilyValue()) == VMSpecs::OsFamily::Windows;
    show(shareToolWarning,
         windows && !sharedPathEdit->text().trimmed().isEmpty() && !tools.isAvailable(ExternalTools::Virtiofsd),
         i18n("virtiofsd was not found. Windows guests need it for shared folders. The virtual machine does not start until it is available."));
}

QString EditVmDialog::systemDiskPath() const
{
    if (m_vmDir.isEmpty())
        return {};
    return m_original.diskPaths(QDir(m_vmDir)).value(0);
}

void EditVmDialog::diskExpanded(const QString &diskPath, int sizeGb)
{
    if (diskPath == systemDiskPath())
        m_systemDiskGb = sizeGb;
    if (m_openDiskDialog && m_openDiskPath == diskPath)
        m_openDiskDialog->setCurrentSize(sizeGb);
    refreshDiskList();
}

void EditVmDialog::applyModeConstraints()
{
    if (m_mode != Mode::Edit)
        return;

    // The size of the image; it grows through the properties of the system disk.
    const QemuImgProcess::ImageInfo info = systemDiskPath().isEmpty() ? QemuImgProcess::ImageInfo{} : QemuImgProcess::inspect(systemDiskPath());
    m_systemDiskGb = info.isValid ? int((info.virtualSizeBytes + (1LL << 30) - 1) >> 30) : 0;

    // Changing these on an installed guest usually makes it unbootable.
    const QString warning = i18n("Changing this on an installed guest can make it unbootable.");
    machineCombo->setToolTip(warning);
    uefiCheck->setToolTip(warning);
    secureBootCheck->setToolTip(warning);
}

void EditVmDialog::resetToFactoryDefaults()
{
    const auto result = KMessageBox::questionTwoActions(this,
                                                        i18n("Are you sure you want to reset all settings to their default "
                                                             "values?"),
                                                        i18n("Reset to Defaults"),
                                                        KGuiItem(i18n("Reset"), QStringLiteral("edit-undo")),
                                                        KStandardGuiItem::cancel());

    if (result != KMessageBox::PrimaryAction) {
        return;
    }

    // VMConfig defaults from vmconfig.h
    ramSpin->setValue(GlobalDefaults.ramMb);
    cpuSpin->setValue(GlobalDefaults.cpuCoreCount);
    if (m_mode != Mode::Edit)
        m_newSystemDiskGb = qMax(GlobalDefaults.diskSizeGb, m_minSystemDiskGb);

    setCombo(cpuCombo, GlobalDefaults.cpuModel);
    kvmCheck->setChecked(GlobalDefaults.useKvm);
    setCombo(machineCombo, GlobalDefaults.machineType);
    setCombo(gpuCombo, GlobalDefaults.gpuType);
    monitorCountSpin->setValue(GlobalDefaults.monitorCount);
    m_vramMb = GlobalDefaults.gpuMemMb;
    hostmemSpin->setValue(GlobalDefaults.gpuHostmemMb);
    setCombo(audioCombo, GlobalDefaults.audioDevice);
    setSystemDiskInterface(GlobalDefaults.diskInterface);
    refreshDiskList(); // the size of a new system disk, too
    setCombo(netCombo, GlobalDefaults.networkCard);
    setCombo(usbCombo, GlobalDefaults.usbController);
    setCombo(mouseCombo, GlobalDefaults.pointerDevice);
    setCombo(keyboardCombo, GlobalDefaults.keyboardDevice);

    accel3dGroup->setChecked(GlobalDefaults.gpu3dAcceleration);
    m_hostBlobsChoice = GlobalDefaults.gpuHostBlobs;
    vulkanCheck->setChecked(GlobalDefaults.gpuVulkan);
    (GlobalDefaults.gpuNativeContext ? nativeContextRadio : universalRadio)->setChecked(true);
    onGpuDisplayChanged();

    uefiCheck->setChecked(GlobalDefaults.useUefi);
    tpmCheck->setChecked(GlobalDefaults.useTpm);
    secureBootCheck->setChecked(GlobalDefaults.useSecureBoot);
    acpiCheck->setChecked(GlobalDefaults.useAcpi);
    localTimeCheck->setChecked(GlobalDefaults.useLocalTime);
    setCombo(exitActionCombo, GlobalDefaults.exitAction);
    setCombo(guestShutdownActionCombo, GlobalDefaults.guestShutdownAction);

    m_sb16Config = Sb16Config();

    extraArgsEdit->clear();
    guestAgentCheck->setChecked(GlobalDefaults.useGuestAgent);
}

QStringList EditVmDialog::usedMacAddresses() const
{
    QStringList macs;
    for (const auto &info : m_macUsageInfo)
        macs << info.mac;
    return macs;
}
