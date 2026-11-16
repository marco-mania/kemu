/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// A guest application for the X11 end-to-end test, which has no xclip:
//   clipboardtool get <clipboard|primary> <type>         data to stdout
//   clipboardtool types <clipboard|primary>              one type per line
//   clipboardtool set <clipboard|primary> <type> <file>  holds the selection
//                                                        until another takes it

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QMimeData>
#include <QTimer>

#include <cstdio>

using namespace Qt::StringLiterals;

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 3)
        return 2;
    const QClipboard::Mode mode = args.at(2) == u"primary"_s ? QClipboard::Selection : QClipboard::Clipboard;
    QClipboard *clipboard = QGuiApplication::clipboard();

    if (args.at(1) == u"get"_s && args.size() == 4) {
        const QMimeData *data = clipboard->mimeData(mode);
        if (!data || !data->hasFormat(args.at(3)))
            return 1;
        const QByteArray bytes = data->data(args.at(3));
        std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
        return 0;
    }
    if (args.at(1) == u"types"_s) {
        const QMimeData *data = clipboard->mimeData(mode);
        if (!data)
            return 1;
        for (const QString &format : data->formats())
            std::printf("%s\n", qPrintable(format));
        return 0;
    }
    if (args.at(1) == u"set"_s && args.size() == 5) {
        QFile file(args.at(4));
        if (!file.open(QIODevice::ReadOnly))
            return 2;
        auto *data = new QMimeData;
        data->setData(args.at(3), file.readAll());
        clipboard->setMimeData(data, mode);
        QObject::connect(clipboard, &QClipboard::changed, &app, [&](QClipboard::Mode changed) {
            if (changed == mode && !(mode == QClipboard::Clipboard ? clipboard->ownsClipboard() : clipboard->ownsSelection()))
                app.quit();
        });
        QTimer::singleShot(60000, &app, &QGuiApplication::quit);
        std::printf("ready\n");
        std::fflush(stdout);
        return app.exec();
    }
    return 2;
}
