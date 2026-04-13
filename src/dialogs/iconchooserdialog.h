/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QListWidget>
#include <QVBoxLayout>

#include <KLocalizedString>

using namespace Qt::StringLiterals;

class IconChooserDialog : public QDialog {
    Q_OBJECT

public:
    explicit IconChooserDialog(QWidget* parent = nullptr);
    QString selectedIconPath() const;

private:
    QListWidget* listWidget = nullptr;
};
