/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QJsonObject>
#include <QWidget>

#include "utils/templatestore.h"

class ImageDatabase;
struct TemplateInfo;
class QPushButton;
class QTreeWidget;

/**
 * @brief Settings page for the OS templates: create, customize, hide, share.
 *
 * Changes are written at once, like those to virtual networks.
 */
class TemplatesSettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit TemplatesSettingsPage(QWidget *parent = nullptr);

    void setImageDatabase(ImageDatabase *db);

private:
    void refresh(const QString &selectId = {});
    void updateButtons();
    [[nodiscard]] const TemplateInfo *selectedTemplate() const;
    [[nodiscard]] QStringList allIds() const;
    [[nodiscard]] std::optional<QJsonObject> definitionOf(const TemplateInfo &tpl) const;

    void newTemplate();
    void duplicateTemplate();
    void editTemplate();
    void removeTemplate();
    void toggleHidden();
    void importTemplates();
    void exportTemplate();
    void openFolder();

    bool save(const QJsonObject &tpl, const QString &replacedUserId = {});
    void showError(const QString &message);

    ImageDatabase *m_db = nullptr;
    TemplateStore m_store;

    QTreeWidget *m_tree = nullptr;
    QPushButton *m_newButton = nullptr;
    QPushButton *m_duplicateButton = nullptr;
    QPushButton *m_editButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_hideButton = nullptr;
    QPushButton *m_importButton = nullptr;
    QPushButton *m_exportButton = nullptr;
};
