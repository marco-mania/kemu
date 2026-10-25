/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "templatessettingspage.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <KLocalizedString>
#include <KMessageBox>

#include <algorithm>

#include "dialogs/cloudinittemplatestab.h"
#include "dialogs/templateeditdialog.h"
#include "utils/imagedatabase.h"
#include "utils/templatepackage.h"

using namespace Qt::StringLiterals;

namespace
{

constexpr int IdRole = Qt::UserRole;
const QString kPackageSuffix = QString::fromLatin1(TemplatesSettingsPage::PackageSuffix);

QString readText(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QString uniqueId(const QString &base, const QStringList &taken)
{
    QString id = base;
    for (int n = 2; taken.contains(id); ++n)
        id = u"%1-%2"_s.arg(base).arg(n);
    return id;
}

} // namespace

TemplatesSettingsPage::TemplatesSettingsPage(QWidget *parent)
    : QWidget(parent)
{
    auto *intro = new QLabel(i18n("OS templates are offered when you create a virtual machine. Your own templates and customized ones take "
                                  "precedence over downloaded and built-in templates."),
                             this);
    intro->setWordWrap(true);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({i18n("Name"), i18n("ID"), i18n("Status")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setRootIsDecorated(false);

    const auto makeButton = [this](const QString &icon, const QString &text, void (TemplatesSettingsPage::*slot)()) {
        auto *button = new QPushButton(QIcon::fromTheme(icon), text, this);
        connect(button, &QPushButton::clicked, this, slot);
        return button;
    };
    m_newButton = makeButton(u"list-add"_s, i18n("New…"), &TemplatesSettingsPage::newTemplate);
    m_duplicateButton = makeButton(u"edit-copy"_s, i18n("Duplicate…"), &TemplatesSettingsPage::duplicateTemplate);
    m_editButton = makeButton(u"document-edit"_s, i18n("Edit…"), &TemplatesSettingsPage::editTemplate);
    m_removeButton = makeButton(u"edit-delete"_s, i18n("Delete"), &TemplatesSettingsPage::removeTemplate);
    m_hideButton = makeButton(u"view-hidden"_s, i18n("Hide"), &TemplatesSettingsPage::toggleHidden);
    m_importButton = makeButton(u"document-import"_s, i18n("Import…"), &TemplatesSettingsPage::importTemplates);
    m_exportButton = makeButton(u"document-export"_s, i18n("Export…"), &TemplatesSettingsPage::exportTemplate);
    QPushButton *folderButton = makeButton(u"folder-open"_s, i18n("Open Folder"), &TemplatesSettingsPage::openFolder);
    folderButton->setToolTip(i18n("Open the folder with your own templates"));

    auto *buttons = new QVBoxLayout;
    for (QPushButton *button : {m_newButton, m_duplicateButton, m_editButton, m_removeButton, m_hideButton})
        buttons->addWidget(button);
    buttons->addSpacing(12);
    buttons->addWidget(m_importButton);
    buttons->addWidget(m_exportButton);
    buttons->addStretch();
    buttons->addWidget(folderButton);

    auto *row = new QHBoxLayout;
    row->addWidget(m_tree, 1);
    row->addLayout(buttons);

    // The OS templates on the first tab, the cloud-init templates on the second.
    auto *osTab = new QWidget(this);
    auto *osLayout = new QVBoxLayout(osTab);
    osLayout->addWidget(intro);
    osLayout->addLayout(row, 1);

    m_cloudInitTab = new CloudInitTemplatesTab(this);
    auto *tabs = new QTabWidget(this);
    tabs->addTab(osTab, i18n("OS Templates"));
    tabs->addTab(m_cloudInitTab, i18n("Cloud-Init Templates"));
    // OS template changes can bring cloud-init templates and change "Used By".
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int index) {
        if (tabs->widget(index) == m_cloudInitTab)
            m_cloudInitTab->refresh();
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tabs);

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &TemplatesSettingsPage::updateButtons);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this]() {
        if (selectedTemplate())
            editTemplate();
    });

    updateButtons();
}

void TemplatesSettingsPage::setImageDatabase(ImageDatabase *db)
{
    m_db = db;
    m_cloudInitTab->setImageDatabase(db);
    refresh();
}

void TemplatesSettingsPage::setPreviewConfig(const CloudDeployConfig &config)
{
    m_cloudInitTab->setPreviewConfig(config);
}

void TemplatesSettingsPage::refresh(const QString &selectId)
{
    m_tree->clear();
    if (!m_db) {
        updateButtons();
        return;
    }

    QList<TemplateInfo> templates = m_db->allTemplates();
    std::sort(templates.begin(), templates.end(), [](const TemplateInfo &a, const TemplateInfo &b) {
        return QString::localeAwareCompare(a.displayName, b.displayName) < 0;
    });

    const std::pair<TemplateOrigin, QString> groups[] = {
        {TemplateOrigin::User, i18n("Own Templates")},
        {TemplateOrigin::Downloaded, i18n("Downloaded Templates")},
        {TemplateOrigin::BuiltIn, i18n("Built-in Templates")},
    };
    const QColor hiddenColor = palette().color(QPalette::Disabled, QPalette::Text);
    QTreeWidgetItem *toSelect = nullptr;

    for (const auto &[origin, title] : groups) {
        auto *group = new QTreeWidgetItem({title});
        group->setFlags(Qt::ItemIsEnabled);
        QFont bold = group->font(0);
        bold.setBold(true);
        group->setFont(0, bold);

        for (const TemplateInfo &tpl : std::as_const(templates)) {
            if (tpl.origin != origin)
                continue;
            QStringList status;
            if (tpl.replaces == TemplateOrigin::BuiltIn)
                status << i18n("Customizes a built-in template");
            else if (tpl.replaces == TemplateOrigin::Downloaded)
                status << i18n("Customizes a downloaded template");
            if (tpl.hidden)
                status << i18n("Hidden");

            auto *item = new QTreeWidgetItem(group, {tpl.displayName, tpl.id, status.join(u", "_s)});
            item->setData(0, IdRole, tpl.id);
            item->setToolTip(0, tpl.sourcePath);
            if (tpl.hidden) {
                for (int column = 0; column < 3; ++column)
                    item->setForeground(column, hiddenColor);
            }
            if (tpl.id == selectId)
                toSelect = item;
        }

        if (group->childCount() > 0)
            m_tree->addTopLevelItem(group);
        else
            delete group;
    }

    m_tree->expandAll();
    if (toSelect) {
        m_tree->setCurrentItem(toSelect);
        m_tree->scrollToItem(toSelect);
    }
    updateButtons();
}

const TemplateInfo *TemplatesSettingsPage::selectedTemplate() const
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    if (!m_db || !item || !item->isSelected())
        return nullptr;
    const QString id = item->data(0, IdRole).toString();
    return id.isEmpty() ? nullptr : m_db->findById(id);
}

void TemplatesSettingsPage::updateButtons()
{
    const TemplateInfo *tpl = selectedTemplate();
    const bool own = tpl && tpl->origin == TemplateOrigin::User;

    m_newButton->setEnabled(m_db);
    m_importButton->setEnabled(m_db);
    for (QPushButton *button : {m_duplicateButton, m_editButton, m_hideButton, m_exportButton})
        button->setEnabled(tpl);

    m_editButton->setText(!tpl || own ? i18n("Edit…") : i18n("Customize…"));
    m_removeButton->setEnabled(own);
    const bool reset = own && tpl->replaces.has_value();
    m_removeButton->setText(reset ? i18n("Reset") : i18n("Delete"));
    m_removeButton->setIcon(QIcon::fromTheme(reset ? u"edit-undo"_s : u"edit-delete"_s));
    const bool hidden = tpl && tpl->hidden;
    m_hideButton->setText(hidden ? i18n("Show") : i18n("Hide"));
    m_hideButton->setIcon(QIcon::fromTheme(hidden ? u"view-visible"_s : u"view-hidden"_s));
    m_hideButton->setToolTip(i18n("Hidden templates are not offered for new virtual machines"));
}

QStringList TemplatesSettingsPage::allIds() const
{
    QStringList ids;
    if (m_db) {
        for (const TemplateInfo &tpl : m_db->allTemplates())
            ids << tpl.id;
    }
    return ids;
}

std::optional<QJsonObject> TemplatesSettingsPage::definitionOf(const TemplateInfo &tpl) const
{
    return TemplateStore::readTemplate(tpl.sourcePath, tpl.id);
}

void TemplatesSettingsPage::newTemplate()
{
    const QJsonObject skeleton{
        {u"id"_s, uniqueId(u"my-os"_s, allIds())},
        {u"display_name"_s, i18n("My Operating System")},
        {u"os_family"_s, u"linux"_s},
        {u"sort_order"_s, 500},
        {u"homepage"_s, QString()},
        {u"releases"_s, QJsonArray()},
        {u"qemu_defaults"_s, QJsonObject()},
    };
    TemplateEditDialog dialog(TemplateEditDialog::Mode::New, skeleton, allIds(), this);
    if (dialog.exec() == QDialog::Accepted)
        save(dialog.result());
}

void TemplatesSettingsPage::duplicateTemplate()
{
    const TemplateInfo *tpl = selectedTemplate();
    if (!tpl)
        return;
    auto copy = definitionOf(*tpl);
    if (!copy) {
        showError(i18n("The template '%1' could not be read from %2.", tpl->displayName, tpl->sourcePath));
        return;
    }
    (*copy)[u"id"_s] = uniqueId(tpl->id + u"-copy"_s, allIds());
    (*copy)[u"display_name"_s] = i18nc("@item name of a copied template", "%1 (Copy)", tpl->displayName);

    TemplateEditDialog dialog(TemplateEditDialog::Mode::New, *copy, allIds(), this);
    if (dialog.exec() == QDialog::Accepted)
        save(dialog.result());
}

void TemplatesSettingsPage::editTemplate()
{
    const TemplateInfo *tpl = selectedTemplate();
    if (!tpl)
        return;
    const auto definition = definitionOf(*tpl);
    if (!definition) {
        showError(i18n("The template '%1' could not be read from %2.", tpl->displayName, tpl->sourcePath));
        return;
    }

    const QString id = tpl->id;
    const bool own = tpl->origin == TemplateOrigin::User;
    QStringList taken = allIds();
    taken.removeAll(id);

    TemplateEditDialog dialog(own ? TemplateEditDialog::Mode::Edit : TemplateEditDialog::Mode::Customize, *definition, taken, this);
    if (dialog.exec() == QDialog::Accepted)
        save(dialog.result(), own ? id : QString());
}

void TemplatesSettingsPage::removeTemplate()
{
    const TemplateInfo *tpl = selectedTemplate();
    if (!tpl || tpl->origin != TemplateOrigin::User)
        return;

    const QString id = tpl->id;
    const bool reset = tpl->replaces.has_value();
    const auto answer = reset
        ? KMessageBox::warningTwoActions(this,
                                         i18n("Reset '%1'? Your changes are lost and the original template is used again.", tpl->displayName),
                                         i18n("Reset Template"),
                                         KGuiItem(i18n("Reset"), u"edit-undo"_s),
                                         KStandardGuiItem::cancel())
        : KMessageBox::warningTwoActions(this,
                                         i18n("Delete the template '%1'? Existing virtual machines are not affected.", tpl->displayName),
                                         i18n("Delete Template"),
                                         KStandardGuiItem::del(),
                                         KStandardGuiItem::cancel());
    if (answer != KMessageBox::PrimaryAction)
        return;

    QString error;
    if (!m_store.removeUserTemplate(id, &error))
        showError(error);
    m_db->load();
    refresh(reset ? id : QString());
}

void TemplatesSettingsPage::toggleHidden()
{
    const TemplateInfo *tpl = selectedTemplate();
    if (!tpl)
        return;
    const QString id = tpl->id;
    QString error;
    if (!m_store.setTemplateHidden(id, !tpl->hidden, &error))
        showError(error);
    m_db->load();
    refresh(id);
}

void TemplatesSettingsPage::exportTemplate()
{
    const TemplateInfo *tpl = selectedTemplate();
    if (!tpl)
        return;
    const auto definition = definitionOf(*tpl);
    if (!definition) {
        showError(i18n("The template '%1' could not be read from %2.", tpl->displayName, tpl->sourcePath));
        return;
    }

    QString path = QFileDialog::getSaveFileName(this,
                                                i18n("Export OS Template"),
                                                QDir::home().filePath(tpl->id + kPackageSuffix),
                                                i18n("KEMU template packages (*%1)", kPackageSuffix));
    if (path.isEmpty())
        return;
    if (!path.endsWith(kPackageSuffix)) {
        if (path.endsWith(u".tar.gz"_s))
            path.chop(7);
        path += kPackageSuffix;
    }

    // The cloud-init templates go along, so the package works on its own.
    TemplatePackage package;
    package.templates << *definition;
    for (const QString &cloudInitId : TemplateStore::cloudInitIds(*definition)) {
        const QString file = m_store.locateCloudInit(cloudInitId);
        if (!file.isEmpty())
            package.cloudInit.insert(cloudInitId, readText(file));
    }

    QString error;
    if (!package.write(path, &error))
        showError(error);
}

void TemplatesSettingsPage::importTemplates()
{
    const QString path =
        QFileDialog::getOpenFileName(this,
                                     i18n("Import OS Templates"),
                                     QDir::homePath(),
                                     i18n("KEMU template packages (*%1);;Archives and template files (*.tar.gz *.tgz *.zip *.json)", kPackageSuffix));
    if (path.isEmpty())
        return;

    importPackage(path);
}

void TemplatesSettingsPage::importPackage(const QString &path)
{
    QString error;
    const auto package = TemplatePackage::read(path, &error);
    if (!package) {
        showError(error);
        return;
    }

    QStringList imported;
    QStringList problems;
    QStringList cloudInitIds;

    for (const QJsonObject &tpl : package->templates) {
        const QString id = tpl.value(u"id"_s).toString();
        if (const QString invalid = TemplateStore::validationError(tpl); !invalid.isEmpty()) {
            problems << i18n("%1: %2", id.isEmpty() ? i18n("Template without id") : id, invalid);
            continue;
        }
        if (const TemplateInfo *existing = m_db->findById(id)) {
            const auto answer = KMessageBox::questionTwoActions(
                this,
                i18n("A template with the id '%1' (%2) exists already. Replace it with the imported one?", id, existing->displayName),
                i18n("Import OS Templates"),
                KGuiItem(i18n("Replace"), u"document-import"_s),
                KGuiItem(i18n("Skip"), u"go-next"_s));
            if (answer != KMessageBox::PrimaryAction)
                continue;
        }
        if (!m_store.saveUserTemplate(tpl, &error)) {
            problems << i18n("%1: %2", id, error);
            continue;
        }
        imported << id;
        for (const QString &cloudInitId : TemplateStore::cloudInitIds(tpl)) {
            if (!cloudInitIds.contains(cloudInitId))
                cloudInitIds << cloudInitId;
        }
    }

    // Only the cloud-init templates the imported templates use.
    for (const QString &cloudInitId : std::as_const(cloudInitIds)) {
        const auto text = package->cloudInit.constFind(cloudInitId);
        if (text == package->cloudInit.cend())
            continue;
        const QString current = m_store.locateCloudInit(cloudInitId);
        if (!current.isEmpty()) {
            if (readText(current) == *text)
                continue;
            // Other templates may refer to the same cloud-init template.
            const auto answer = KMessageBox::questionTwoActions(this,
                                                                i18n("The package brings its own version of the cloud-init template '%1', which "
                                                                     "other templates may use as well. Use the version from the package?",
                                                                     cloudInitId),
                                                                i18n("Import OS Templates"),
                                                                KGuiItem(i18n("Use Package Version"), u"document-import"_s),
                                                                KGuiItem(i18n("Keep Current"), u"go-next"_s));
            if (answer != KMessageBox::PrimaryAction)
                continue;
        }
        if (!m_store.saveUserCloudInit(cloudInitId, *text, &error))
            problems << i18n("%1: %2", cloudInitId, error);
    }

    m_db->load();
    refresh(imported.value(0));

    const QString summary =
        imported.isEmpty() ? i18n("No templates were imported.") : i18np("Imported one template.", "Imported %1 templates.", imported.size());
    if (problems.isEmpty())
        KMessageBox::information(this, summary, i18n("Import OS Templates"));
    else
        KMessageBox::informationList(this, summary + u' ' + i18n("Some entries were skipped:"), problems, i18n("Import OS Templates"));
}

void TemplatesSettingsPage::openFolder()
{
    const QString dir = m_store.userTemplateDir();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

bool TemplatesSettingsPage::save(const QJsonObject &tpl, const QString &replacedUserId)
{
    QString error;
    if (!m_store.saveUserTemplate(tpl, &error)) {
        showError(error);
        return false;
    }
    const QString id = tpl.value(u"id"_s).toString();
    if (!replacedUserId.isEmpty() && replacedUserId != id && !m_store.removeUserTemplate(replacedUserId, &error))
        showError(error);

    m_db->load();
    refresh(id);
    return true;
}

void TemplatesSettingsPage::showError(const QString &message)
{
    KMessageBox::error(this, message, i18n("OS Templates"));
}
