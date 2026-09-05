#include "views/hexview.hh"
#include "theme/theme.hh"

#include <QKeyEvent>
#include <QTextBlock>

namespace astral::gui {

namespace {
constexpr int kPerLine = 16;
// "0x", twelve address digits, ':' and two spaces.
constexpr int kPrefix = 17;
// Sixteen "hh " groups plus the gap after the eighth byte, then one space.
constexpr int kAsciiStart = kPrefix + kPerLine * 3 + 1 + 1;

bool isHexDigit(QChar c)
{
    return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c.toLower() >= QLatin1Char('a') && c.toLower() <= QLatin1Char('f'));
}
} // namespace

int HexView::hexColumn(int byte) { return kPrefix + byte * 3 + (byte >= 8 ? 1 : 0); }
int HexView::asciiColumn(int byte) { return kAsciiStart + byte; }

HexView::HexView(QWidget *parent) : CodeView(parent)
{
    setPlaceholderText(tr("Bytes at the current address"));
}

void HexView::showBytes(quint64 base, const QByteArray &bytes, quint64 mark, quint64 markSize)
{
    // Edits made against the same window survive a redraw; anything else is a
    // different view of the program and the edits no longer mean anything.
    const bool sameWindow = base == base_ && bytes.size() == bytes_.size() && !dirty_.isEmpty();
    base_ = base;
    original_ = bytes;
    bytes_ = bytes;
    mark_ = mark;
    markSize_ = markSize;
    if (sameWindow) {
        for (auto it = dirty_.cbegin(); it != dirty_.cend(); ++it)
            if (it.key() < bytes_.size())
                bytes_[it.key()] = static_cast<char>(it.value());
    } else if (!dirty_.isEmpty()) {
        dirty_.clear();
        Q_EMIT editsChanged(0);
    }
    render();
}

void HexView::render()
{
    QString text;
    text.reserve(bytes_.size() * 5);
    for (qsizetype offset = 0; offset < bytes_.size(); offset += kPerLine) {
        QString hex, ascii;
        for (int i = 0; i < kPerLine; ++i) {
            if (offset + i < bytes_.size()) {
                const unsigned char c = static_cast<unsigned char>(bytes_[offset + i]);
                hex += QStringLiteral("%1 ").arg(c, 2, 16, QLatin1Char('0'));
                ascii += (c >= 0x20 && c < 0x7f) ? QChar(char16_t(c)) : QLatin1Char('.');
            } else {
                hex += QStringLiteral("   ");
            }
            if (i == 7)
                hex += QLatin1Char(' ');
        }
        text += QStringLiteral("0x%1:  %2 %3\n").arg(base_ + offset, 12, 16, QLatin1Char('0')).arg(hex, ascii);
    }
    setPlainText(text);
    applySelections();
    if (markSize_ > 0 && mark_ >= base_) {
        const int firstLine = static_cast<int>((mark_ - base_) / kPerLine);
        if (firstLine < blockCount()) {
            setTextCursor(QTextCursor(document()->findBlockByNumber(firstLine)));
            centerCursor();
        }
    }
}

void HexView::applySelections()
{
    QList<QTextEdit::ExtraSelection> marks;
    const Theme &theme = Theme::current();
    if (markSize_ > 0 && mark_ >= base_) {
        const int firstLine = static_cast<int>((mark_ - base_) / kPerLine);
        const int lastLine = static_cast<int>((mark_ + markSize_ - 1 - base_) / kPerLine);
        for (int line = firstLine; line <= lastLine && line < blockCount(); ++line) {
            const quint64 lineBase = base_ + static_cast<quint64>(line) * kPerLine;
            const int from = static_cast<int>(qMax<qint64>(0, static_cast<qint64>(mark_ - lineBase)));
            const int to = static_cast<int>(qMin<quint64>(kPerLine, mark_ + markSize_ - lineBase));
            QTextEdit::ExtraSelection sel;
            sel.format.setBackground(theme.colour(QStringLiteral("selectionInactive")));
            sel.cursor = QTextCursor(document()->findBlockByNumber(line));
            sel.cursor.setPosition(sel.cursor.block().position() + hexColumn(from));
            sel.cursor.setPosition(sel.cursor.block().position() + hexColumn(to) - 1, QTextCursor::KeepAnchor);
            marks << sel;
        }
    }
    // Changed bytes stand out until they are queued.
    for (auto it = dirty_.cbegin(); it != dirty_.cend(); ++it) {
        const int line = static_cast<int>(it.key() / kPerLine);
        const int byte = static_cast<int>(it.key() % kPerLine);
        if (line >= blockCount())
            continue;
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(theme.colour(QStringLiteral("token.dirty")));
        sel.format.setForeground(theme.colour(QStringLiteral("background")));
        sel.cursor = QTextCursor(document()->findBlockByNumber(line));
        sel.cursor.setPosition(sel.cursor.block().position() + hexColumn(byte));
        sel.cursor.setPosition(sel.cursor.block().position() + hexColumn(byte) + 2, QTextCursor::KeepAnchor);
        marks << sel;
    }
    setExtraSelections(marks);
}

void HexView::setEditing(bool editing)
{
    editing_ = editing;
    setCursorWidth(editing ? 2 : 1);
}

qsizetype HexView::byteUnderCursor(int *nibble) const
{
    const QTextCursor cursor = textCursor();
    const int line = cursor.blockNumber();
    const int column = cursor.positionInBlock();
    for (int i = 0; i < kPerLine; ++i) {
        const int at = hexColumn(i);
        if (column == at || column == at + 1) {
            const qsizetype index = static_cast<qsizetype>(line) * kPerLine + i;
            if (index >= bytes_.size())
                return -1;
            *nibble = column - at;
            return index;
        }
    }
    return -1;
}

void HexView::setByte(qsizetype index, unsigned char value)
{
    bytes_[index] = static_cast<char>(value);
    if (static_cast<unsigned char>(original_[index]) == value)
        dirty_.remove(index);
    else
        dirty_.insert(index, value);

    const int line = static_cast<int>(index / kPerLine);
    const int byte = static_cast<int>(index % kPerLine);
    QTextCursor edit(document()->findBlockByNumber(line));
    const int start = edit.block().position();
    edit.beginEditBlock();
    edit.setPosition(start + hexColumn(byte));
    edit.setPosition(start + hexColumn(byte) + 2, QTextCursor::KeepAnchor);
    edit.insertText(QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')));
    edit.setPosition(start + asciiColumn(byte));
    edit.setPosition(start + asciiColumn(byte) + 1, QTextCursor::KeepAnchor);
    edit.insertText(value >= 0x20 && value < 0x7f ? QString(QChar(char16_t(value))) : QStringLiteral("."));
    edit.endEditBlock();
    applySelections();
    Q_EMIT editsChanged(dirtyCount());
}

void HexView::keyPressEvent(QKeyEvent *event)
{
    const bool plain = !(event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier));
    if (editing_ && plain && event->text().size() == 1 && isHexDigit(event->text().at(0))) {
        int nibble = 0;
        const qsizetype index = byteUnderCursor(&nibble);
        if (index < 0) {
            event->accept();
            return;
        }
        const int digit = event->text().at(0).toLower().unicode() <= QLatin1Char('9').unicode()
                              ? event->text().at(0).unicode() - '0'
                              : event->text().at(0).toLower().unicode() - 'a' + 10;
        const unsigned char before = static_cast<unsigned char>(bytes_[index]);
        const unsigned char after = nibble == 0 ? static_cast<unsigned char>((digit << 4) | (before & 0x0f))
                                                : static_cast<unsigned char>((before & 0xf0) | digit);
        setByte(index, after);

        // Move on to the next nibble so a pair can be typed straight through.
        const int line = static_cast<int>(index / kPerLine);
        const int byte = static_cast<int>(index % kPerLine);
        QTextCursor cursor = textCursor();
        if (nibble == 0) {
            cursor.setPosition(document()->findBlockByNumber(line).position() + hexColumn(byte) + 1);
        } else if (byte + 1 < kPerLine && index + 1 < bytes_.size()) {
            cursor.setPosition(document()->findBlockByNumber(line).position() + hexColumn(byte + 1));
        } else if (line + 1 < blockCount() && index + 1 < bytes_.size()) {
            cursor.setPosition(document()->findBlockByNumber(line + 1).position() + hexColumn(0));
        }
        setTextCursor(cursor);
        event->accept();
        return;
    }
    // The widget stays read-only, so everything else either navigates or is
    // dropped; nothing can disturb the line structure.
    CodeView::keyPressEvent(event);
}

std::vector<std::pair<quint64, QByteArray>> HexView::dirtyRuns() const
{
    std::vector<std::pair<quint64, QByteArray>> runs;
    for (auto it = dirty_.cbegin(); it != dirty_.cend(); ++it) {
        const quint64 address = base_ + static_cast<quint64>(it.key());
        if (!runs.empty() && runs.back().first + static_cast<quint64>(runs.back().second.size()) == address)
            runs.back().second.append(static_cast<char>(it.value()));
        else
            runs.push_back({address, QByteArray(1, static_cast<char>(it.value()))});
    }
    return runs;
}

void HexView::revert()
{
    if (dirty_.isEmpty())
        return;
    dirty_.clear();
    bytes_ = original_;
    render();
    Q_EMIT editsChanged(0);
}

void HexView::clearEdits()
{
    if (dirty_.isEmpty())
        return;
    dirty_.clear();
    original_ = bytes_;
    applySelections();
    Q_EMIT editsChanged(0);
}

} // namespace astral::gui
