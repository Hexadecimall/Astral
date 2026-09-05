#include "views/decompilerview.hh"
#include "views/codeview.hh"

#include <QFontDatabase>
#include <QLabel>
#include <QToolButton>
#include <QScrollBar>
#include <QStyle>
#include <QVBoxLayout>

namespace astral::gui {

DecompilerView::DecompilerView(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *headerRow = new QWidget;
    headerRow->setObjectName(QStringLiteral("decompilerHeader"));
    headerRow->setAttribute(Qt::WA_StyledBackground, true);
    auto *headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(12, 6, 12, 6);
    header_ = new QLabel;
    header_->setObjectName(QStringLiteral("decompilerName"));
    header_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    detail_ = new QLabel;
    detail_->setObjectName(QStringLiteral("muted"));
    headerLayout->addWidget(header_);
    headerLayout->addStretch(1);
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("muted"));
    headerLayout->addWidget(status_);
    headerLayout->addWidget(detail_);
    // The code is a document to work in: edit it, check that it still
    // compiles against Astral's runtime, or go back to what was recovered.
    revert_ = new QToolButton;
    revert_->setObjectName(QStringLiteral("headerButton"));
    revert_->setText(tr("Revert"));
    revert_->setEnabled(false);
    compile_ = new QToolButton;
    compile_->setObjectName(QStringLiteral("headerButton"));
    compile_->setText(tr("Patch"));
    compile_->setToolTip(tr("Compile this C for the program and write it over the function"));
    headerLayout->addSpacing(8);
    headerLayout->addWidget(revert_);
    headerLayout->addWidget(compile_);
    connect(compile_, &QToolButton::clicked, this, [this] { Q_EMIT compileRequested(code_->toPlainText()); });
    connect(revert_, &QToolButton::clicked, this, [this] { render(); });
    layout->addWidget(headerRow);

    code_ = new CodeView;
    code_->setEditable(true);
    new CHighlighter(code_->document());
    layout->addWidget(code_, 1);
    connect(code_->document(), &QTextDocument::modificationChanged, this, [this](bool modified) {
        revert_->setEnabled(modified);
        status_->setText(modified ? tr("edited") : QString());
        Q_EMIT modifiedChanged(modified);
    });
    showEmpty(tr("Choose a function"));
}

void DecompilerView::showPending(const QString &name, quint64 address)
{
    header_->setText(name);
    detail_->setText(QStringLiteral("0x%1  ·  %2").arg(address, 0, 16).arg(tr("decompiling")));
}

void DecompilerView::showFunction(const Decompiled &function)
{
    current_ = function;
    header_->setText(function.signature.isEmpty() ? function.name : function.signature);
    detail_->setText(QStringLiteral("0x%1  ·  %2 bytes").arg(function.address, 0, 16).arg(function.size));
    detail_->setToolTip(function.namingReason);
    render();
}

void DecompilerView::setPseudo(bool pseudo)
{
    pseudo_ = pseudo;
    // The engine's listing is reference material, not something to compile.
    compile_->setVisible(!pseudo);
    revert_->setVisible(!pseudo);
    code_->setEditable(!pseudo);
    render();
}

QString DecompilerView::text() const
{
    return code_->toPlainText();
}

void DecompilerView::setText(const QString &text)
{
    code_->setPlainText(text);
}

bool DecompilerView::modified() const
{
    return code_->document()->isModified();
}

void DecompilerView::showCompileResult(bool ok, int errors)
{
    status_->setText(ok ? tr("compiling") : tr("%n error(s)", nullptr, errors));
    status_->setObjectName(ok ? QStringLiteral("muted") : QStringLiteral("error"));
    status_->setToolTip(QString());
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
}

void DecompilerView::showPatched()
{
    status_->setText(tr("patched, see Log"));
    status_->setObjectName(QStringLiteral("success"));
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
}

void DecompilerView::showRefused(const QString &reason)
{
    status_->setText(tr("refused"));
    status_->setToolTip(reason);
    status_->setObjectName(QStringLiteral("warning"));
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
}

void DecompilerView::render()
{
    if (current_.code.isEmpty() && current_.pseudoCode.isEmpty())
        return;
    QString text = pseudo_ ? current_.pseudoCode : current_.code;
    if (pseudo_ && !current_.namingReason.isEmpty())
        text.prepend(QStringLiteral("/* %1 */\n").arg(current_.namingReason));
    const int scroll = code_->verticalScrollBar()->value();
    code_->setPlainText(text);
    code_->document()->setModified(false);
    code_->verticalScrollBar()->setValue(scroll);
}

void DecompilerView::showError(const QString &error)
{
    detail_->setText(tr("failed"));
    code_->setPlainText(QStringLiteral("/* %1 */").arg(error));
}

void DecompilerView::showEmpty(const QString &message)
{
    current_ = Decompiled();
    header_->clear();
    detail_->clear();
    code_->setPlainText(QString());
    code_->setPlaceholderText(message);
}

} // namespace astral::gui
