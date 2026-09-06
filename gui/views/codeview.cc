#include "views/codeview.hh"
#include "theme/theme.hh"

#include <QAbstractItemView>
#include <QCompleter>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QScrollBar>
#include <QSet>
#include <QStringListModel>
#include <QContextMenuEvent>

#include <algorithm>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QTextBlock>
#include <cstdint>

namespace astral::gui {

namespace {

class Gutter : public QWidget {
public:
    explicit Gutter(CodeView *view) : QWidget(view), view_(view) { setCursor(Qt::PointingHandCursor); }
    QSize sizeHint() const override { return QSize(view_->gutterWidth(), 0); }

protected:
    void paintEvent(QPaintEvent *event) override { view_->paintGutter(event); }
    void mousePressEvent(QMouseEvent *event) override
    {
        const QTextBlock block = view_->cursorForPosition(QPoint(0, event->position().toPoint().y())).block();
        Q_EMIT view_->gutterClicked(block.blockNumber());
    }

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


// ------------------------------------------------------------------- editing
//
// What a person expects of any editor, and what recovered code needs more than
// most: the brackets match because they were written as a pair, the indent
// carries because nobody wants to press space four times, and the names on
// offer are the ones already in the document rather than a dictionary.

namespace {

// The closing character for an opening one, or nul when it does not open
// anything. Quotes close themselves.
QChar closerFor(QChar opener)
{
    switch (opener.unicode()) {
    case '(': return QLatin1Char(')');
    case '[': return QLatin1Char(']');
    case '{': return QLatin1Char('}');
    case '"': return QLatin1Char('"');
    case '\'': return QLatin1Char('\'');
    default: return QChar();
    }
}

bool isCloser(QChar c)
{
    return c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}') ||
           c == QLatin1Char('"') || c == QLatin1Char('\'');
}

// A name character, so the character before the cursor decides whether a quote
// is opening a string or ending an identifier that happens to end in one.
bool isNameChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

QString indentOf(const QString &line)
{
    int at = 0;
    while (at < line.size() && (line.at(at) == QLatin1Char(' ') || line.at(at) == QLatin1Char('\t')))
        ++at;
    return line.left(at);
}

const QStringList &wordsFor(CodeView::Language language)
{
    static const QStringList none;
    static const QStringList c = {
        QStringLiteral("break"), QStringLiteral("case"), QStringLiteral("char"),
        QStringLiteral("const"), QStringLiteral("continue"), QStringLiteral("default"),
        QStringLiteral("do"), QStringLiteral("double"), QStringLiteral("else"),
        QStringLiteral("enum"), QStringLiteral("extern"), QStringLiteral("float"),
        QStringLiteral("for"), QStringLiteral("goto"), QStringLiteral("if"),
        QStringLiteral("int"), QStringLiteral("long"), QStringLiteral("return"),
        QStringLiteral("short"), QStringLiteral("signed"), QStringLiteral("sizeof"),
        QStringLiteral("static"), QStringLiteral("struct"), QStringLiteral("switch"),
        QStringLiteral("typedef"), QStringLiteral("union"), QStringLiteral("unsigned"),
        QStringLiteral("void"), QStringLiteral("while"),
        QStringLiteral("int8_t"), QStringLiteral("int16_t"), QStringLiteral("int32_t"),
        QStringLiteral("int64_t"), QStringLiteral("uint8_t"), QStringLiteral("uint16_t"),
        QStringLiteral("uint32_t"), QStringLiteral("uint64_t"), QStringLiteral("bool"),
        QStringLiteral("memcpy"), QStringLiteral("memset"), QStringLiteral("printf"),
        QStringLiteral("puts"), QStringLiteral("strcmp"), QStringLiteral("strlen"),
        QStringLiteral("malloc"), QStringLiteral("free"),
    };
    static const QStringList nova = {
        QStringLiteral("func"), QStringLiteral("var"), QStringLiteral("val"),
        QStringLiteral("stack"), QStringLiteral("asm"), QStringLiteral("call"),
        QStringLiteral("return"), QStringLiteral("if"), QStringLiteral("else"),
        QStringLiteral("while"), QStringLiteral("for"), QStringLiteral("loop"),
        QStringLiteral("break"), QStringLiteral("continue"), QStringLiteral("switch"),
        QStringLiteral("case"), QStringLiteral("as"), QStringLiteral("true"),
        QStringLiteral("false"), QStringLiteral("null"),
        QStringLiteral("i8"), QStringLiteral("i16"), QStringLiteral("i32"), QStringLiteral("i64"),
        QStringLiteral("u8"), QStringLiteral("u16"), QStringLiteral("u32"), QStringLiteral("u64"),
        QStringLiteral("f32"), QStringLiteral("f64"), QStringLiteral("bool"),
        QStringLiteral("void"), QStringLiteral("char"),
        QStringLiteral("unknown8"), QStringLiteral("unknown16"),
        QStringLiteral("unknown32"), QStringLiteral("unknown64"),
    };
    switch (language) {
    case CodeView::Language::C: return c;
    case CodeView::Language::Nova: return nova;
    default: return none;
    }
}

} // namespace

void CodeView::setLanguage(Language language)
{
    language_ = language;
}

void CodeView::setKnownNames(const QStringList &names)
{
    knownNames_ = names;
}

// Everything worth offering, in one list: what the language calls things, what
// the program calls things, and every name already written in this document -
// which for recovered code is most of what anyone wants to type.
QStringList CodeView::completionWords() const
{
    QSet<QString> words;
    for (const QString &word : wordsFor(language_))
        words.insert(word);
    for (const QString &name : knownNames_)
        words.insert(name);
    static const QRegularExpression name(QStringLiteral("[A-Za-z_][A-Za-z0-9_]{2,}"));
    QRegularExpressionMatchIterator it = name.globalMatch(toPlainText());
    while (it.hasNext())
        words.insert(it.next().captured());
    QStringList sorted(words.begin(), words.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

QString CodeView::prefixUnderCursor() const
{
    QTextCursor cursor = textCursor();
    const QString line = cursor.block().text();
    int at = cursor.positionInBlock();
    int start = at;
    while (start > 0 && isNameChar(line.at(start - 1)))
        --start;
    return line.mid(start, at - start);
}

void CodeView::insertCompletion(const QString &completion)
{
    QTextCursor cursor = textCursor();
    const int extra = completion.size() - completer_->completionPrefix().size();
    cursor.insertText(completion.right(extra));
    setTextCursor(cursor);
}

// The list is rebuilt each time it is asked for. A document being edited is a
// document whose names are changing, and an offer of a name that was deleted a
// minute ago is worse than no offer.
void CodeView::showCompletions()
{
    if (completer_ == nullptr) {
        completer_ = new QCompleter(this);
        completer_->setWidget(this);
        completer_->setCompletionMode(QCompleter::PopupCompletion);
        completer_->setCaseSensitivity(Qt::CaseInsensitive);
        connect(completer_, QOverload<const QString &>::of(&QCompleter::activated), this,
                &CodeView::insertCompletion);
    }
    const QString prefix = prefixUnderCursor();
    if (prefix.size() < 2) {
        completer_->popup()->hide();
        return;
    }
    completer_->setModel(new QStringListModel(completionWords(), completer_));
    completer_->setCompletionPrefix(prefix);
    if (completer_->completionCount() == 0 ||
        (completer_->completionCount() == 1 && completer_->currentCompletion() == prefix)) {
        completer_->popup()->hide();
        return;
    }
    completer_->popup()->setCurrentIndex(completer_->completionModel()->index(0, 0));
    QRect where = cursorRect();
    where.setWidth(completer_->popup()->sizeHintForColumn(0) +
                   completer_->popup()->verticalScrollBar()->sizeHint().width());
    completer_->complete(where);
}

// An opening bracket is written as a pair, so what is opened is closed. Over a
// selection the pair goes around it rather than replacing it, which is how a
// person wraps an expression in parentheses.
bool CodeView::closeBracket(QKeyEvent *event)
{
    if (event->text().size() != 1)
        return false;
    const QChar typed = event->text().at(0);
    const QChar closer = closerFor(typed);
    if (closer.isNull())
        return false;
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        const QString selected = cursor.selectedText();
        cursor.insertText(typed + selected + closer);
        return true;
    }
    // A quote right after a name is an apostrophe in someone's identifier, or
    // the end of a string being retyped; either way it is not opening one.
    const QString line = cursor.block().text();
    const int at = cursor.positionInBlock();
    if (closer == typed && at > 0 && isNameChar(line.at(at - 1)))
        return false;
    // Nor is a bracket opened in front of a name, where what follows would end
    // up inside it.
    if (at < line.size() && isNameChar(line.at(at)))
        return false;
    cursor.insertText(QString(typed) + closer);
    cursor.movePosition(QTextCursor::PreviousCharacter);
    setTextCursor(cursor);
    return true;
}

// Typing the closer that is already there steps over it instead of writing a
// second one, so a pair written by the editor closes the way a person expects.
bool CodeView::stepOverClose(QKeyEvent *event)
{
    if (event->text().size() != 1)
        return false;
    const QChar typed = event->text().at(0);
    if (!isCloser(typed))
        return false;
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection())
        return false;
    const QString line = cursor.block().text();
    const int at = cursor.positionInBlock();
    if (at >= line.size() || line.at(at) != typed)
        return false;
    cursor.movePosition(QTextCursor::NextCharacter);
    setTextCursor(cursor);
    return true;
}

// A new line starts where the last one did. After a brace it starts one level
// further in, and a brace waiting on the other side of the cursor is put on a
// line of its own, so the block is opened complete.
bool CodeView::openLine(QKeyEvent *event)
{
    if (event->key() != Qt::Key_Return && event->key() != Qt::Key_Enter)
        return false;
    if (event->modifiers() != Qt::NoModifier && event->modifiers() != Qt::KeypadModifier)
        return false;
    QTextCursor cursor = textCursor();
    const QString line = cursor.block().text();
    const int at = cursor.positionInBlock();
    const QString indent = indentOf(line);
    const QString before = line.left(at).trimmed();
    const QString after = line.mid(at);
    const bool opens = before.endsWith(QLatin1Char('{'));
    const QString deeper = indent + QStringLiteral("    ");

    cursor.beginEditBlock();
    if (opens && after.trimmed().startsWith(QLatin1Char('}'))) {
        cursor.insertText(QStringLiteral("\n") + deeper + QStringLiteral("\n") + indent);
        cursor.movePosition(QTextCursor::PreviousBlock);
        cursor.movePosition(QTextCursor::EndOfBlock);
    } else {
        cursor.insertText(QStringLiteral("\n") + (opens ? deeper : indent));
    }
    cursor.endEditBlock();
    setTextCursor(cursor);
    return true;
}

// A closing brace belongs one level out from what it closes, so typing it on a
// line holding nothing else moves that line back rather than leaving the brace
// wherever the indent happened to be.
bool CodeView::outdentClose(QKeyEvent *event)
{
    if (event->text() != QStringLiteral("}"))
        return false;
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection())
        return false;
    const QString line = cursor.block().text();
    const int at = cursor.positionInBlock();
    if (!line.left(at).trimmed().isEmpty())
        return false;
    const QString indent = indentOf(line);
    if (indent.size() < 4)
        return false;
    cursor.beginEditBlock();
    cursor.movePosition(QTextCursor::StartOfBlock);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, 4);
    cursor.removeSelectedText();
    cursor.movePosition(QTextCursor::EndOfBlock);
    cursor.insertText(QStringLiteral("}"));
    cursor.endEditBlock();
    setTextCursor(cursor);
    return true;
}

// Erasing the opening half of an empty pair erases both. The pair was written
// by one keystroke, so it comes back out on one.
bool CodeView::eraseEmptyPair(QKeyEvent *event)
{
    if (event->key() != Qt::Key_Backspace || event->modifiers() != Qt::NoModifier)
        return false;
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection())
        return false;
    const QString line = cursor.block().text();
    const int at = cursor.positionInBlock();
    if (at == 0 || at >= line.size())
        return false;
    if (closerFor(line.at(at - 1)) != line.at(at))
        return false;
    cursor.beginEditBlock();
    cursor.movePosition(QTextCursor::PreviousCharacter);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, 2);
    cursor.removeSelectedText();
    cursor.endEditBlock();
    setTextCursor(cursor);
    return true;
}

// Tab is four spaces, because that is what the emitter writes and a document
// that mixes the two lines up under one font and not under another. Over a
// selection it moves the whole block, and with shift it moves it back.
bool CodeView::indentBySpaces(QKeyEvent *event)
{
    const bool forward = event->key() == Qt::Key_Tab;
    const bool back = event->key() == Qt::Key_Backtab ||
                      (event->key() == Qt::Key_Tab &&
                       (event->modifiers() & Qt::ShiftModifier) != 0);
    if (!forward && !back)
        return false;

    QTextCursor cursor = textCursor();
    if (!cursor.hasSelection() && forward && !back) {
        // Round up to the next stop, so a tab lands where the next one would.
        const int column = cursor.positionInBlock();
        cursor.insertText(QString(4 - column % 4, QLatin1Char(' ')));
        setTextCursor(cursor);
        return true;
    }

    const int start = cursor.selectionStart();
    const int end = cursor.selectionEnd();
    cursor.beginEditBlock();
    cursor.setPosition(start);
    const int first = cursor.blockNumber();
    cursor.setPosition(end);
    const int last = cursor.blockNumber();
    for (int number = first; number <= last; ++number) {
        QTextCursor line(document()->findBlockByNumber(number));
        if (back) {
            const QString text = line.block().text();
            int spaces = 0;
            while (spaces < 4 && spaces < text.size() && text.at(spaces) == QLatin1Char(' '))
                ++spaces;
            if (spaces == 0)
                continue;
            line.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, spaces);
            line.removeSelectedText();
        } else {
            line.insertText(QStringLiteral("    "));
        }
    }
    cursor.endEditBlock();
    return true;
}

void CodeView::keyPressEvent(QKeyEvent *event)
{
    if (isReadOnly()) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }

    // While the list is up it owns the keys that move through it and the ones
    // that decide, and the view sees neither.
    if (completer_ != nullptr && completer_->popup()->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab:
            insertCompletion(completer_->currentCompletion());
            completer_->popup()->hide();
            event->accept();
            return;
        case Qt::Key_Escape:
            completer_->popup()->hide();
            event->accept();
            return;
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            event->ignore();
            return;
        default:
            break;
        }
    }

    // Asked for by name, whatever is under the cursor.
    const bool asked = event->key() == Qt::Key_Space &&
                       (event->modifiers() & Qt::ControlModifier) != 0;
    if (asked) {
        showCompletions();
        event->accept();
        return;
    }

    if (eraseEmptyPair(event) || openLine(event) || outdentClose(event) ||
        indentBySpaces(event) || stepOverClose(event) || closeBracket(event)) {
        event->accept();
        if (completer_ != nullptr && completer_->popup()->isVisible())
            completer_->popup()->hide();
        return;
    }

    QPlainTextEdit::keyPressEvent(event);

    // Offered as the name is typed, never for the first letter: one letter
    // matches most of the document and the list would be noise.
    if (!event->text().isEmpty() && (event->text().at(0).isLetterOrNumber() ||
                                     event->text().at(0) == QLatin1Char('_')))
        showCompletions();
    else if (completer_ != nullptr && completer_->popup()->isVisible())
        completer_->popup()->hide();
}

void CodeView::focusInEvent(QFocusEvent *event)
{
    if (completer_ != nullptr)
        completer_->setWidget(this);
    QPlainTextEdit::focusInEvent(event);
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

void CodeView::setGutterMarks(const std::vector<quint64> &marks, quint64 current)
{
    marks_ = marks;
    current_ = current;
    gutter_->update();
}

void CodeView::scrollToAddress(quint64 address)
{
    for (int line = 0; line < document()->blockCount(); ++line) {
        const auto at = addressAtLine(line);
        if (!at || *at != address)
            continue;
        QTextCursor cursor(document()->findBlockByNumber(line));
        setTextCursor(cursor);
        centerCursor();
        return;
    }
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
            // A breakpoint reads as a dot, the instruction about to run as an
            // arrow, both in the space the line number does not use.
            if (const auto at = addressAtLine(number)) {
                const int size = fontMetrics().height();
                const QRectF box(1, top, size, size);
                if (*at == current_ && current_ != 0) {
                    painter.setBrush(theme.colour(QStringLiteral("warning")));
                    painter.setPen(Qt::NoPen);
                    const QPolygonF arrow({QPointF(box.left() + 2, box.top() + 3),
                                           QPointF(box.left() + 2, box.bottom() - 3),
                                           QPointF(box.right() - 2, box.center().y())});
                    painter.drawPolygon(arrow);
                } else if (std::find(marks_.begin(), marks_.end(), *at) != marks_.end()) {
                    painter.setBrush(theme.colour(QStringLiteral("error")));
                    painter.setPen(Qt::NoPen);
                    painter.drawEllipse(box.adjusted(3, 3, -3, -3));
                }
                painter.setPen(theme.colour(number == current ? QStringLiteral("textMuted")
                                                              : QStringLiteral("textDisabled")));
            }
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

// ------------------------------------------------------------------ Nova

NovaHighlighter::NovaHighlighter(QTextDocument *document) : QSyntaxHighlighter(document)
{
    static const char *keywords[] = {
        "func", "var", "val", "stack", "return", "if", "else", "while", "for", "in", "loop",
        "break", "continue", "match", "or", "enum", "struct", "asm", "call", "import", "goto",
        "label", "sizeof", "as", "do", "switch", "extern", "true", "false", "null"};
    static const char *types[] = {
        "void", "bool", "char", "byte", "code", "int", "uint", "isize", "usize",
        "i8", "i16", "i32", "i64", "i128", "u8", "u16", "u32", "u64", "u128",
        "f32", "f64", "f80", "f128", "unknown8", "unknown16", "unknown32", "unknown64",
        "unk8", "unk16", "unk24", "unk32", "unk40", "unk48", "unk56", "unk64",
        "size_t", "wchar16", "wchar32"};

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
    rules_.push_back({QRegularExpression(QStringLiteral("\\b(0x[0-9A-Fa-f]+|0b[01_]+|0o[0-7_]+|\\d[\\d_]*)\\b")),
                      formatFor("token.number")});
    // `@` says where a thing lives, and the place after it is the point of it.
    rules_.push_back({QRegularExpression(QStringLiteral("@\\s*[-+]?[A-Za-z0-9_]+(@entry)?")),
                      formatFor("token.global")});
    rules_.push_back({QRegularExpression(QStringLiteral("\\b(global|label|data|function)[0-9a-fA-F]{3,}\\b")),
                      formatFor("token.global")});
    rules_.push_back({QRegularExpression(QStringLiteral("\\b(local|stack)[0-9a-fA-F]+\\b")),
                      formatFor("token.parameter")});
    rules_.push_back({QRegularExpression(QStringLiteral("\"(\\\\.|[^\"\\\\])*\"|'(\\\\.|[^'\\\\])'")),
                      formatFor("token.string")});
    rules_.push_back({QRegularExpression(QStringLiteral("^\\s*[A-Za-z_][A-Za-z0-9_]*:\\s*$")),
                      formatFor("token.label")});
    // Nova comments start at a `#` wherever it sits, and Nova reads C's too.
    rules_.push_back({QRegularExpression(QStringLiteral("#[^\\n]*")), formatFor("token.comment")});
    rules_.push_back({QRegularExpression(QStringLiteral("//[^\\n]*")), formatFor("token.comment")});

    commentStart_ = QRegularExpression(QStringLiteral("/\\*"));
    commentEnd_ = QRegularExpression(QStringLiteral("\\*/"));
    commentFormat_ = formatFor("token.comment");
}

void NovaHighlighter::highlightBlock(const QString &text)
{
    for (const Rule &rule : rules_) {
        auto it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            const auto match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }
    setCurrentBlockState(0);
    int start = previousBlockState() == 1 ? 0 : text.indexOf(commentStart_);
    while (start >= 0) {
        const auto end = commentEnd_.match(text, start);
        int length = 0;
        if (end.hasMatch()) {
            length = end.capturedEnd() - start;
        } else {
            setCurrentBlockState(1);
            length = text.length() - start;
        }
        setFormat(start, length, commentFormat_);
        start = text.indexOf(commentStart_, start + length);
    }
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
