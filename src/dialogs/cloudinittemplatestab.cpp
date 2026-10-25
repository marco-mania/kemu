/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cloudinittemplatestab.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <KLocalizedString>
#include <KMessageBox>

#include "dialogs/cloudiniteditdialog.h"
#include "utils/imagedatabase.h"

using namespace Qt::StringLiterals;

namespace
{

constexpr int IdRole = Qt::UserRole;
constexpr qint64 kMaxTemplateSize = 1 << 20;
const QString kSuffix = u".yaml.tpl"_s;

QString readText(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.size() <= kMaxTemplateSize ? QString::fromUtf8(file.readAll()) : QString();
}

QString uniqueId(const QString &base, const QStringList &taken)
{
    QString id = base;
    for (int n = 2; taken.contains(id); ++n)
        id = u"%1-%2"_s.arg(base).arg(n);
    return id;
}

} // namespace

CloudInitTemplatesTab::CloudInitTemplatesTab(QWidget *parent)
    : QWidget(parent)
{
    auto *intro = new QLabel(i18n("Cloud-init templates set up a guest during automated deployment. Each OS template names the one it uses; "
                                  "a different one can be chosen in the cloud image configuration."),
                             this);
    intro->setWordWrap(true);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({i18n("ID"), i18n("Type"), i18n("Used By"), i18n("Status")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setRootIsDecorated(false);

    const auto makeButton = [this](const QString &icon, const QString &text, void (CloudInitTemplatesTab::*slot)()) {
        auto *button = new QPushButton(QIcon::fromTheme(icon), text, this);
        connect(button, &QPushButton::clicked, this, slot);
        return button;
    };
    m_newButton = makeButton(u"list-add"_s, i18n("New…"), &CloudInitTemplatesTab::newTemplate);
    m_duplicateButton = makeButton(u"edit-copy"_s, i18n("Duplicate…"), &CloudInitTemplatesTab::duplicateTemplate);
    m_editButton = makeButton(u"document-edit"_s, i18n("Edit…"), &CloudInitTemplatesTab::editTemplate);
    m_removeButton = makeButton(u"edit-delete"_s, i18n("Delete"), &CloudInitTemplatesTab::removeTemplate);
    QPushButton *importButton = makeButton(u"document-import"_s, i18n("Import…"), &CloudInitTemplatesTab::importTemplate);
    m_exportButton = makeButton(u"document-export"_s, i18n("Export…"), &CloudInitTemplatesTab::exportTemplate);
    QPushButton *folderButton = makeButton(u"folder-open"_s, i18n("Open Folder"), &CloudInitTemplatesTab::openFolder);
    folderButton->setToolTip(i18n("Open the folder with your own cloud-init templates"));

    auto *buttons = new QVBoxLayout;
    for (QPushButton *button : {m_newButton, m_duplicateButton, m_editButton, m_removeButton})
        buttons->addWidget(button);
    buttons->addSpacing(12);
    buttons->addWidget(importButton);
    buttons->addWidget(m_exportButton);
    buttons->addStretch();
    buttons->addWidget(folderButton);

    auto *row = new QHBoxLayout;
    row->addWidget(m_tree, 1);
    row->addLayout(buttons);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(row, 1);

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &CloudInitTemplatesTab::updateButtons);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this]() {
        if (selectedEntry())
            editTemplate();
    });

    refresh();
}

void CloudInitTemplatesTab::setImageDatabase(ImageDatabase *db)
{
    m_db = db;
    refresh();
}

void CloudInitTemplatesTab::setPreviewConfig(const CloudDeployConfig &config)
{
    m_previewConfig = config;
}

void CloudInitTemplatesTab::refresh(const QString &selectId)
{
    const QString keep = selectId.isEmpty() && selectedEntry() ? selectedEntry()->id : selectId;
    m_entries = m_store.cloudInitTemplates();
    m_tree->clear();

    const std::pair<TemplateOrigin, QString> groups[] = {
        {TemplateOrigin::User, i18n("Own Templates")},
        {TemplateOrigin::Downloaded, i18n("Downloaded Templates")},
        {TemplateOrigin::BuiltIn, i18n("Built-in Templates")},
    };
    QTreeWidgetItem *toSelect = nullptr;

    for (const auto &[origin, title] : groups) {
        auto *group = new QTreeWidgetItem({title});
        group->setFlags(Qt::ItemIsEnabled);
        QFont bold = group->font(0);
        bold.setBold(true);
        group->setFont(0, bold);

        for (const CloudInitEntry &entry : std::as_const(m_entries)) {
            if (entry.origin != origin)
                continue;
            QString status;
            if (entry.replaces == TemplateOrigin::BuiltIn)
                status = i18n("Customizes a built-in template");
            else if (entry.replaces == TemplateOrigin::Downloaded)
                status = i18n("Customizes a downloaded template");
            QString users = usersOf(entry.id).join(u", "_s);
            if (entry.id == u"default"_s)
                users = users.isEmpty() ? i18n("Fallback for missing templates") : users;

            auto *item = new QTreeWidgetItem(group,
                                             {entry.id,
                                              TemplateStore::isNetworkTemplate(entry.id) ? i18nc("@item cloud-init template type", "Network")
                                                                                         : i18nc("@item cloud-init template type", "User data"),
                                              users,
                                              status});
            item->setData(0, IdRole, entry.id);
            item->setToolTip(0, entry.path);
            item->setToolTip(2, users);
            if (entry.id == keep)
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

std::optional<CloudInitEntry> CloudInitTemplatesTab::selectedEntry() const
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    if (!item || !item->isSelected())
        return std::nullopt;
    const QString id = item->data(0, IdRole).toString();
    for (const CloudInitEntry &entry : m_entries) {
        if (entry.id == id)
            return entry;
    }
    return std::nullopt;
}

QStringList CloudInitTemplatesTab::allIds() const
{
    QStringList ids;
    for (const CloudInitEntry &entry : m_entries)
        ids << entry.id;
    return ids;
}

QStringList CloudInitTemplatesTab::usersOf(const QString &id) const
{
    QStringList names;
    if (!m_db)
        return names;
    for (const TemplateInfo &tpl : m_db->allTemplates()) {
        if (tpl.cloudInitTemplate == id || tpl.networkTemplate == id)
            names << tpl.displayName;
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

void CloudInitTemplatesTab::updateButtons()
{
    const auto entry = selectedEntry();
    const bool own = entry && entry->origin == TemplateOrigin::User;
    m_duplicateButton->setEnabled(entry.has_value());
    m_editButton->setEnabled(entry.has_value());
    m_exportButton->setEnabled(entry.has_value());
    m_editButton->setText(!entry || own ? i18n("Edit…") : i18n("Customize…"));
    m_removeButton->setEnabled(own);
    const bool reset = own && entry->replaces.has_value();
    m_removeButton->setText(reset ? i18n("Reset") : i18n("Delete"));
    m_removeButton->setIcon(QIcon::fromTheme(reset ? u"edit-undo"_s : u"edit-delete"_s));
}

void CloudInitTemplatesTab::newTemplate()
{
    QString text = readText(m_store.locateCloudInit(u"default"_s));
    if (text.isEmpty())
        text = u"#cloud-config\nhostname: {{ HOSTNAME }}\n"_s;
    CloudInitEditDialog dialog(CloudInitEditDialog::Mode::New, uniqueId(u"my-cloud-init"_s, allIds()), text, allIds(), m_previewConfig, this);
    if (dialog.exec() == QDialog::Accepted)
        save(dialog.id(), dialog.text());
}

void CloudInitTemplatesTab::duplicateTemplate()
{
    const auto entry = selectedEntry();
    if (!entry)
        return;
    CloudInitEditDialog dialog(CloudInitEditDialog::Mode::New,
                               uniqueId(entry->id + u"-copy"_s, allIds()),
                               readText(entry->path),
                               allIds(),
                               m_previewConfig,
                               this);
    if (dialog.exec() == QDialog::Accepted)
        save(dialog.id(), dialog.text());
}

void CloudInitTemplatesTab::editTemplate()
{
    const auto entry = selectedEntry();
    if (!entry)
        return;
    const bool own = entry->origin == TemplateOrigin::User;
    QStringList taken = allIds();
    taken.removeAll(entry->id);

    CloudInitEditDialog dialog(own ? CloudInitEditDialog::Mode::Edit : CloudInitEditDialog::Mode::Customize,
                               entry->id,
                               readText(entry->path),
                               taken,
                               m_previewConfig,
                               this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    // OS templates refer to cloud-init templates by id.
    const QStringList users = usersOf(entry->id);
    if (own && dialog.id() != entry->id && !users.isEmpty()) {
        const auto answer = KMessageBox::warningTwoActions(
            this,
            i18n("'%1' is used by %2, which will fall back to the default template once it is renamed. Rename it anyway?", entry->id, users.join(u", "_s)),
            i18n("Rename Template"),
            KGuiItem(i18n("Rename"), u"edit-rename"_s),
            KStandardGuiItem::cancel());
        if (answer != KMessageBox::PrimaryAction)
            return;
    }
    save(dialog.id(), dialog.text(), own ? entry->id : QString());
}

void CloudInitTemplatesTab::removeTemplate()
{
    const auto entry = selectedEntry();
    if (!entry || entry->origin != TemplateOrigin::User)
        return;

    const bool reset = entry->replaces.has_value();
    QString text = reset ? i18n("Reset '%1'? Your changes are lost and the original template is used again.", entry->id)
                         : i18n("Delete the cloud-init template '%1'?", entry->id);
    const QStringList users = usersOf(entry->id);
    if (!reset && !users.isEmpty())
        text += u' ' + i18n("It is used by %1, which will fall back to the default template.", users.join(u", "_s));

    const auto answer = KMessageBox::warningTwoActions(this,
                                                       text,
                                                       reset ? i18n("Reset Template") : i18n("Delete Template"),
                                                       reset ? KGuiItem(i18n("Reset"), u"edit-undo"_s) : KStandardGuiItem::del(),
                                                       KStandardGuiItem::cancel());
    if (answer != KMessageBox::PrimaryAction)
        return;

    QString error;
    if (!m_store.removeUserCloudInit(entry->id, &error))
        showError(error);
    refresh(reset ? entry->id : QString());
}

void CloudInitTemplatesTab::importTemplate()
{
    const QString path = QFileDialog::getOpenFileName(this, i18n("Import Cloud-Init Template"), QDir::homePath(), i18n("Cloud-init templates (*.yaml.tpl)"));
    if (path.isEmpty())
        return;

    const QString fileName = QFileInfo(path).fileName();
    const QString id = fileName.endsWith(kSuffix) ? fileName.chopped(kSuffix.size()) : QString();
    if (!TemplateStore::isValidId(id)) {
        showError(i18n("The file name must be the template id followed by \".yaml.tpl\", e.g. \"my-server.yaml.tpl\"."));
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxTemplateSize) {
        showError(i18n("Cannot read %1.", path));
        return;
    }
    const QString text = QString::fromUtf8(file.readAll());

    QString renderError;
    CloudImageDeployer::renderTemplate(text, m_previewConfig, renderError);
    if (!renderError.isEmpty()) {
        showError(i18n("The template cannot be rendered: %1", renderError));
        return;
    }

    if (allIds().contains(id)) {
        const auto answer =
            KMessageBox::questionTwoActions(this,
                                            i18n("A cloud-init template with the id '%1' exists already. Replace it with the imported one?", id),
                                            i18n("Import Cloud-Init Template"),
                                            KGuiItem(i18n("Replace"), u"document-import"_s),
                                            KStandardGuiItem::cancel());
        if (answer != KMessageBox::PrimaryAction)
            return;
    }
    save(id, text);
}

void CloudInitTemplatesTab::exportTemplate()
{
    const auto entry = selectedEntry();
    if (!entry)
        return;
    QString path = QFileDialog::getSaveFileName(this,
                                                i18n("Export Cloud-Init Template"),
                                                QDir::home().filePath(entry->id + kSuffix),
                                                i18n("Cloud-init templates (*.yaml.tpl)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(kSuffix))
        path += kSuffix;

    const QByteArray data = readText(entry->path).toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        showError(i18n("Cannot write %1: %2", path, file.errorString()));
}

void CloudInitTemplatesTab::openFolder()
{
    const QString dir = m_store.userCloudInitDir();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

bool CloudInitTemplatesTab::save(const QString &id, const QString &text, const QString &replacedUserId)
{
    QString error;
    if (!m_store.saveUserCloudInit(id, text, &error)) {
        showError(error);
        return false;
    }
    if (!replacedUserId.isEmpty() && replacedUserId != id && !m_store.removeUserCloudInit(replacedUserId, &error))
        showError(error);
    refresh(id);
    return true;
}

void CloudInitTemplatesTab::showError(const QString &message)
{
    KMessageBox::error(this, message, i18n("Cloud-Init Templates"));
}
