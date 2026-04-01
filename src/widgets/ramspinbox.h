/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDoubleSpinBox>
#include <QLocale>
#include <QRegularExpression>

#include <KLocalizedString>

using namespace Qt::StringLiterals;

class RamSpinBox : public QDoubleSpinBox {

    Q_OBJECT

public:
    explicit RamSpinBox(QWidget* parent = nullptr)
        : QDoubleSpinBox(parent)
    {
        setDecimals(2);
        setRange(16.0, 131072.0);
        setSingleStep(128.0);
    }

protected:
    QString textFromValue(double value) const override
    {
        if (value >= 1024.0) {
            const double gb = value / 1024.0;
            QString str = locale().toString(gb, 'f', 2);

            const QString decPoint = locale().decimalPoint();
            while (str.contains(decPoint) && str.endsWith(u'0')) {
                str.chop(1);
            }
            if (str.endsWith(decPoint)) {
                str.chop(1);
            }
            return str + i18n(" GiB");
        } else {
            return locale().toString(value, 'f', 0) + i18n(" MiB");
        }
    }

    double valueFromText(const QString& text) const override
    {
        QString str = text.trimmed().toUpper();
        double multiplier = 1.0;

        if (str.endsWith(i18n("GiB")) || str.endsWith(i18n("G")) || str.endsWith(i18n("GB"))) {
            multiplier = 1024.0;
            str.remove(QRegularExpression(u"[A-Z\\s]"_s));
        } else if (str.endsWith(i18n("MiB")) || str.endsWith(i18n("M")) || str.endsWith(i18n("MB"))) {
            multiplier = 1.0;
            str.remove(QRegularExpression(u"[A-Z\\s]"_s));
        }

        bool ok;
        double val = locale().toDouble(str, &ok);
        if (!ok) {
            val = str.toDouble();
        }

        return val * multiplier;
    }

    QValidator::State validate(QString& input, int& pos) const override
    {
        Q_UNUSED(input);
        Q_UNUSED(pos);
        return QValidator::Acceptable;
    }
};
