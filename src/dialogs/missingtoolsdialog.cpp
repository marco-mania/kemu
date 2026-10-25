/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "missingtoolsdialog.h"

#include "utils/sandboxcapabilities.h"

MissingToolsDialog::MissingToolsDialog(const QList<ExternalTools::FeatureGroup> &missing, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(i18nc("@title:window", "Missing Tools"));

    auto *layout = new QVBoxLayout(this);

    const bool editablePaths = SandboxCapabilities::instance().has(SandboxCapabilities::ExternalToolPaths);

    auto *intro = new QLabel(editablePaths ? i18n("The following features are unavailable because their "
                                                  "external helper programs were not found on this system:")
                                           : i18n("The following features are unavailable in this build:"));

    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *textBrowser = new QTextBrowser(this);
    textBrowser->setOpenExternalLinks(false);

    // Embed theme icons as inline images so HTML can reference them.
    QTextDocument *doc = textBrowser->document();
    const QPixmap warnPix = QIcon::fromTheme(u"dialog-warning"_s).pixmap(22, 22);
    const QPixmap errPix = QIcon::fromTheme(u"dialog-error"_s).pixmap(22, 22);
    doc->addResource(QTextDocument::ImageResource, QUrl(u"kemu://icon-warn"_s), warnPix);
    doc->addResource(QTextDocument::ImageResource, QUrl(u"kemu://icon-err"_s), errPix);

    const auto &tools = ExternalTools::instance();

    QString html =
        u"<style>"
        u"  .group { margin-bottom: 12px; }"
        u"  .header { font-weight: bold; font-size: 110%; }"
        u"  .body { margin-left: 30px; }"
        u"  .missing { color: palette(mid); }"
        u"  .required { color: palette(mid); font-style: italic; }"
        u"</style>"_s;

    const QList<ExternalTools::Tool> unused = ExternalTools::instance().unusedTools();

    for (const auto &group : missing) {
        const QString iconSrc = group.suppressible ? u"kemu://icon-warn"_s : u"kemu://icon-err"_s;

        QStringList missingNames;
        for (auto t : group.tools) {
            if (!unused.contains(t) && !tools.isAvailable(t))
                missingNames << ExternalTools::displayName(t);
        }

        if (missingNames.isEmpty())
            continue; // nothing left to report for this group

        html += u"<div class='group'>"_s;
        html += u"<div class='header'><img src='%1' style='vertical-align:middle;'/>&nbsp;%2</div>"_s.arg(iconSrc, group.name.toHtmlEscaped());
        html += u"<div class='body'>%1</div>"_s.arg(group.description.toHtmlEscaped());
        html += u"<div class='body missing'>%1 <i>%2</i></div>"_s.arg(i18n("Missing:"), missingNames.join(u", "_s).toHtmlEscaped());
        if (!group.suppressible) {
            html +=
                u"<div class='body required'>%1</div>"_s.arg(i18n("This warning cannot be disabled because the affected tools are "
                                                                  "required."));
        }
        html += u"</div>"_s;
    }

    textBrowser->setHtml(html);
    layout->addWidget(textBrowser, 1);

    // Suppression controls for the suppressible subset.
    QList<ExternalTools::FeatureGroup> suppressible;
    for (const auto &g : missing) {
        if (g.suppressible)
            suppressible.append(g);
    }
    if (!suppressible.isEmpty()) {
        auto *suppressBox = new QGroupBox(i18n("Don't warn me again about:"), this);
        auto *suppressLayout = new QVBoxLayout(suppressBox);
        for (const auto &g : suppressible) {
            auto *cb = new QCheckBox(g.name, suppressBox);
            cb->setChecked(ExternalTools::isWarningSuppressed(g.id));
            connect(cb, &QCheckBox::toggled, this, [id = g.id](bool checked) {
                ExternalTools::setWarningSuppressed(id, checked);
            });
            suppressLayout->addWidget(cb);
        }
        layout->addWidget(suppressBox);
    }

    auto *buttons = new QDialogButtonBox(this);
    if (editablePaths) {
        auto *settingsBtn = buttons->addButton(i18n("Open Settings…"), QDialogButtonBox::AcceptRole);
        settingsBtn->setIcon(QIcon::fromTheme(u"configure"_s));
        connect(settingsBtn, &QPushButton::clicked, this, [this]() {
            m_openSettings = true;
        });
    }
    buttons->addButton(QDialogButtonBox::Close);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    layout->addWidget(buttons);
    resize(520, 480);
}
