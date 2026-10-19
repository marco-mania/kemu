/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QWidget>

#include "utils/cloudimagedeployer.h"
#include "utils/templatestore.h"

class ImageDatabase;
class QPushButton;
class QTreeWidget;

/**
 * @brief The cloud-init templates of the templates settings page.
 *
 * Changes are written at once.
 */
class CloudInitTemplatesTab : public QWidget
{
    Q_OBJECT

public:
    explicit CloudInitTemplatesTab(QWidget *parent = nullptr);

    /// For the "Used by" column.
    void setImageDatabase(ImageDatabase *db);
    /// Values the preview renders with.
    void setPreviewConfig(const CloudDeployConfig &config);
    void refresh(const QString &selectId = {});

private:
    void updateButtons();
    [[nodiscard]] std::optional<CloudInitEntry> selectedEntry() const;
    [[nodiscard]] QStringList allIds() const;
    [[nodiscard]] QStringList usersOf(const QString &id) const;

    void newTemplate();
    void duplicateTemplate();
    void editTemplate();
    void removeTemplate();
    void importTemplate();
    void exportTemplate();
    void openFolder();

    bool save(const QString &id, const QString &text, const QString &replacedUserId = {});
    void showError(const QString &message);

    ImageDatabase *m_db = nullptr;
    TemplateStore m_store;
    CloudDeployConfig m_previewConfig;
    QList<CloudInitEntry> m_entries;

    QTreeWidget *m_tree = nullptr;
    QPushButton *m_newButton = nullptr;
    QPushButton *m_duplicateButton = nullptr;
    QPushButton *m_editButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_exportButton = nullptr;
};
