/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QObject>

#include <functional>

/**
 * Sequentially runs asynchronous steps that each report success or
 * failure through a callback. Stops at the first failure and invokes
 * a single finally-callback with the overall result.
 *
 * Each step receives a `done(bool)` continuation that it must call
 * exactly once when the step is finished. Steps may complete
 * synchronously or asynchronously.
 *
 * The chain self-deletes via deleteLater() after the finally-callback
 * fires, so callers do not have to track it. If `parent` is destroyed
 * during a pending step, the chain dies with it; the finally-callback
 * will not fire in that case.
 */
class StartupChain : public QObject
{
    Q_OBJECT

public:
    using Step = std::function<void(std::function<void(bool ok)> done)>;
    using FinishedCallback = std::function<void(bool ok)>;

    explicit StartupChain(QObject *parent);

    /// Append a step. Must be called before run().
    StartupChain &then(Step step);

    /// Begin executing the chain. The finally-callback fires once
    /// after the last step succeeds, or as soon as any step fails.
    void run(FinishedCallback finally = {});

private:
    void runNext();

    QList<Step> m_steps;
    int m_index = 0;
    FinishedCallback m_finally;
    bool m_done = false;
};
