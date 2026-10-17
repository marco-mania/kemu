/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>

#include "utils/vmconfig.h"

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class KUrlRequester;

class DiskDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Mode {
        Add,
        Edit
    };

    DiskDialog(Mode mode, VMSpecs::MachineType machineType, QWidget *parent = nullptr);

    void setLabel(const QString &label);
    void setDiskInterface(VMSpecs::DiskInterface diskInterface);

    [[nodiscard]] QString label() const;
    [[nodiscard]] VMSpecs::DiskInterface diskInterface() const;
    [[nodiscard]] int sizeGb() const;
    /// Image to copy into the VM; empty for a new, blank disk.
    [[nodiscard]] QString sourcePath() const;

private:
    void updateState();

    Mode m_mode;

    QRadioButton *m_newRadio = nullptr;
    QRadioButton *m_existingRadio = nullptr;
    QLineEdit *m_labelEdit = nullptr;
    QSpinBox *m_sizeSpin = nullptr;
    QLabel *m_sizeLabel = nullptr;
    KUrlRequester *m_sourceRequester = nullptr;
    QLabel *m_sourceLabel = nullptr;
    QLabel *m_sourceHint = nullptr;
    QComboBox *m_interfaceCombo = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;
};
