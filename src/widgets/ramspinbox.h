/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "widgets/steppedspinbox.h"

#include <KLocalizedString>

using namespace Qt::StringLiterals;

/**
 * @brief Spin box for a memory size, held in MiB.
 *
 * Shows MiB below 1 GiB and GiB above it, and accepts either unit on input --
 * translated or not, with or without a space. A bare number means MiB.
 *
 * Owns the canonical list of memory steps so the paired sliders in the VM
 * dialogs cannot drift apart from it.
 */
class RamSpinBox : public SteppedSpinBox
{
    Q_OBJECT

public:
    explicit RamSpinBox(QWidget *parent = nullptr)
        : SteppedSpinBox(parent)
    {
        setRange(steps().first(), steps().last());
        setSteps(steps());

        // Interpret the text on Enter or focus-out only. Otherwise every
        // keystroke would move the paired slider.
        setKeyboardTracking(false);
        setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }

    /// Memory sizes in MiB offered by the arrows and the paired sliders.
    static const QList<int> &steps()
    {
        static const QList<int> s = {16,   32,   64,   128,   256,   512,   768,   1024,  1536,  2048,  3072,
                                     4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152, 65536, 98304, 131072};
        return s;
    }

protected:
    QString textFromValue(int value) const override
    {
        if (value < 1024)
            return locale().toString(value) + unitMib();

        QString str = locale().toString(value / 1024.0, 'f', 2);

        const QString decPoint = locale().decimalPoint();
        while (str.contains(decPoint) && str.endsWith(u'0'))
            str.chop(1);
        if (str.endsWith(decPoint))
            str.chop(1);

        return str + unitGib();
    }

    int valueFromText(const QString &text) const override
    {
        const Parsed parsed = parse(text);

        bool ok = false;
        double number = locale().toDouble(parsed.number, &ok);
        if (!ok)
            number = parsed.number.toDouble(&ok);
        if (!ok)
            return value();

        return qRound(parsed.gib ? number * 1024.0 : number);
    }

    QValidator::State validate(QString &input, int &pos) const override
    {
        Q_UNUSED(pos)

        const Parsed parsed = parse(input);
        if (parsed.number.isEmpty() || !parsed.unitKnown)
            return QValidator::Intermediate;

        bool ok = false;
        locale().toDouble(parsed.number, &ok);
        if (!ok)
            parsed.number.toDouble(&ok);

        return ok ? QValidator::Acceptable : QValidator::Intermediate;
    }

private:
    struct Parsed {
        QString number;
        bool gib = false;
        bool unitKnown = true;
    };

    static QString unitMib()
    {
        return i18nc("Memory size in mebibytes, keep the leading space", " MiB");
    }

    static QString unitGib()
    {
        return i18nc("Memory size in gibibytes, keep the leading space", " GiB");
    }

    static Parsed parse(QStringView text)
    {
        Parsed out;
        QStringView str = text.trimmed();

        // Scan backwards over the ASCII-letter suffix to find where the unit starts
        qsizetype unitStart = str.size();
        while (unitStart > 0) {
            const ushort u = str.at(unitStart - 1).unicode();
            const bool letter = (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z');
            if (!letter)
                break;
            --unitStart;
        }

        out.number = str.left(unitStart).trimmed().toString();

        if (unitStart < str.size()) {
            const QStringView unit(str.constData() + unitStart, str.size() - unitStart);
            const auto eq = [unit](auto s) {
                return unit.compare(s, Qt::CaseInsensitive) == 0;
            };

            if (eq(u"g"_s) || eq(u"gb"_s) || eq(u"gib"_s) || eq(unitGib()))
                out.gib = true;
            else if (!eq(u"m"_s) && !eq(u"mb"_s) && !eq(u"mib"_s) && !eq(unitMib()))
                out.unitKnown = false;
        }

        return out;
    }
};
