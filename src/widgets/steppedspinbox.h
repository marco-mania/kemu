/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QSpinBox>

/**
 * @brief Spin box whose arrows walk a fixed list of values.
 *
 * Free input stays possible; snapToStep() puts an off-list value back onto the
 * nearest entry, so a paired slider can never disagree with the box.
 */
class SteppedSpinBox : public QSpinBox
{
    Q_OBJECT

public:
    explicit SteppedSpinBox(QWidget *parent = nullptr)
        : QSpinBox(parent)
    {
    }

    void setSteps(const QList<int> &steps)
    {
        m_steps = steps;
    }

    [[nodiscard]] int nearestStep(int value) const
    {
        int best = value;
        int bestDiff = std::numeric_limits<int>::max();
        for (const int step : m_steps) {
            const int diff = std::abs(step - value);
            if (diff < bestDiff) {
                bestDiff = diff;
                best = step;
            }
        }
        return best;
    }

public Q_SLOTS:
    void snapToStep()
    {
        if (!m_steps.isEmpty())
            setValue(nearestStep(value()));
    }

protected:
    void stepBy(int steps) override
    {
        if (m_steps.isEmpty() || steps == 0) {
            QSpinBox::stepBy(steps);
            return;
        }

        int index = m_steps.indexOf(nearestStep(value()));

        // An off-list value must move to the next step in the requested
        // direction, not back to the nearest one.
        if (steps > 0 && m_steps.at(index) <= value())
            ++index;
        else if (steps < 0 && m_steps.at(index) >= value())
            --index;

        index += (steps > 0) ? steps - 1 : steps + 1;
        setValue(m_steps.at(qBound(0, index, static_cast<int>(m_steps.size()) - 1)));
    }

private:
    QList<int> m_steps;
};
