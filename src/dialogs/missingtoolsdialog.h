/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <KLocalizedString>

#include "utils/externaltools.h"

using namespace Qt::StringLiterals;

class MissingToolsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MissingToolsDialog(const QList<ExternalTools::FeatureGroup> &missing, QWidget *parent = nullptr);

    [[nodiscard]] bool openSettingsRequested() const
    {
        return m_openSettings;
    }

private:
    bool m_openSettings = false;
};
