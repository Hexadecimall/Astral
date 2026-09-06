#include "views/listingview.hh"

#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QRegularExpression>

namespace astral::gui {

ListingView::ListingView(QWidget *parent) : CodeView(parent)
{
    highlighter_ = new AsmHighlighter(document());
}

void ListingView::contextMenuEvent(QContextMenuEvent *event)
{
    // The line under the pointer is what the menu is about, so the cursor goes
    // there before the window is asked what applies to it.
    setTextCursor(cursorForPosition(event->pos()));
    CodeView::contextMenuEvent(event);
}

void ListingView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Double-clicking a branch target follows it.
    QTextCursor cursor = cursorForPosition(event->pos());
    cursor.select(QTextCursor::WordUnderCursor);
    QString word = cursor.selectedText();
    static const QRegularExpression hex(QStringLiteral("^(0x)?[0-9a-fA-F]{4,16}$"));
    if (hex.match(word).hasMatch()) {
        if (word.startsWith(QStringLiteral("0x")))
            word = word.mid(2);
        bool ok = false;
        const quint64 target = word.toULongLong(&ok, 16);
        if (ok && cursor.positionInBlock() > 16) {
            Q_EMIT navigateRequested(target);
            return;
        }
    }
    CodeView::mouseDoubleClickEvent(event);
}


void ListingView::setShowingSource(bool source)
{
    if (source == showingSource_ && highlighter_ != nullptr)
        return;
    showingSource_ = source;
    delete highlighter_;
    highlighter_ = source ? static_cast<QSyntaxHighlighter *>(new NovaHighlighter(document()))
                          : static_cast<QSyntaxHighlighter *>(new AsmHighlighter(document()));
}

} // namespace astral::gui
