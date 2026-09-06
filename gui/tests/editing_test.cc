// The editing help in the code view. Every case here is a keystroke a person
// makes without thinking about it, which is exactly why it has to be right:
// an editor that writes one bracket where two were meant, or loses the indent
// on Enter, is noticed on the first line and resented on every one after.
#include "views/codeview.hh"

#include <QCompleter>
#include <QtTest/QtTest>

using astral::gui::CodeView;

namespace {

// Types `text` into `view` one character at a time, so each keystroke goes
// through the same path a person's does.
void type(CodeView &view, const QString &text)
{
    for (const QChar c : text) {
        if (c == QLatin1Char('\n'))
            QTest::keyClick(&view, Qt::Key_Return);
        else
            QTest::keyClicks(&view, QString(c));
    }
}

} // namespace

class EditingTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void anOpenBracketIsWrittenAsAPair();
    void typingTheCloserStepsOverIt();
    void aSelectionIsWrappedRatherThanReplaced();
    void quotesPairButNotAfterAName();
    void erasingAnEmptyPairErasesBoth();
    void aNewLineKeepsTheIndent();
    void aBraceOpensABlock();
    void aClosingBraceComesBackOut();
    void tabIsFourSpaces();
    void tabMovesAWholeSelection();
    void aReadOnlyViewIsLeftAlone();
    void completionOffersWhatTheDocumentHolds();
};

void EditingTest::anOpenBracketIsWrittenAsAPair()
{
    CodeView view;
    view.setEditable(true);
    type(view, QStringLiteral("strcmp("));
    QCOMPARE(view.toPlainText(), QStringLiteral("strcmp()"));
    // And the cursor is between them, so what is typed next goes inside.
    type(view, QStringLiteral("x"));
    QCOMPARE(view.toPlainText(), QStringLiteral("strcmp(x)"));
}

void EditingTest::typingTheCloserStepsOverIt()
{
    CodeView view;
    view.setEditable(true);
    type(view, QStringLiteral("f(x)"));
    QCOMPARE(view.toPlainText(), QStringLiteral("f(x)"));
    QCOMPARE(view.textCursor().positionInBlock(), 4);
}

void EditingTest::aSelectionIsWrappedRatherThanReplaced()
{
    CodeView view;
    view.setEditable(true);
    view.setPlainText(QStringLiteral("a == b"));
    QTextCursor cursor = view.textCursor();
    cursor.select(QTextCursor::Document);
    view.setTextCursor(cursor);
    QTest::keyClicks(&view, QStringLiteral("("));
    QCOMPARE(view.toPlainText(), QStringLiteral("(a == b)"));
}

void EditingTest::quotesPairButNotAfterAName()
{
    CodeView view;
    view.setEditable(true);
    type(view, QStringLiteral("\""));
    QCOMPARE(view.toPlainText(), QStringLiteral("\"\""));

    CodeView other;
    other.setEditable(true);
    // An apostrophe after a name is part of the name, not the start of a
    // string, so nothing is closed for it.
    type(other, QStringLiteral("dont'"));
    QCOMPARE(other.toPlainText(), QStringLiteral("dont'"));
}

void EditingTest::erasingAnEmptyPairErasesBoth()
{
    CodeView view;
    view.setEditable(true);
    type(view, QStringLiteral("f("));
    QCOMPARE(view.toPlainText(), QStringLiteral("f()"));
    QTest::keyClick(&view, Qt::Key_Backspace);
    QCOMPARE(view.toPlainText(), QStringLiteral("f"));
}

void EditingTest::aNewLineKeepsTheIndent()
{
    CodeView view;
    view.setEditable(true);
    view.setPlainText(QStringLiteral("    return 1;"));
    QTextCursor cursor = view.textCursor();
    cursor.movePosition(QTextCursor::End);
    view.setTextCursor(cursor);
    QTest::keyClick(&view, Qt::Key_Return);
    type(view, QStringLiteral("x"));
    QCOMPARE(view.toPlainText(), QStringLiteral("    return 1;\n    x"));
}

void EditingTest::aBraceOpensABlock()
{
    CodeView view;
    view.setEditable(true);
    // The brace is written as a pair, so the block is opened complete and the
    // new line lands inside it.
    type(view, QStringLiteral("func f() {"));
    QCOMPARE(view.toPlainText(), QStringLiteral("func f() {}"));
    QTest::keyClick(&view, Qt::Key_Return);
    type(view, QStringLiteral("return 1;"));
    QCOMPARE(view.toPlainText(),
             QStringLiteral("func f() {\n    return 1;\n}"));
}

void EditingTest::aClosingBraceComesBackOut()
{
    CodeView view;
    view.setEditable(true);
    view.setPlainText(QStringLiteral("        "));
    QTextCursor cursor = view.textCursor();
    cursor.movePosition(QTextCursor::End);
    view.setTextCursor(cursor);
    QTest::keyClicks(&view, QStringLiteral("}"));
    QCOMPARE(view.toPlainText(), QStringLiteral("    }"));
}

void EditingTest::tabIsFourSpaces()
{
    CodeView view;
    view.setEditable(true);
    QTest::keyClick(&view, Qt::Key_Tab);
    QCOMPARE(view.toPlainText(), QStringLiteral("    "));
    // And from a column that is not a multiple of four, up to the next stop.
    type(view, QStringLiteral("ab"));
    QTest::keyClick(&view, Qt::Key_Tab);
    QCOMPARE(view.toPlainText(), QStringLiteral("    ab  "));
}

void EditingTest::tabMovesAWholeSelection()
{
    CodeView view;
    view.setEditable(true);
    view.setPlainText(QStringLiteral("one\ntwo"));
    QTextCursor cursor = view.textCursor();
    cursor.select(QTextCursor::Document);
    view.setTextCursor(cursor);
    QTest::keyClick(&view, Qt::Key_Tab);
    QCOMPARE(view.toPlainText(), QStringLiteral("    one\n    two"));
    QTest::keyClick(&view, Qt::Key_Backtab);
    QCOMPARE(view.toPlainText(), QStringLiteral("one\ntwo"));
}

void EditingTest::aReadOnlyViewIsLeftAlone()
{
    CodeView view;
    view.setPlainText(QStringLiteral("read only"));
    type(view, QStringLiteral("("));
    QCOMPARE(view.toPlainText(), QStringLiteral("read only"));
}

void EditingTest::completionOffersWhatTheDocumentHolds()
{
    CodeView view;
    view.setEditable(true);
    view.setLanguage(CodeView::Language::Nova);
    view.setPlainText(QStringLiteral("var verifyPassword: i32;\n"));
    QTextCursor cursor = view.textCursor();
    cursor.movePosition(QTextCursor::End);
    view.setTextCursor(cursor);
    type(view, QStringLiteral("ver"));
    QCompleter *completer = view.findChild<QCompleter *>();
    QVERIFY(completer != nullptr);
    QStringList offered;
    for (int i = 0; i < completer->completionCount(); ++i) {
        completer->setCurrentRow(i);
        offered.append(completer->currentCompletion());
    }
    // A name written above, offered below. Nothing taught it that name.
    QVERIFY(offered.contains(QStringLiteral("verifyPassword")));
}

QTEST_MAIN(EditingTest)
#include "editing_test.moc"
