#include "views/decompilerview.hh"
#include "views/codeview.hh"

#include <QFontDatabase>
#include <QLabel>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QTimer>
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
    layout->addWidget(headerRow);

    code_ = new CodeView;
    code_->setEditable(true);
    new CHighlighter(code_->document());
    layout->addWidget(code_, 1);

    // A pill over the code while the engine works, so a slow function is
    // visibly in progress rather than apparently ignored.
    busy_ = new QLabel(code_);
    busy_->setObjectName(QStringLiteral("busyPill"));
    busy_->setAttribute(Qt::WA_TransparentForMouseEvents);
    busy_->hide();
    busyTimer_ = new QTimer(this);
    busyTimer_->setInterval(300);
    connect(busyTimer_, &QTimer::timeout, this, [this] {
        busyTick_ = (busyTick_ + 1) % 4;
        busy_->setText(busyVerb_ + QString(busyTick_, QLatin1Char('.')).leftJustified(3, QLatin1Char(' ')));
        busy_->adjustSize();
        busy_->move((code_->width() - busy_->width()) / 2, 24);
    });
    connect(code_->document(), &QTextDocument::modificationChanged, this, [this](bool modified) {
        status_->setText(modified ? tr("edited") : QString());
        Q_EMIT modifiedChanged(modified);
    });
    showEmpty(tr("Choose a function"));
}

void DecompilerView::showPending(const QString &name, quint64 address)
{
    header_->setText(name);
    detail_->setText(QStringLiteral("0x%1").arg(address, 0, 16));
    setBusy(true);
}

void DecompilerView::setBusy(bool busy, const QString &verb)
{
    if (busy) {
        busyVerb_ = verb.isEmpty() ? tr("Decompiling") : verb;
        busyTick_ = 0;
        busy_->setText(busyVerb_ + QStringLiteral("   "));
        busy_->adjustSize();
        busy_->move((code_->width() - busy_->width()) / 2, 24);
        busy_->show();
        busy_->raise();
        busyTimer_->start();
    } else {
        busyTimer_->stop();
        busy_->hide();
    }
}

void DecompilerView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (busy_->isVisible())
        busy_->move((code_->width() - busy_->width()) / 2, 24);
}

void DecompilerView::showFunction(const Decompiled &function)
{
    setBusy(false);
    current_ = function;
    detail_->setText(QStringLiteral("0x%1  ·  %2 bytes").arg(function.address, 0, 16).arg(function.size));
    detail_->setToolTip(function.namingReason);
    render();
}

void DecompilerView::setPseudo(bool pseudo)
{
    pseudo_ = pseudo;
    // Both views are documents to work in. The engine's listing names types by
    // width, so patching from it usually needs those spellings replaced first;
    // the attempt says so plainly rather than the button being missing.
    code_->setEditable(true);
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

void DecompilerView::showPatching()
{
    status_->clear();
    setBusy(true, tr("Patching"));
}

void DecompilerView::showCompileResult(bool ok, int errors)
{
    setStatus(ok ? QString() : tr("%n error(s)", nullptr, errors),
              ok ? QStringLiteral("muted") : QStringLiteral("error"));
}

void DecompilerView::setStatus(const QString &text, const QString &kind, const QString &tip)
{
    setBusy(false);
    status_->setText(text);
    status_->setToolTip(tip);
    status_->setObjectName(kind);
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
}

void DecompilerView::showPatchQueued()
{
    setStatus(tr("patch queued"), QStringLiteral("warning"),
              tr("The edit is in the patch queue. Nothing on disk has changed yet."));
}

void DecompilerView::showPatchWritten()
{
    setStatus(tr("patched"), QStringLiteral("success"), tr("The patched binary was written."));
}

void DecompilerView::showRefused(const QString &reason)
{
    setStatus(tr("refused"), QStringLiteral("error"), reason);
}

namespace {

// The line that opens `name`'s definition in `source`. The two views print
// different signatures for the same function, so the header quotes whichever
// text is on screen rather than one fixed form.
QString definitionLine(const QString &source, const QString &name)
{
    if (name.isEmpty())
        return QString();
    const QStringList lines = source.split(QLatin1Char('\n'));
    const QRegularExpression declares(
        QStringLiteral(R"((^|[^A-Za-z0-9_])%1\s*\()").arg(QRegularExpression::escape(name)));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines[i];
        const QString trimmed = line.trimmed();
        // A definition is not a call, a prototype, or a comment about one.
        if (trimmed.isEmpty() || trimmed.endsWith(QLatin1Char(';')) || trimmed.startsWith(QLatin1Char('*'))
            || trimmed.startsWith(QStringLiteral("/*")) || trimmed.startsWith(QStringLiteral("//")))
            continue;
        if (!declares.match(line).hasMatch())
            continue;
        // The body opens on this line or on the next line that holds
        // anything; the emitted C leaves a blank line before the brace.
        bool bodyFollows = trimmed.endsWith(QLatin1Char('{'));
        for (int j = i + 1; !bodyFollows && j < lines.size(); ++j) {
            const QString next = lines[j].trimmed();
            if (next.isEmpty())
                continue;
            bodyFollows = next.startsWith(QLatin1Char('{'));
            break;
        }
        if (!bodyFollows)
            continue;
        QString signature = trimmed;
        if (signature.endsWith(QLatin1Char('{')))
            signature.chop(1);
        return signature.trimmed();
    }
    return QString();
}

} // namespace

void DecompilerView::render()
{
    if (current_.code.isEmpty() && current_.pseudoCode.isEmpty())
        return;
    QString text = pseudo_ ? current_.pseudoCode : current_.code;
    const QString shown = definitionLine(text, current_.name);
    header_->setText(shown.isEmpty()
                         ? (current_.signature.isEmpty() ? current_.name : current_.signature)
                         : shown);
    if (pseudo_ && !current_.namingReason.isEmpty())
        text.prepend(QStringLiteral("/* %1 */\n").arg(current_.namingReason));
    const int scroll = code_->verticalScrollBar()->value();
    code_->setPlainText(text);
    code_->document()->setModified(false);
    code_->verticalScrollBar()->setValue(scroll);
}

void DecompilerView::showError(const QString &error)
{
    setBusy(false);
    detail_->setText(tr("failed"));
    code_->setPlainText(QStringLiteral("/* %1 */").arg(error));
}

void DecompilerView::showEmpty(const QString &message)
{
    setBusy(false);
    current_ = Decompiled();
    header_->clear();
    detail_->clear();
    code_->setPlainText(QString());
    code_->setPlaceholderText(message);
}

} // namespace astral::gui
