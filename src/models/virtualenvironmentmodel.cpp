/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "models/virtualenvironmentmodel.h"
#include "const.h"

VirtualEnvironmentModel::VirtualEnvironmentModel(ImageDatabase *imageDb, QObject *parent)
    : QAbstractItemModel(parent)
    , m_imageDatabase(imageDb)
    , m_root(new Node)
{
    m_root->type = Node::RootNode;
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
        case Qt::DecorationRole: {
            VMConfig::VmVisualState vs = VMConfig::VmVisualState::Stopped;
            if (vm.state == VmEntry::State::Running)
                vs = VMConfig::VmVisualState::Running;
            else if (vm.state == VmEntry::State::Saving || vm.state == VmEntry::State::Saved)
                vs = VMConfig::VmVisualState::Saved;
            return VMConfig::styledVmIcon(vm.config.icon(KEMUConst::ListViewIconSize), vs, vm.isHeadless, KEMUConst::ListViewIconSize);
        }
        case ItemTypeRole:
            return Vm;
        case VmNameRole:
            return vm.name;
        case SubtitleRole: {
            if (vm.state == VmEntry::State::Deploying)
                return i18n("Deploying...");
            if (vm.state == VmEntry::State::Cloning)
                return i18n("Cloning...");
            if (vm.state == VmEntry::State::Saving)
                return i18n("Saving state...");
            if (vm.state == VmEntry::State::Saved)
                return i18n("Saved");
            QString label = VMConfig::toUIString(vm.config.osType());
            if (!vm.config.distroId().isEmpty() && vm.config.distroId() != u"other"_s)
                label += u" \u2014 "_s + m_imageDatabase->displayName(vm.config.distroId());
            return label;
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

void VirtualEnvironmentModel::loadFromDisk(const QDir &baseDir,
                                           const QMap<QString, QString> &folderAssignments,
                                           std::function<VMConfig(const QString &)> loader)
{
    beginResetModel();

    m_vms.clear();
    m_folders.clear();
    delete m_root;
    m_root = new Node;
    m_root->type = Node::RootNode;

    const QStringList vmDirs = baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    for (const QString &d : vmDirs) {
        QString vmPath = baseDir.filePath(d);
        if (!QFile::exists(vmPath + u"/config.json"_s))
            continue;

        VmEntry entry;
        entry.name = d;
        entry.config = loader(vmPath);
        entry.folderName = folderAssignments.value(d);
        m_vms.append(std::move(entry));
    }

    rebuildTree(folderAssignments);
    endResetModel();
}

void VirtualEnvironmentModel::reload(const QDir &baseDir, const QMap<QString, QString> &folderAssignments, std::function<VMConfig(const QString &)> loader)
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

        VmEntry entry;
        entry.name = d;
        entry.config = loader(vmPath);
        entry.folderName = folderAssignments.value(d);

        if (stateBackup.contains(d)) {
            const VmEntry &old = stateBackup[d];
            entry.state = old.state;
            entry.qemuProcess = old.qemuProcess;
            entry.swtpmProcess = old.swtpmProcess;
            entry.virtiofsdProcess = old.virtiofsdProcess;
            entry.passtProcess = old.passtProcess;
            entry.spiceViewerProcess = old.spiceViewerProcess;
            entry.qmpClient = old.qmpClient;
            entry.cpuHistory = old.cpuHistory;
            entry.ramHistory = old.ramHistory;
            entry.ioHistory = old.ioHistory;
            entry.ioPeakBytesPerSec = old.ioPeakBytesPerSec;
        }

        m_vms.append(std::move(entry));
    }

    rebuildTree(folderAssignments);
    endResetModel();
}

// -- State management --

void VirtualEnvironmentModel::setVmState(const QString &name, VmEntry::State state)
{
    VmEntry *e = entryByName(name);
    if (!e || e->state == state)
        return;
    e->state = state;
    emitVmChanged(name);
    Q_EMIT vmStateChanged(name, state);
}

void VirtualEnvironmentModel::setHeadless(const QString &name, const bool headless)
{
    if (VmEntry *e = entryByName(name))
        e->isHeadless = headless;
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

void VirtualEnvironmentModel::setPasstProcess(const QString &name, KProcess *process)
{
    if (VmEntry *e = entryByName(name))
        e->passtProcess = process;
}

void VirtualEnvironmentModel::setSpiceViewerProcess(const QString &name, KProcess *process)
{
    if (VmEntry *e = entryByName(name))
        e->spiceViewerProcess = process;
}

void VirtualEnvironmentModel::setDBusDaemonProcess(const QString &name, KProcess *process)
{
    if (VmEntry *e = entryByName(name))
        e->dbusDaemonProcess = process;
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

    e->state = VmEntry::State::Stopped;
    e->qemuProcess = nullptr;
    e->swtpmProcess = nullptr;
    e->virtiofsdProcess = nullptr;
    e->passtProcess = nullptr;
    e->spiceViewerProcess = nullptr;
    e->qmpClient = nullptr;
    e->cpuHistory.clear();
    e->ramHistory.clear();
    e->ioHistory.clear();
    e->ioPeakBytesPerSec = 0.0;
    e->attachedUsb.clear();

    emitVmChanged(name);
    Q_EMIT vmStateChanged(name, VmEntry::State::Stopped);
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
        if (f.name == name)
            return indexForFolder(name);

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
    const Node *node = nodeFromIndex(index);
    if (!node || node->type != Node::FolderNode || !node->children.isEmpty())
        return false;

    const int row = m_root->children.indexOf(node);
    if (row < 0)
        return false;

    beginRemoveRows({}, row, row);
    m_root->children.removeAt(row);
    delete node;
    endRemoveRows();
    return true;
}

bool VirtualEnvironmentModel::renameFolder(const QModelIndex &index, const QString &newName)
{
    if (!index.isValid())
        return false;
    Node *node = nodeFromIndex(index);
    if (!node || node->type != Node::FolderNode)
        return false;

    m_folders[node->folderIndex].name = newName;
    for (const Node *child : std::as_const(node->children)) {
        if (child->type == Node::VmNode)
            m_vms[child->vmIndex].folderName = newName;
    }
    Q_EMIT dataChanged(index, index);
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
    std::function<Node *(Node *)> search = [&](Node *parent) -> Node * {
        for (Node *child : std::as_const(parent->children)) {
            if (child->type == Node::VmNode && m_vms.at(child->vmIndex).name == name)
                return child;
            if (child->type == Node::FolderNode)
                if (Node *found = search(child))
                    return found;
        }
        return nullptr;
    };
    return search(m_root);
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

void VirtualEnvironmentModel::rebuildTree(const QMap<QString, QString> &folderAssignments)
{
    QSet<QString> fNames;
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
