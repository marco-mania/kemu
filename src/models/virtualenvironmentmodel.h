/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QAbstractItemModel>
#include <QDateTime>
#include <QDir>
#include <QIcon>
#include <QMimeData>
#include <QPointer>
#include <QTimer>
#include <QVector>

#include <KProcess>

#include "utils/imagedatabase.h"
#include "utils/qmpclient.h"
#include "utils/vmconfig.h"

using namespace Qt::StringLiterals;

struct VmEntry {
    VmEntry() = default;
    VmEntry(const QString &vmName, const QDir &vmDir)
        : name(vmName)
        , path(vmDir)
    {
    }

    QString name;
    QDir path; // absolute directory of this VM, set when loading

    VMConfig config;

    enum class State {
        Stopped,
        Creating,
        Running,
        ShuttingDown,
        Shrinking,
        Flattening, // a linked clone copies in the shared disk data
        Encrypting, // the system disk is rewritten encrypted
        Decrypting, // ... and back
        Exporting,
        Deploying,
        Cloning,
        Saving,
        Saved
    };
    State state = State::Stopped;

    bool isHeadless = false;

    QPointer<QMPClient> qmpClient;

    QPointer<KProcess> qemuProcess;
    QPointer<KProcess> swtpmProcess;
    QPointer<KProcess> virtiofsdProcess;

    QVector<double> cpuHistory;
    QVector<double> ramHistory;
    QVector<double> ioHistory;
    double ioPeakBytesPerSec = 0.0;

    QString folderName;

    // USB passthrough: key = "bus:addr", value = QEMU device id
    QMap<QString, QString> attachedUsb;

    bool hasSavestateFile = false; // cached: existence of savestate file on disk
    QDateTime startedAt; // wall-clock when the VM entered Running; null otherwise
    QDateTime savestateTime; // mtime of the savestate file; null when none
    QIcon styledIcon; // cached composed icon (visual state + headless emblem)

    // Derived from the configs of all VMs on every reload, never stored.
    QStringList linkedClones; // linked clones based on this VM
    bool linkedSourceMissing = false; // this linked clone lost its source

    // A source VM stays untouched while linked clones depend on it.
    [[nodiscard]] bool isFrozen() const
    {
        return !linkedClones.isEmpty();
    }

    // Everything that only exists while the VM is live. Kept in one place so a
    // reload cannot silently drop a field.
    void adoptRuntimeStateFrom(const VmEntry &other)
    {
        state = other.state;
        isHeadless = other.isHeadless;
        startedAt = other.startedAt;
        qemuProcess = other.qemuProcess;
        swtpmProcess = other.swtpmProcess;
        virtiofsdProcess = other.virtiofsdProcess;
        qmpClient = other.qmpClient;
        attachedUsb = other.attachedUsb;
        cpuHistory = other.cpuHistory;
        ramHistory = other.ramHistory;
        ioHistory = other.ioHistory;
        ioPeakBytesPerSec = other.ioPeakBytesPerSec;
    }

    void resetRuntimeState()
    {
        state = State::Stopped;
        isHeadless = false;
        startedAt = {};
        qemuProcess = nullptr;
        swtpmProcess = nullptr;
        virtiofsdProcess = nullptr;
        qmpClient = nullptr;
        attachedUsb.clear();
        cpuHistory.clear();
        ramHistory.clear();
        ioHistory.clear();
        ioPeakBytesPerSec = 0.0;
    }
};

struct FolderEntry {
    QString name;
    QVector<int> vmIndices;
};

class VirtualEnvironmentModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    // Item-Typen
    enum ItemType {
        Folder = 0,
        Vm = 1
    };

    enum Role {
        ItemTypeRole = Qt::UserRole + 100,
        SubtitleRole = Qt::UserRole + 101,
        VmNameRole = Qt::UserRole + 102,
        VmStateRole = Qt::UserRole + 200,
        MediaCdRole = Qt::UserRole + 201,
        MediaFloppyRole = Qt::UserRole + 202,
    };

    explicit VirtualEnvironmentModel(ImageDatabase *imageDb, QObject *parent = nullptr);
    ~VirtualEnvironmentModel() override;

    // QAbstractItemModel interface
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    // Drag & Drop
    Qt::DropActions supportedDropActions() const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) override;

    // VM access
    VmEntry *entryByName(const QString &name);
    const VmEntry *entryByName(const QString &name) const;
    QModelIndex indexForVm(const QString &name) const;
    QModelIndex indexForFolder(const QString &folderName) const;
    QStringList vmNames() const;

    // Loading
    // folderNames carries folders that hold no VM; they exist only here.
    void reload(const QDir &baseDir,
                const QStringList &folderNames,
                const QMap<QString, QString> &folderAssignments,
                std::function<VMConfig(const QString &vmPath)> loader);

    // State management (emits dataChanged)
    void setVmState(const QString &name, VmEntry::State state);
    void setHeadless(const QString &name, const bool headless);
    void setQemuProcess(const QString &name, KProcess *process);
    void setSwtpmProcess(const QString &name, KProcess *process);
    void setVirtiofsdProcess(const QString &name, KProcess *process);
    void setQmpClient(const QString &name, QMPClient *client);
    void updateConfig(const QString &name, const VMConfig &config);
    void appendPerfSample(const QString &name, int channel, double value);

    void setHasSavestate(const QString &name, bool exists);

    bool isRunning(const QString &name) const;
    bool isBusy(const QString &name) const;
    void clearRuntimeState(const QString &name);
    bool isHeadless(const QString &name) const;

    // Folder management
    QModelIndex createFolder(const QString &name);
    bool removeFolder(const QModelIndex &index);
    // Returns false when another folder already uses that name.
    bool renameFolder(const QModelIndex &index, const QString &newName);
    void moveVmToFolder(const QString &vmName, const QString &folderName);
    void moveVmToRoot(const QString &vmName);
    // Moves a folder node back to its sorted position after a rename.

    // Persistence
    QMap<QString, QString> folderAssignments() const;
    QStringList folderNames() const;

    // MAC address management (unique addresses)
    [[nodiscard]] QList<MacUsageInfo> macUsageInfo() const;
    [[nodiscard]] QStringList usedMacAddresses() const;

    /// VM name to UUID, for VMs that have one.
    [[nodiscard]] QMap<QString, QUuid> vmUuids() const;

Q_SIGNALS:
    void vmStateChanged(const QString &vmName, VmEntry::State newState);

private:
    ImageDatabase *m_imageDatabase;

    struct Node {
        enum Type {
            RootNode,
            FolderNode,
            VmNode
        };
        Type type = RootNode;
        int folderIndex = -1;
        int vmIndex = -1;
        Node *parent = nullptr;
        QVector<Node *> children;
        ~Node()
        {
            qDeleteAll(children);
        }
    };

    Node *m_root = nullptr;
    QVector<VmEntry> m_vms;
    QVector<FolderEntry> m_folders;

    Node *nodeFromIndex(const QModelIndex &index) const;
    QModelIndex indexFromNode(Node *node) const;
    Node *findVmNode(const QString &name) const;
    Node *findFolderNode(const QString &folderName) const;
    void emitVmChanged(const QString &name);
    void rebuildTree(const QStringList &folderNames, const QMap<QString, QString> &folderAssignments);
    void moveFolderNodeIntoOrder(Node *node);

    QTimer *m_uptimeTimer = nullptr;

    void recomputeIcon(VmEntry &e) const; // rebuild cached styled icon
    void refreshLinks(); // derive linked clone relations from the configs
    void refreshSavestateTime(VmEntry &e) const; // (re)read savestate file mtime
    void updateUptimeTimer(); // start/stop based on running VMs
    void tickUptime(); // emit subtitle/tooltip for running VMs
};
