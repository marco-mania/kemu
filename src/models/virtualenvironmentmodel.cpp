/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "models/virtualenvironmentmodel.h"

#include <QFileInfo>

#include "const.h"
#include "utils/savestatemanager.h"

#include "kemu_debug.h"

namespace
{
QString formatUptime(const QDateTime &since)
{
    if (!since.isValid())
        return {};

    const qint64 totalSecs = since.secsTo(QDateTime::currentDateTime());
    if (totalSecs < 0)
        return {};

    qint64 mins = totalSecs / 60;
    qint64 hours = mins / 60;
    qint64 days = hours / 24;

    mins %= 60;
    hours %= 24;
    const qint64 years = days / 365;
    days %= 365;
    const qint64 weeks = days / 7;
    days %= 7;

    QStringList parts;

    if (years > 0) {
        parts << i18ncp("VM uptime year", "%1 year", "%1 years", years);
    }
    if (weeks > 0) {
        parts << i18ncp("VM uptime week", "%1 week", "%1 weeks", weeks);
    }
    if (days > 0) {
        parts << i18ncp("VM uptime day", "%1 day", "%1 days", days);
    }
    if (hours > 0) {
        parts << i18ncp("VM uptime hour", "%1 hour", "%1 hours", hours);
    }

    if (mins > 0 || parts.isEmpty()) {
        parts << i18ncp("VM uptime minute", "%1 minute", "%1 minutes", mins);
    }

    return i18nc("VM uptime prefix", "up %1", parts.join(QStringLiteral(", ")));
}
}

VirtualEnvironmentModel::VirtualEnvironmentModel(ImageDatabase *imageDb, QObject *parent)
    : QAbstractItemModel(parent)
    , m_imageDatabase(imageDb)
    , m_root(new Node)
{
    m_root->type = Node::RootNode;

    m_uptimeTimer = new QTimer(this);
    // formatUptime() resolves to minutes, so a second-resolution tick would
    // repaint 59 times for nothing.
    m_uptimeTimer->setInterval(15000);
    connect(m_uptimeTimer, &QTimer::timeout, this, [this] {
        tickUptime();
    });
}

VirtualEnvironmentModel::~VirtualEnvironmentModel()
{
    delete m_root;
}

// -- QAbstractItemModel interface --

QModelIndex VirtualEnvironmentModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column != 0)
        return {};

    const Node *parentNode = parent.isValid() ? nodeFromIndex(parent) : m_root;
    if (!parentNode || row < 0 || row >= parentNode->children.size())
        return {};

    return createIndex(row, 0, parentNode->children.at(row));
}

QModelIndex VirtualEnvironmentModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};

    const Node *node = nodeFromIndex(child);
    if (!node || !node->parent || node->parent == m_root)
        return {};

    return indexFromNode(node->parent);
}

int VirtualEnvironmentModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0)
        return 0;
    const Node *node = parent.isValid() ? nodeFromIndex(parent) : m_root;
    return node ? node->children.size() : 0;
}

int VirtualEnvironmentModel::columnCount(const QModelIndex &) const
{
    return 1;
}

QVariant VirtualEnvironmentModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};

    const Node *node = nodeFromIndex(index);
    if (!node)
        return {};

    if (node->type == Node::FolderNode) {
        const FolderEntry &folder = m_folders.at(node->folderIndex);
        switch (role) {
        case Qt::DisplayRole:
            return folder.name;
        case Qt::DecorationRole:
            return QIcon::fromTheme(u"folder"_s);
        case ItemTypeRole:
            return Folder;
        default:
            return {};
        }
    }

    if (node->type == Node::VmNode) {
        const VmEntry &vm = m_vms.at(node->vmIndex);
        switch (role) {
        case Qt::DisplayRole:
            return vm.name;
        case Qt::DecorationRole:
            return vm.styledIcon;
        case ItemTypeRole:
            return Vm;
        case VmNameRole:
            return vm.name;
        case SubtitleRole: {
            switch (vm.state) {
            case VmEntry::State::Running: {
                QString s = formatUptime(vm.startedAt);
                if (vm.isHeadless && !s.isEmpty())
                    s += i18nc("appended to VM uptime", " \u00B7 headless");
                return s;
            }
            case VmEntry::State::Saved:
                if (vm.savestateTime.isValid())
                    return i18nc("saved VM, with absolute timestamp",
                                 "Saved State \u00B7 %1",
                                 QLocale::system().toString(vm.savestateTime, QLocale::ShortFormat));
                return i18n("Saved State");
            case VmEntry::State::Saving:
                return i18n("Saving state...");
            case VmEntry::State::Deploying:
                return i18n("Deploying...");
            case VmEntry::State::Cloning:
                return i18n("Cloning...");
            case VmEntry::State::Creating:
                return i18n("Creating...");
            case VmEntry::State::Shrinking:
                return i18n("Shrinking disk...");
            case VmEntry::State::Exporting:
                return i18n("Exporting...");
            case VmEntry::State::ShuttingDown:
                return i18n("Shutting down...");
            case VmEntry::State::Stopped:
                return {}; // intentionally empty
            }
            return {};
        }
        case Qt::ToolTipRole: {
            QStringList lines;
            lines << vm.name;

            QString os = VMConfig::toUIString(vm.config.osType());
            if (!vm.config.distroId().isEmpty() && vm.config.distroId() != u"other"_s)
                os += u" \u2014 "_s + m_imageDatabase->displayName(vm.config.distroId());
            lines << os;

            if (vm.state == VmEntry::State::Running && vm.startedAt.isValid()) {
                lines << i18n("Uptime: %1", formatUptime(vm.startedAt));
                if (vm.isHeadless)
                    lines << i18n("Headless");
            } else if (vm.state == VmEntry::State::Saved && vm.savestateTime.isValid()) {
                lines << i18n("Saved: %1", QLocale::system().toString(vm.savestateTime, QLocale::ShortFormat));
            }

            return lines.join(u'\n');
        }
        case VmStateRole:
            return static_cast<int>(vm.state);
        case MediaCdRole:
            return !vm.config.isoPath().isEmpty();
        case MediaFloppyRole:
            return !vm.config.floppyPath().isEmpty() && vm.config.machineType() != VMSpecs::MachineType::Q35;
        default:
            return {};
        }
    }

    return {};
}

Qt::ItemFlags VirtualEnvironmentModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::ItemIsDropEnabled;

    const Node *node = nodeFromIndex(index);
    if (!node)
        return Qt::NoItemFlags;

    if (node->type == Node::FolderNode)
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;
    if (node->type == Node::VmNode)
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;

    return Qt::NoItemFlags;
}

// -- Drag & Drop --

Qt::DropActions VirtualEnvironmentModel::supportedDropActions() const
{
    return Qt::MoveAction;
}

QStringList VirtualEnvironmentModel::mimeTypes() const
{
    return {u"application/x-kemu-vm"_s};
}

QMimeData *VirtualEnvironmentModel::mimeData(const QModelIndexList &indexes) const
{
    if (indexes.isEmpty())
        return nullptr;
    const Node *node = nodeFromIndex(indexes.first());
    if (!node || node->type != Node::VmNode)
        return nullptr;

    auto *md = new QMimeData;
    md->setData(u"application/x-kemu-vm"_s, m_vms.at(node->vmIndex).name.toUtf8());
    return md;
}

bool VirtualEnvironmentModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int, int, const QModelIndex &parent)
{
    if (action != Qt::MoveAction)
        return false;
    if (!data->hasFormat(u"application/x-kemu-vm"_s))
        return false;

    const QString vmName = QString::fromUtf8(data->data(u"application/x-kemu-vm"_s));

    if (parent.isValid()) {
        const Node *target = nodeFromIndex(parent);
        if (target && target->type == Node::FolderNode) {
            moveVmToFolder(vmName, m_folders.at(target->folderIndex).name);
            return true;
        }
    }
    moveVmToRoot(vmName);
    return true;
}

// -- VM access --

VmEntry *VirtualEnvironmentModel::entryByName(const QString &name)
{
    for (auto &vm : m_vms)
        if (vm.name == name)
            return &vm;
    return nullptr;
}

const VmEntry *VirtualEnvironmentModel::entryByName(const QString &name) const
{
    for (const auto &vm : m_vms)
        if (vm.name == name)
            return &vm;
    return nullptr;
}

QModelIndex VirtualEnvironmentModel::indexForVm(const QString &name) const
{
    Node *node = findVmNode(name);
    return node ? indexFromNode(node) : QModelIndex();
}

QModelIndex VirtualEnvironmentModel::indexForFolder(const QString &folderName) const
{
    Node *node = findFolderNode(folderName);
    return node ? indexFromNode(node) : QModelIndex();
}

QStringList VirtualEnvironmentModel::vmNames() const
{
    QStringList names;
    names.reserve(m_vms.size());
    for (const auto &vm : m_vms)
        names << vm.name;
    return names;
}

// -- Loading --

void VirtualEnvironmentModel::reload(const QDir &baseDir,
                                     const QStringList &folderNames,
                                     const QMap<QString, QString> &folderAssignments,
                                     std::function<VMConfig(const QString &vmPath)> loader)
{
    QMap<QString, VmEntry> stateBackup;
    for (const auto &vm : std::as_const(m_vms)) {
        if (vm.state != VmEntry::State::Stopped)
            stateBackup.insert(vm.name, vm);
    }

    beginResetModel();

    delete m_root;
    m_root = new Node;
    m_root->type = Node::RootNode;
    m_vms.clear();
    m_folders.clear();

    const QStringList vmDirs = baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    for (const QString &d : vmDirs) {
        QString vmPath = baseDir.filePath(d);
        if (!QFile::exists(vmPath + u"/config.json"_s))
            continue;

        VmEntry entry(d, QDir(vmPath));
        entry.config = loader(vmPath);
        entry.folderName = folderAssignments.value(d);

        const auto backup = stateBackup.constFind(d);
        if (backup != stateBackup.cend())
            entry.adoptRuntimeStateFrom(*backup);

        entry.hasSavestateFile = SaveStateManager::hasSavestate(vmPath, d);

        if (entry.hasSavestateFile) {
            const QFileInfo fi(SaveStateManager::savestateFilePath(vmPath, d));
            if (fi.exists())
                entry.savestateTime = fi.lastModified();
        }
        // Reflect saved state on load, but never clobber a running/busy VM that
        // was just restored from stateBackup above.
        if (entry.hasSavestateFile && entry.state == VmEntry::State::Stopped)
            entry.state = VmEntry::State::Saved;
        recomputeIcon(entry);

        m_vms.append(std::move(entry));
    }

    rebuildTree(folderNames, folderAssignments);
    endResetModel();
    updateUptimeTimer();
}

// -- State management --

void VirtualEnvironmentModel::setVmState(const QString &name, VmEntry::State state)
{
    VmEntry *e = entryByName(name);
    if (!e || e->state == state)
        return;

    const VmEntry::State prev = e->state;
    e->state = state;

    if (state == VmEntry::State::Running && prev != VmEntry::State::Running)
        e->startedAt = QDateTime::currentDateTime();
    if (state == VmEntry::State::Saved) {
        e->startedAt = QDateTime();
        e->hasSavestateFile = true; // entering Saved implies the file exists
        refreshSavestateTime(*e);
    }

    recomputeIcon(*e);
    emitVmChanged(name);
    Q_EMIT vmStateChanged(name, state);
    updateUptimeTimer();
}

void VirtualEnvironmentModel::setHeadless(const QString &name, const bool headless)
{
    if (VmEntry *e = entryByName(name)) {
        if (e->isHeadless == headless)
            return;
        e->isHeadless = headless;
        recomputeIcon(*e);
        emitVmChanged(name);
    }
}

void VirtualEnvironmentModel::setQemuProcess(const QString &name, KProcess *p)
{
    if (VmEntry *e = entryByName(name))
        e->qemuProcess = p;
}

void VirtualEnvironmentModel::setSwtpmProcess(const QString &name, KProcess *p)
{
    if (VmEntry *e = entryByName(name))
        e->swtpmProcess = p;
}

void VirtualEnvironmentModel::setVirtiofsdProcess(const QString &name, KProcess *p)
{
    if (VmEntry *e = entryByName(name))
        e->virtiofsdProcess = p;
}

void VirtualEnvironmentModel::setQmpClient(const QString &name, QMPClient *c)
{
    if (VmEntry *e = entryByName(name))
        e->qmpClient = c;
}

void VirtualEnvironmentModel::updateConfig(const QString &name, const VMConfig &config)
{
    VmEntry *e = entryByName(name);
    if (!e)
        return;
    e->config = config;
    recomputeIcon(*e);
    emitVmChanged(name);
}

void VirtualEnvironmentModel::appendPerfSample(const QString &name, int channel, double value)
{
    VmEntry *e = entryByName(name);
    if (!e)
        return;

    static constexpr int MaxHistory = 120;
    QVector<double> *hist = nullptr;
    switch (channel) {
    case 0:
        hist = &e->cpuHistory;
        break;
    case 1:
        hist = &e->ramHistory;
        break;
    case 2:
        hist = &e->ioHistory;
        break;
    default:
        return;
    }
    hist->append(value);
    if (hist->size() > MaxHistory)
        hist->removeFirst();
}

void VirtualEnvironmentModel::setHasSavestate(const QString &name, bool exists)
{
    if (VmEntry *e = entryByName(name)) {
        if (e->hasSavestateFile != exists) {
            e->hasSavestateFile = exists;
            refreshSavestateTime(*e);
            emitVmChanged(name);
        }
    }
}

bool VirtualEnvironmentModel::isRunning(const QString &name) const
{
    const VmEntry *e = entryByName(name);
    return e && (e->state == VmEntry::State::Running || e->state == VmEntry::State::Saving);
}

bool VirtualEnvironmentModel::isBusy(const QString &name) const
{
    const VmEntry *e = entryByName(name);
    return e && e->state != VmEntry::State::Stopped;
}

void VirtualEnvironmentModel::clearRuntimeState(const QString &name)
{
    VmEntry *e = entryByName(name);
    if (!e)
        return;

    e->resetRuntimeState();
    recomputeIcon(*e);

    emitVmChanged(name);
    Q_EMIT vmStateChanged(name, VmEntry::State::Stopped);
    updateUptimeTimer();
}

bool VirtualEnvironmentModel::isHeadless(const QString &name) const
{
    const VmEntry *e = entryByName(name);
    return e && e->isHeadless;
}

// -- Folder management --

QModelIndex VirtualEnvironmentModel::createFolder(const QString &name)
{
    for (const auto &f : std::as_const(m_folders))
        if (f.name.compare(name, Qt::CaseInsensitive) == 0)
            return indexForFolder(f.name);

    int insertRow = 0;
    for (int i = 0; i < m_root->children.size(); ++i) {
        const Node *child = m_root->children.at(i);
        if (child->type == Node::FolderNode && m_folders.at(child->folderIndex).name.compare(name, Qt::CaseInsensitive) < 0)
            insertRow = i + 1;
        else
            break;
    }

    FolderEntry folder;
    folder.name = name;
    m_folders.append(folder);

    auto *node = new Node;
    node->type = Node::FolderNode;
    node->folderIndex = m_folders.size() - 1;
    node->parent = m_root;

    beginInsertRows({}, insertRow, insertRow);
    m_root->children.insert(insertRow, node);
    endInsertRows();

    return indexFromNode(node);
}

bool VirtualEnvironmentModel::removeFolder(const QModelIndex &index)
{
    if (!index.isValid())
        return false;
    Node *node = nodeFromIndex(index);
    if (!node || node->type != Node::FolderNode || !node->children.isEmpty())
        return false;

    const int row = m_root->children.indexOf(node);
    if (row < 0)
        return false;

    const int folderIndex = node->folderIndex;

    beginRemoveRows({}, row, row);
    m_root->children.removeAt(row);
    delete node;
    endRemoveRows();

    // Node::folderIndex is a position in m_folders, so dropping an entry
    // shifts every folder behind it.
    m_folders.removeAt(folderIndex);
    for (Node *child : std::as_const(m_root->children)) {
        if (child->type == Node::FolderNode && child->folderIndex > folderIndex)
            --child->folderIndex;
    }

    return true;
}

bool VirtualEnvironmentModel::renameFolder(const QModelIndex &index, const QString &newName)
{
    if (!index.isValid())
        return false;
    Node *node = nodeFromIndex(index);
    if (!node || node->type != Node::FolderNode)
        return false;

    if (m_folders.at(node->folderIndex).name == newName)
        return true;

    // Two folders of one name are indistinguishable to findFolderNode() and to
    // the assignments on disk.
    for (int i = 0; i < m_folders.size(); ++i) {
        if (i != node->folderIndex && m_folders.at(i).name.compare(newName, Qt::CaseInsensitive) == 0)
            return false;
    }

    m_folders[node->folderIndex].name = newName;
    for (const Node *child : std::as_const(node->children)) {
        if (child->type == Node::VmNode)
            m_vms[child->vmIndex].folderName = newName;
    }

    moveFolderNodeIntoOrder(node);

    // `index` still carries the old row if the node has moved.
    const QModelIndex renamed = indexFromNode(node);
    Q_EMIT dataChanged(renamed, renamed);
    return true;
}

void VirtualEnvironmentModel::moveVmToFolder(const QString &vmName, const QString &folderName)
{
    Node *vmNode = findVmNode(vmName);
    Node *folderNode = findFolderNode(folderName);
    if (!vmNode || !folderNode)
        return;

    Node *oldParent = vmNode->parent;
    if (oldParent == folderNode)
        return;

    const QModelIndex oldParentIdx = (oldParent == m_root) ? QModelIndex() : indexFromNode(oldParent);
    const int oldRow = oldParent->children.indexOf(vmNode);

    beginRemoveRows(oldParentIdx, oldRow, oldRow);
    oldParent->children.removeAt(oldRow);
    endRemoveRows();

    const QModelIndex newParentIdx = indexFromNode(folderNode);
    const int newRow = folderNode->children.size();

    beginInsertRows(newParentIdx, newRow, newRow);
    vmNode->parent = folderNode;
    folderNode->children.append(vmNode);
    endInsertRows();

    m_vms[vmNode->vmIndex].folderName = folderName;
}

void VirtualEnvironmentModel::moveVmToRoot(const QString &vmName)
{
    Node *vmNode = findVmNode(vmName);
    if (!vmNode || vmNode->parent == m_root)
        return;

    Node *oldParent = vmNode->parent;
    const QModelIndex oldParentIdx = indexFromNode(oldParent);
    const int oldRow = oldParent->children.indexOf(vmNode);

    beginRemoveRows(oldParentIdx, oldRow, oldRow);
    oldParent->children.removeAt(oldRow);
    endRemoveRows();

    const int newRow = m_root->children.size();
    beginInsertRows({}, newRow, newRow);
    vmNode->parent = m_root;
    m_root->children.append(vmNode);
    endInsertRows();

    m_vms[vmNode->vmIndex].folderName.clear();
}

// -- Persistence --

QMap<QString, QString> VirtualEnvironmentModel::folderAssignments() const
{
    QMap<QString, QString> map;
    for (const auto &vm : m_vms)
        if (!vm.folderName.isEmpty())
            map.insert(vm.name, vm.folderName);
    return map;
}

QStringList VirtualEnvironmentModel::folderNames() const
{
    QStringList names;
    for (const auto &f : m_folders)
        names << f.name;
    return names;
}

QList<MacUsageInfo> VirtualEnvironmentModel::macUsageInfo() const
{
    QList<MacUsageInfo> result;
    result.reserve(m_vms.size());

    for (const VmEntry &e : m_vms) {
        if (e.config.macAddress().isEmpty())
            continue;

        MacUsageInfo info;
        info.vmName = e.name;
        info.mac = e.config.macAddress();
        info.networkMode = e.config.networkMode();
        info.virtualNetworkName = e.config.virtualNetworkName();
        info.dnsHostname = e.config.effectiveDnsHostname();
        result << info;
    }

    return result;
}

QStringList VirtualEnvironmentModel::usedMacAddresses() const
{
    QStringList macs;
    const QList<MacUsageInfo> usage = macUsageInfo();
    macs.reserve(usage.size());

    for (const MacUsageInfo &info : usage)
        macs << info.mac;

    return macs;
}

QMap<QString, QUuid> VirtualEnvironmentModel::vmUuids() const
{
    QMap<QString, QUuid> result;
    for (const VmEntry &e : m_vms) {
        if (!e.config.uuid().isNull())
            result.insert(e.name, e.config.uuid());
    }
    return result;
}

// -- Private helpers --

VirtualEnvironmentModel::Node *VirtualEnvironmentModel::nodeFromIndex(const QModelIndex &index) const
{
    if (!index.isValid())
        return m_root;
    return static_cast<Node *>(index.internalPointer());
}

QModelIndex VirtualEnvironmentModel::indexFromNode(Node *node) const
{
    if (!node || node == m_root)
        return {};
    const Node *p = node->parent;
    if (!p)
        return {};
    const int row = p->children.indexOf(node);
    return (row < 0) ? QModelIndex() : createIndex(row, 0, node);
}

VirtualEnvironmentModel::Node *VirtualEnvironmentModel::findVmNode(const QString &name) const
{
    // The tree is exactly two levels deep: a VM row sits at the root or inside
    // one folder.
    for (Node *child : std::as_const(m_root->children)) {
        if (child->type == Node::VmNode) {
            if (m_vms.at(child->vmIndex).name == name)
                return child;
            continue;
        }
        for (Node *grandChild : std::as_const(child->children)) {
            if (grandChild->type == Node::VmNode && m_vms.at(grandChild->vmIndex).name == name)
                return grandChild;
        }
    }
    return nullptr;
}

VirtualEnvironmentModel::Node *VirtualEnvironmentModel::findFolderNode(const QString &folderName) const
{
    for (Node *child : std::as_const(m_root->children))
        if (child->type == Node::FolderNode && m_folders.at(child->folderIndex).name == folderName)
            return child;
    return nullptr;
}

void VirtualEnvironmentModel::emitVmChanged(const QString &name)
{
    const QModelIndex idx = indexForVm(name);
    if (idx.isValid())
        Q_EMIT dataChanged(idx, idx);
}

void VirtualEnvironmentModel::rebuildTree(const QStringList &folderNames, const QMap<QString, QString> &folderAssignments)
{
    QSet<QString> fNames(folderNames.cbegin(), folderNames.cend());
    fNames.remove(QString());

    // An assignment to a folder missing from the list still gets one, so no VM
    // silently drops back to the root.
    for (auto it = folderAssignments.cbegin(); it != folderAssignments.cend(); ++it)
        if (!it.value().isEmpty())
            fNames.insert(it.value());

    QStringList sorted(fNames.begin(), fNames.end());
    sorted.sort(Qt::CaseInsensitive);

    m_folders.clear();
    QMap<QString, Node *> folderNodeMap;

    for (const QString &name : std::as_const(sorted)) {
        FolderEntry fe;
        fe.name = name;
        m_folders.append(fe);

        auto *node = new Node;
        node->type = Node::FolderNode;
        node->folderIndex = m_folders.size() - 1;
        node->parent = m_root;
        m_root->children.append(node);
        folderNodeMap.insert(name, node);
    }

    for (int i = 0; i < m_vms.size(); ++i) {
        auto *node = new Node;
        node->type = Node::VmNode;
        node->vmIndex = i;

        const QString &folder = m_vms[i].folderName;
        if (!folder.isEmpty() && folderNodeMap.contains(folder)) {
            Node *fn = folderNodeMap[folder];
            node->parent = fn;
            fn->children.append(node);
        } else {
            node->parent = m_root;
            m_root->children.append(node);
        }
    }
}

void VirtualEnvironmentModel::moveFolderNodeIntoOrder(Node *node)
{
    const int from = m_root->children.indexOf(node);
    if (from < 0)
        return;

    // Folder nodes are kept sorted and always precede the VM rows.
    int folderCount = 0;
    while (folderCount < m_root->children.size() && m_root->children.at(folderCount)->type == Node::FolderNode)
        ++folderCount;

    const QString name = m_folders.at(node->folderIndex).name;

    int target = 0;
    for (int i = 0; i < folderCount; ++i) {
        if (i == from)
            continue;
        if (m_folders.at(m_root->children.at(i)->folderIndex).name.compare(name, Qt::CaseInsensitive) < 0)
            ++target;
        else
            break;
    }

    if (target == from)
        return;

    // beginMoveRows() wants the destination in pre-move coordinates: one past
    // the target when moving down, the target itself when moving up.
    const int destination = (target > from) ? target + 1 : target;
    if (!beginMoveRows({}, from, from, {}, destination))
        return;

    m_root->children.move(from, target);
    endMoveRows();
}

void VirtualEnvironmentModel::recomputeIcon(VmEntry &e) const
{
    VMConfig::VmVisualState vs = VMConfig::VmVisualState::Stopped;
    // Deploying is a live VM: QEMU is up and the guest is installing itself.
    if (e.state == VmEntry::State::Running || e.state == VmEntry::State::Deploying)
        vs = VMConfig::VmVisualState::Running;
    else if (e.state == VmEntry::State::Saving || e.state == VmEntry::State::Saved)
        vs = VMConfig::VmVisualState::Saved;

    e.styledIcon = VMConfig::styledVmIcon(e.config.icon(KEMUConst::ListViewIconSize), vs, e.isHeadless, KEMUConst::ListViewIconSize);
}

void VirtualEnvironmentModel::refreshSavestateTime(VmEntry &e) const
{
    e.savestateTime = QDateTime();
    if (!e.hasSavestateFile)
        return;

    // QDir::isEmpty() asks the filesystem whether the directory has entries;
    // path().isEmpty() is what we actually want here.
    if (e.path.path().isEmpty()) {
        qCWarning(KEMU) << "VmEntry" << e.name << "has no path; savestate time unavailable";
        return;
    }

    const QFileInfo fi(SaveStateManager::savestateFilePath(e.path.absolutePath(), e.name));
    if (fi.exists())
        e.savestateTime = fi.lastModified();
}

void VirtualEnvironmentModel::updateUptimeTimer()
{
    bool anyRunning = false;
    for (const auto &vm : std::as_const(m_vms)) {
        if (vm.state == VmEntry::State::Running) {
            anyRunning = true;
            break;
        }
    }

    if (anyRunning && !m_uptimeTimer->isActive())
        m_uptimeTimer->start();
    else if (!anyRunning && m_uptimeTimer->isActive())
        m_uptimeTimer->stop();
}

void VirtualEnvironmentModel::tickUptime()
{
    for (const auto &vm : std::as_const(m_vms)) {
        if (vm.state != VmEntry::State::Running)
            continue;
        const QModelIndex idx = indexForVm(vm.name);
        if (idx.isValid())
            Q_EMIT dataChanged(idx, idx, {SubtitleRole, Qt::ToolTipRole});
    }
}
