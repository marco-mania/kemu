/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>

#include <optional>

#include "utils/vmconfig.h"

class QComboBox;
class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QToolButton;
class KMessageWidget;
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
    /// The system disk: its name is fixed, and the controller hint covers
    /// what installing @p osFamily needs.
    void setSystemDisk(VMSpecs::OsFamily osFamily);

    [[nodiscard]] QString label() const;
    [[nodiscard]] VMSpecs::DiskInterface diskInterface() const;
    [[nodiscard]] int sizeGb() const;
    /// Image to copy into the VM; empty for a new, blank disk.
    [[nodiscard]] QString sourcePath() const;

    /// Edit mode, for a disk that exists already: offers compacting it,
    /// which happens at once, independent of OK/Cancel.
    void setCompactAvailable(bool available);
    /// Edit mode, existing disk: its size; larger sizes are applied at once
    /// through "Expand…". 0 if unknown.
    void setCurrentSize(int sizeGb);
    /// Edit mode, disk that is only created on OK: its size can change.
    void setNewDiskSize(int sizeGb, int minimumGb = 1);

    struct Encryption {
        bool encrypted = false;
        bool passwordStored = false;
        bool walletAvailable = false;
        /// Why it cannot change now; empty if it can.
        QString blockedReason;
    };
    /// System disk of an existing VM: shows its encryption and offers to
    /// change it, which happens at once, independent of OK/Cancel.
    void setEncryption(const Encryption &encryption);

Q_SIGNALS:
    void compactRequested();
    void expandRequested(int sizeGb);
    void encryptRequested();
    void decryptRequested();
    void changePasswordRequested();
    void storePasswordRequested();
    void forgetPasswordRequested();

private:
    void updateState();
    void updateInterfaceHint();

    Mode m_mode;
    VMSpecs::MachineType m_machineType;
    std::optional<VMSpecs::OsFamily> m_systemOsFamily;

    QRadioButton *m_newRadio = nullptr;
    QRadioButton *m_existingRadio = nullptr;
    QLineEdit *m_labelEdit = nullptr;
    QSpinBox *m_sizeSpin = nullptr;
    QLabel *m_sizeLabel = nullptr;
    KUrlRequester *m_sourceRequester = nullptr;
    QLabel *m_sourceLabel = nullptr;
    KMessageWidget *m_sourceHint = nullptr;
    QFormLayout *m_form = nullptr;
    QComboBox *m_interfaceCombo = nullptr;
    KMessageWidget *m_interfaceHint = nullptr;
    QLabel *m_encryptionLabel = nullptr;
    QToolButton *m_encryptButton = nullptr;
    QToolButton *m_decryptButton = nullptr;
    QToolButton *m_changePasswordButton = nullptr;
    QToolButton *m_walletButton = nullptr;
    bool m_passwordStored = false;
    QDialogButtonBox *m_buttonBox = nullptr;
    QPushButton *m_compactButton = nullptr;
    QPushButton *m_expandButton = nullptr;
    int m_currentSizeGb = 0;
};
