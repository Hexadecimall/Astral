#include "views/hexview.hh"
#include "theme/theme.hh"

#include <QTextBlock>

namespace astral::gui {

namespace {
constexpr int kPerLine = 16;
}

HexView::HexView(QWidget *parent) : CodeView(parent)
{
    setPlaceholderText(tr("Bytes at the current address"));
}

void HexView::showBytes(quint64 base, const QByteArray &bytes, quint64 mark, quint64 markSize)
{
    base_ = base;
    QString text;
    text.reserve(bytes.size() * 4);
    for (qsizetype offset = 0; offset < bytes.size(); offset += kPerLine) {
        QString hex, ascii;
        for (int i = 0; i < kPerLine; ++i) {
            if (offset + i < bytes.size()) {
                const unsigned char c = static_cast<unsigned char>(bytes[offset + i]);
                hex += QStringLiteral("%1 ").arg(c, 2, 16, QLatin1Char('0'));
                ascii += (c >= 0x20 && c < 0x7f) ? QChar(char16_t(c)) : QLatin1Char('.');
            } else {
                hex += QStringLiteral("   ");
            }
            if (i == 7)
                hex += QLatin1Char(' ');
        }
        text += QStringLiteral("0x%1:  %2 %3\n").arg(base + offset, 12, 16, QLatin1Char('0')).arg(hex, ascii);
    }
    setPlainText(text);

    // Mark the function's bytes and park the cursor on its first line.
    QList<QTextEdit::ExtraSelection> marks;
    const Theme &theme = Theme::current();
    if (markSize > 0 && mark >= base) {
        const int firstLine = static_cast<int>((mark - base) / kPerLine);
        const int lastLine = static_cast<int>((mark + markSize - 1 - base) / kPerLine);
        for (int line = firstLine; line <= lastLine && line < blockCount(); ++line) {
            const quint64 lineBase = base + static_cast<quint64>(line) * kPerLine;
            const int from = static_cast<int>(qMax<qint64>(0, static_cast<qint64>(mark - lineBase)));
            const int to = static_cast<int>(qMin<quint64>(kPerLine, mark + markSize - lineBase));
            auto column = [](int byte) { return 16 + byte * 3 + (byte >= 8 ? 1 : 0); };
            QTextEdit::ExtraSelection sel;
            sel.format.setBackground(theme.colour(QStringLiteral("selectionInactive")));
            sel.cursor = QTextCursor(document()->findBlockByNumber(line));
            sel.cursor.setPosition(sel.cursor.block().position() + column(from));
            sel.cursor.setPosition(sel.cursor.block().position() + column(to) - 1, QTextCursor::KeepAnchor);
            marks << sel;
        }
        QTextCursor cursor(document()->findBlockByNumber(firstLine));
        setTextCursor(cursor);
        centerCursor();
    }
    QList<QTextEdit::ExtraSelection> all = extraSelections();
    all += marks;
    setExtraSelections(all);
}

} // namespace astral::gui
