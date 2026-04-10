#pragma once

#include <QApplication>
#include <QClipboard>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVirtualObject>
#include <QDebug>
#include <QMimeData>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

using namespace Qt::StringLiterals;

enum class ClipboardDirection {
    Off,
    ToHost, // Guest -> Host
    ToGuest, // Host -> Guest
    Both
};

// =============================================================================
// ClipboardDBusHandler – receives ALL D-Bus calls from QEMU
// =============================================================================
// QDBusVirtualObject instead of QDBusAbstractAdaptor, because:
//   - No signature matching necessary (QtDBus drops calls on mismatch)
//   - Every call is logged -> easy debugging
//   - Arguments are manually read from QDBusMessage -> robust
// =============================================================================

class QemuClipboardManager;

class ClipboardDBusHandler : public QDBusVirtualObject {

    Q_OBJECT

public:
    ClipboardDBusHandler(QemuClipboardManager* manager, QObject* parent = nullptr);

    QString introspect(const QString& path) const override;
    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override;

private:
    QemuClipboardManager* m_manager;
};

// =============================================================================
// QemuClipboardManager
// =============================================================================

class QemuClipboardManager : public QObject {

    Q_OBJECT

public:
    QemuClipboardManager(QDBusConnection& bus,
        ClipboardDirection direction,
        QObject* parent = nullptr);
    ~QemuClipboardManager() override;

    bool setup();

    // Called by ClipboardDBusHandler:
    void handleGrab(uint selection, uint serial, const QStringList& mimes);
    void handleRelease(uint selection);
    QByteArray handleRequest(uint selection, const QStringList& mimes);

private Q_SLOTS:
    void onHostClipboardChanged();
    void doRegister();
    void onRequestFinished(QDBusPendingCallWatcher* watcher);

private:
    QDBusConnection m_bus;
    QDBusInterface* m_qemuClipboard = nullptr;
    ClipboardDBusHandler* m_handler = nullptr;
    QClipboard* m_hostClipboard;
    ClipboardDirection m_direction;

    uint m_serial = 0;
    bool m_registered = false;

    // Feedback loop prevention:
    // Under Wayland, setText() triggers MULTIPLE dataChanged signals.
    // A boolean is not enough – we remember the text we
    // last received from the guest and set on the host.
    // Every dataChanged with identical text is ignored.
    QString m_lastTextFromGuest;

    // Host ownership: true if WE last sent a Grab to QEMU
    // (Host owns the clipboard). Becomes false when QEMU sends us a Grab.
    bool m_hostOwnsClipboard = false;
    QPointer<QTimer> m_ownershipTimer;
};
