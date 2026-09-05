#include "views/codeview.hh"
#include "theme/theme.hh"

#include <QFontDatabase>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QTextBlock>

namespace astral::gui {

namespace {

class Gutter : public QWidget {
public:
    explicit Gutter(CodeView *view) : QWidget(view), view_(view) {}
    QSize sizeHint() const override { return QSize(view_->gutterWidth(), 0); }

protected:
    void paintEvent(QPaintEvent *event) override { view_->paintGutter(event); }

private:
    CodeView *view_;
};

QTextCharFormat formatFor(const char *key)
{
    QTextCharFormat format;
    format.setForeground(Theme::current().colour(QString::fromLatin1(key)));
    return format;
}

} // namespace

CodeView::CodeView(QWidget *parent) : QPlainTextEdit(parent), gutter_(new Gutter(this))
{
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(12);
    setFont(mono);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    setFrameShape(QFrame::NoFrame);

    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeView::updateGutterWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeView::updateGutter);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeView::highlightCurrentLine);
    updateGutterWidth();
    highlightCurrentLine();
}

void CodeView::setEditable(bool editable)
{
    setReadOnly(!editable);
    setTextInteractionFlags(editable ? Qt::TextEditorInteraction
                                     : Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
}

std::optional<quint64> CodeView::addressAtLine(int blockNumber) const
{
    const QString line = document()->findBlockByNumber(blockNumber).text();
    const int colon = line.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return std::nullopt;
    QString head = line.left(colon).trimmed();
    if (head.startsWith(QStringLiteral("0x")))
        head = head.mid(2);
    bool ok = false;
    const quint64 address = head.toULongLong(&ok, 16);
    if (!ok)
        return std::nullopt;
    return address;
}

std::optional<quint64> CodeView::addressAtCursor() const
{
    return addressAtLine(textCursor().blockNumber());
}

int CodeView::gutterWidth() const
{
    int digits = 1;
    for (int max = qMax(1, blockCount()); max >= 10; max /= 10)
        ++digits;
    return 16 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * qMax(digits, 3);
}

void CodeView::updateGutterWidth()
{
    setViewportMargins(gutterWidth(), 0, 0, 0);
}

void CodeView::updateGutter(const QRect &rect, int dy)
{
    if (dy)
        gutter_->scroll(0, dy);
    else
        gutter_->update(0, rect.y(), gutter_->width(), rect.height());
    if (rect.contains(viewport()->rect()))
        updateGutterWidth();
}

QString CodeView::wordUnderCursor() const
{
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection())
        return cursor.selectedText().trimmed();
    cursor.select(QTextCursor::WordUnderCursor);
    return cursor.selectedText().trimmed();
}

QString CodeView::wordAt(const QPoint &pos) const
{
    QTextCursor cursor = cursorForPosition(pos);
    cursor.select(QTextCursor::WordUnderCursor);
    return cursor.selectedText().trimmed();
}

void CodeView::contextMenuEvent(QContextMenuEvent *event)
{
    // Right-clicking moves the cursor first, so the menu and the keyboard
    // shortcuts always talk about the same word.
    if (!textCursor().hasSelection())
        setTextCursor(cursorForPosition(event->pos()));
    const QString word = wordAt(event->pos());

    QMenu menu(this);
    Q_EMIT contextMenuAboutToShow(&menu, word);
    if (!menu.isEmpty())
        menu.addSeparator();
    if (!isReadOnly()) {
        menu.addAction(tr("Undo"), QKeySequence::Undo, this, &QPlainTextEdit::undo)
            ->setEnabled(document()->isUndoAvailable());
        menu.addAction(tr("Redo"), QKeySequence::Redo, this, &QPlainTextEdit::redo)
            ->setEnabled(document()->isRedoAvailable());
        menu.addSeparator();
        menu.addAction(tr("Cut"), QKeySequence::Cut, this, &QPlainTextEdit::cut)
            ->setEnabled(textCursor().hasSelection());
    }
    menu.addAction(tr("Copy"), QKeySequence::Copy, this, &QPlainTextEdit::copy)
        ->setEnabled(textCursor().hasSelection());
    if (!isReadOnly())
        menu.addAction(tr("Paste"), QKeySequence::Paste, this, &QPlainTextEdit::paste);
    menu.addAction(tr("Select All"), QKeySequence::SelectAll, this, &QPlainTextEdit::selectAll);
    menu.exec(event->globalPos());
}

void CodeView::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    gutter_->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
}

void CodeView::paintGutter(QPaintEvent *event)
{
    QPainter painter(gutter_);
    const Theme &theme = Theme::current();
    painter.fillRect(event->rect(), theme.colour(QStringLiteral("editorBackground")));
    painter.setPen(theme.colour(QStringLiteral("textDisabled")));
    painter.setFont(font());

    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int current = textCursor().blockNumber();
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(theme.colour(number == current ? QStringLiteral("textMuted")
                                                          : QStringLiteral("textDisabled")));
            painter.drawText(0, top, gutter_->width() - 8, fontMetrics().height(),
                             Qt::AlignRight, QString::number(number + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

void CodeView::highlightCurrentLine()
{
    QTextEdit::ExtraSelection selection;
    selection.format.setBackground(Theme::current().colour(QStringLiteral("editorLine")));
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    selection.cursor = textCursor();
    selection.cursor.clearSelection();
    setExtraSelections({selection});
    gutter_->update();
}

// ------------------------------------------------------------------ C

CHighlighter::CHighlighter(QTextDocument *document) : QSyntaxHighlighter(document)
{
    static const char *keywords[] = {
        "if", "else", "while", "for", "do", "return", "break", "continue", "switch", "case",
        "default", "goto", "sizeof", "struct", "union", "enum", "typedef", "static", "extern",
        "const", "volatile", "register", "inline", "true", "false", "NULL"};
    static const char *types[] = {
        "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "bool",
        "int1", "int2", "int4", "int8", "uint1", "uint2", "uint4", "uint8", "uint", "ulong",
        "ushort", "uchar", "byte", "undefined", "undefined1", "undefined2", "undefined4",
        "undefined8", "code", "size_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t",
        "uint16_t", "uint32_t", "uint64_t", "uintptr_t", "intptr_t", "FILE", "xunknown8",
        "xunknown4", "xunknown2", "xunknown1"};

    QStringList kw, ty;
    for (const char *k : keywords)
        kw << QString::fromLatin1(k);
    for (const char *t : types)
        ty << QString::fromLatin1(t);

    rules_.push_back({QRegularExpression(QStringLiteral("\\b(%1)\\b").arg(kw.join(QLatin1Char('|')))),
                      formatFor("token.keyword")});
    rules_.push_back({QRegularExpression(QStringLiteral("\\b(%1)\\b").arg(ty.join(QLatin1Char('|')))),
                      formatFor("token.type")});
    rules_.push_back({QRegularExpression(QStringLiteral("\\b[A-Za-z_][A-Za-z0-9_]*(?=\\s*\\()")),
                      formatFor("token.function")});
    rules_.push_back({QRegularExpression(QStringLiteral("\\b(0x[0-9A-Fa-f]+|\\d+)[uUlL]*\\b")),
                      formatFor("token.number")});
    rules_.push_back({QRegularExpression(QStringLiteral("\\b(param_\\d+|[a-z][A-Za-z0-9]*Var\\d+|iVar\\d+|uVar\\d+|pcVar\\d+|puVar\\d+)\\b")),
                      formatFor("token.parameter")});
    rules_.push_back({QRegularExpression(QStringLiteral("\\b(DAT_|g|dat|loc|sub)[0-9a-fA-F]{3,}\\b")),
                      formatFor("token.global")});
    rules_.push_back({QRegularExpression(QStringLiteral("\"(\\\\.|[^\"\\\\])*\"|'(\\\\.|[^'\\\\])'")),
                      formatFor("token.string")});
    rules_.push_back({QRegularExpression(QStringLiteral("^\\s*[A-Za-z_][A-Za-z0-9_]*:\\s*$")),
                      formatFor("token.label")});
    rules_.push_back({QRegularExpression(QStringLiteral("^\\s*#\\s*\\w+.*$")), formatFor("token.keyword")});
    rules_.push_back({QRegularExpression(QStringLiteral("<[A-Za-z0-9_/.]+>")), formatFor("token.string")});
    rules_.push_back({QRegularExpression(QStringLiteral("//[^\\n]*")), formatFor("token.comment")});

    commentStart_ = QRegularExpression(QStringLiteral("/\\*"));
    commentEnd_ = QRegularExpression(QStringLiteral("\\*/"));
    commentFormat_ = formatFor("token.comment");
}

void CHighlighter::highlightBlock(const QString &text)
{
    for (const Rule &rule : rules_) {
        auto it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            const auto match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }
    // Block comments may span lines; state 1 means "inside one".
    setCurrentBlockState(0);
    int start = 0;
    if (previousBlockState() != 1)
        start = text.indexOf(commentStart_);
    while (start >= 0) {
        const auto end = commentEnd_.match(text, start);
        int length;
        if (!end.hasMatch()) {
            setCurrentBlockState(1);
            length = text.length() - start;
        } else {
            length = end.capturedStart() - start + end.capturedLength();
        }
        setFormat(start, length, commentFormat_);
        start = text.indexOf(commentStart_, start + length);
    }
}

// ------------------------------------------------------------------ asm

AsmHighlighter::AsmHighlighter(QTextDocument *document) : QSyntaxHighlighter(document)
{
    address_ = formatFor("token.address");
    mnemonic_ = formatFor("token.mnemonic");
    register_ = formatFor("token.register");
    number_ = formatFor("token.number");
    symbol_ = formatFor("token.function");
}

void AsmHighlighter::highlightBlock(const QString &text)
{
    static const QRegularExpression line(QStringLiteral("^(0x[0-9a-fA-F]+:)\\s*(\\S+)(.*)$"));
    static const QRegularExpression reg(QStringLiteral("\\b([xwvqdshb]\\d{1,2}|sp|lr|pc|fp|xzr|wzr|[re]?[abcd]x|[re]?[sd]i|[re]?[sb]p|r\\d{1,2}[dwb]?|[abcd][lh]|[sd]il|[sb]pl|[xyz]mm\\d{1,2})\\b"));
    static const QRegularExpression num(QStringLiteral("#?-?(0x[0-9a-fA-F]+|\\d+)\\b"));
    static const QRegularExpression sym(QStringLiteral("\\b_?[A-Za-z][A-Za-z0-9_]{2,}\\b"));

    const auto m = line.match(text);
    if (!m.hasMatch())
        return;
    setFormat(m.capturedStart(1), m.capturedLength(1), address_);
    setFormat(m.capturedStart(2), m.capturedLength(2), mnemonic_);
    const int base = m.capturedStart(3);
    const QString operands = m.captured(3);
    auto symbols = sym.globalMatch(operands);
    while (symbols.hasNext()) {
        const auto s = symbols.next();
        setFormat(base + s.capturedStart(), s.capturedLength(), symbol_);
    }
    auto regs = reg.globalMatch(operands);
    while (regs.hasNext()) {
        const auto r = regs.next();
        setFormat(base + r.capturedStart(), r.capturedLength(), register_);
    }
    auto nums = num.globalMatch(operands);
    while (nums.hasNext()) {
        const auto n = nums.next();
        setFormat(base + n.capturedStart(), n.capturedLength(), number_);
    }
}

} // namespace astral::gui
