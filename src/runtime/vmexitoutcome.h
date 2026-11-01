/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <KProcess>

struct VmExitOutcome {
    bool abnormal = false;
    bool markSaved = false;

    static VmExitOutcome fromProcess(int exitCode, KProcess::ExitStatus status, bool deployment, bool savestatePresent)
    {
        const bool abnormal = status != KProcess::NormalExit || exitCode != 0;
        return {abnormal, !deployment && savestatePresent && !abnormal};
    }
};
