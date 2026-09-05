// The hex view is the only place a byte is typed straight into the program,
// so its editing model is checked on its own: what a digit does, where a
// digit is refused, and that nothing the user can press disturbs the layout
// the view reads its bytes back out of.
#include "views/hexview.hh"

#include <QtTest/QtTest>

using namespace astral::gui;

class HexEditTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void init();
    void cleanup();
    void typingDigitsChangesBytes();
    void typingRunsCoalesceIntoOneRun();
    void ignoresKeysThatWouldBreakTheLayout();
    void refusesEditsOutsideTheHexColumns();
    void doesNothingWhenNotEditing();
    void revertPutsTheBytesBack();

private:
    void placeCursor(int line, int byte, int nibble = 0);
    HexView *view_ = nullptr;
    QByteArray bytes_;
};

void HexEditTest::init()
{
    bytes_.resize(32);
    for (int i = 0; i < bytes_.size(); ++i)
        bytes_[i] = static_cast<char>(i);
    view_ = new HexView;
    view_->showBytes(0x1000, bytes_, 0x1000, 32);
    view_->setEditing(true);
}

void HexEditTest::cleanup()
{
    delete view_;
    view_ = nullptr;
}

void HexEditTest::placeCursor(int line, int byte, int nibble)
{
    QTextCursor cursor = view_->textCursor();
    cursor.setPosition(view_->document()->findBlockByNumber(line).position()
                       + HexView::hexColumn(byte) + nibble);
    view_->setTextCursor(cursor);
}

void HexEditTest::typingDigitsChangesBytes()
{
    placeCursor(0, 0);
    QTest::keyClicks(view_, QStringLiteral("ff"));
    QCOMPARE(view_->dirtyCount(), 1);
    const auto runs = view_->dirtyRuns();
    QCOMPARE(runs.size(), size_t(1));
    QCOMPARE(runs[0].first, quint64(0x1000));
    QCOMPARE(runs[0].second, QByteArray(1, '\xff'));
    // The line still reads back the way the view writes it.
    const QString line = view_->document()->findBlockByNumber(0).text();
    QCOMPARE(line.mid(HexView::hexColumn(0), 2), QStringLiteral("ff"));
    QCOMPARE(line.mid(HexView::asciiColumn(0), 1), QStringLiteral("."));

    // Typing one nibble leaves the other alone.
    placeCursor(0, 1, 1);
    QTest::keyClicks(view_, QStringLiteral("a"));
    QCOMPARE(view_->dirtyCount(), 2);
    QCOMPARE(view_->document()->findBlockByNumber(0).text().mid(HexView::hexColumn(1), 2),
             QStringLiteral("0a"));
}

void HexEditTest::typingRunsCoalesceIntoOneRun()
{
    placeCursor(0, 4);
    // Four bytes typed straight through: the cursor walks itself along.
    QTest::keyClicks(view_, QStringLiteral("41424344"));
    QCOMPARE(view_->dirtyCount(), 4);
    const auto runs = view_->dirtyRuns();
    QCOMPARE(runs.size(), size_t(1));
    QCOMPARE(runs[0].first, quint64(0x1004));
    QCOMPARE(runs[0].second, QByteArray("ABCD"));

    // A byte typed back to what it was is no longer a change.
    placeCursor(0, 4);
    QTest::keyClicks(view_, QStringLiteral("04"));
    QCOMPARE(view_->dirtyCount(), 3);
    QCOMPARE(view_->dirtyRuns().size(), size_t(1));
    QCOMPARE(view_->dirtyRuns()[0].first, quint64(0x1005));
}

void HexEditTest::ignoresKeysThatWouldBreakTheLayout()
{
    const QString before = view_->toPlainText();
    placeCursor(0, 3);
    for (int key : {Qt::Key_Backspace, Qt::Key_Delete, Qt::Key_Return, Qt::Key_Enter, Qt::Key_Space,
                    Qt::Key_Tab})
        QTest::keyClick(view_, static_cast<Qt::Key>(key));
    QTest::keyClicks(view_, QStringLiteral("zq!-"));
    QCOMPARE(view_->dirtyCount(), 0);
    QCOMPARE(view_->toPlainText(), before);
}

void HexEditTest::refusesEditsOutsideTheHexColumns()
{
    const QString before = view_->toPlainText();
    // The address column.
    QTextCursor cursor = view_->textCursor();
    cursor.setPosition(view_->document()->findBlockByNumber(0).position() + 4);
    view_->setTextCursor(cursor);
    QTest::keyClicks(view_, QStringLiteral("ff"));
    // The characters column.
    cursor.setPosition(view_->document()->findBlockByNumber(0).position() + HexView::asciiColumn(0));
    view_->setTextCursor(cursor);
    QTest::keyClicks(view_, QStringLiteral("ff"));
    QCOMPARE(view_->dirtyCount(), 0);
    QCOMPARE(view_->toPlainText(), before);
}

void HexEditTest::doesNothingWhenNotEditing()
{
    view_->setEditing(false);
    const QString before = view_->toPlainText();
    placeCursor(0, 0);
    QTest::keyClicks(view_, QStringLiteral("ff"));
    QCOMPARE(view_->dirtyCount(), 0);
    QCOMPARE(view_->toPlainText(), before);
}

void HexEditTest::revertPutsTheBytesBack()
{
    const QString before = view_->toPlainText();
    placeCursor(1, 2);
    QTest::keyClicks(view_, QStringLiteral("de"));
    QCOMPARE(view_->dirtyCount(), 1);
    QCOMPARE(view_->dirtyRuns()[0].first, quint64(0x1012));
    view_->revert();
    QCOMPARE(view_->dirtyCount(), 0);
    QCOMPARE(view_->toPlainText(), before);
}

QTEST_MAIN(HexEditTest)
#include "hexedit_test.moc"
