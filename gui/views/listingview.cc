#include "views/listingview.hh"

#include <QContextMenuEvent>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>

namespace astral::gui {

ListingView::ListingView(QWidget *parent) : CodeView(parent)
{
    new AsmHighlighter(document());
}

void ListingView::contextMenuEvent(QContextMenuEvent *event)
{
    QTextCursor cursor = cursorForPosition(event->pos());
    setTextCursor(cursor);
    const auto address = addressAtCursor();
    QMenu menu(this);
    if (address) {
        menu.addAction(tr("Patch: no-op this instruction"), this, [this, address] {
            Q_EMIT nopRequested(*address, 1);
        });
        menu.addAction(tr("Patch: no-op N instructions..."), this, [this, address] {
            bool ok = false;
            const int count = QInputDialog::getInt(this, tr("No-op"), tr("Instructions"), 1, 1, 4096, 1, &ok);
            if (ok)
                Q_EMIT nopRequested(*address, count);
        });
        menu.addAction(tr("Patch: invert this branch"), this, [this, address] {
            Q_EMIT invertRequested(*address);
        });
        menu.addAction(tr("Patch: make function return a value..."), this, [this, address] {
            Q_EMIT returnRequested(*address);
        });
        menu.addSeparator();
    }
    menu.addAction(tr("Copy"), this, &QPlainTextEdit::copy)->setEnabled(textCursor().hasSelection());
    menu.addAction(tr("Select All"), this, &QPlainTextEdit::selectAll);
    menu.exec(event->globalPos());
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

} // namespace astral::gui
