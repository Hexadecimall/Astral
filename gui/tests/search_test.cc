// The search box, on the path a person actually takes: type, look, press
// Enter. The list is a popup over a line edit, so its keys are read out of
// the box rather than received directly, and choosing a row has to survive the
// list being put away in the same breath.
#include "app/searchresults.hh"

#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalSpy>
#include <QWidget>
#include <QtTest/QtTest>

using astral::gui::SearchResults;

namespace {

std::vector<SearchResults::Match> threeMatches()
{
    return {{QStringLiteral("address"), QStringLiteral("0x100000484"), 0x100000484ULL},
            {QStringLiteral("function"), QStringLiteral("check"), 0x100000410ULL},
            {QStringLiteral("string"), QStringLiteral("astral"), 0x100003f2aULL}};
}

void press(QLineEdit *box, int key)
{
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(box, &event);
}

} // namespace

class SearchTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void enterTakesTheHighlightedRow();
    void enterAfterMovingTakesTheRowMovedTo();
    void theListIsGoneBeforeTheChoiceIsActedOn();
    void escapePutsTheListAwayWithoutChoosing();
    void noMatchesShowsNothing();
};

void SearchTest::enterTakesTheHighlightedRow()
{
    QWidget host;
    auto *box = new QLineEdit(&host);
    auto *results = new SearchResults(box, &host);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    results->showMatches(threeMatches(), QStringLiteral("check"));
    QSignalSpy chosen(results, &SearchResults::chosen);
    press(box, Qt::Key_Return);
    QCOMPARE(chosen.count(), 1);
    QCOMPARE(chosen.at(0).at(0).toULongLong(), 0x100000484ULL);
}

void SearchTest::enterAfterMovingTakesTheRowMovedTo()
{
    QWidget host;
    auto *box = new QLineEdit(&host);
    auto *results = new SearchResults(box, &host);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    results->showMatches(threeMatches(), QStringLiteral("check"));
    QSignalSpy chosen(results, &SearchResults::chosen);
    press(box, Qt::Key_Down);
    press(box, Qt::Key_Down);
    press(box, Qt::Key_Return);
    QCOMPARE(chosen.count(), 1);
    QCOMPARE(chosen.at(0).at(0).toULongLong(), 0x100003f2aULL);
}

// Putting the list away deletes every row in it. The address has to be read
// before that happens, or what is reported is whatever the freed memory
// happens to hold - which is the shape of a crash, not of a wrong answer.
void SearchTest::theListIsGoneBeforeTheChoiceIsActedOn()
{
    QWidget host;
    auto *box = new QLineEdit(&host);
    auto *results = new SearchResults(box, &host);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    results->showMatches(threeMatches(), QStringLiteral("check"));
    bool showingWhenTold = true;
    quint64 got = 0;
    connect(results, &SearchResults::chosen, this, [&](quint64 address) {
        showingWhenTold = results->isShowing();
        got = address;
    });
    press(box, Qt::Key_Return);
    QVERIFY(!showingWhenTold);
    QCOMPARE(got, 0x100000484ULL);
}

void SearchTest::escapePutsTheListAwayWithoutChoosing()
{
    QWidget host;
    auto *box = new QLineEdit(&host);
    auto *results = new SearchResults(box, &host);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    results->showMatches(threeMatches(), QStringLiteral("check"));
    QSignalSpy chosen(results, &SearchResults::chosen);
    press(box, Qt::Key_Escape);
    QCOMPARE(chosen.count(), 0);
    QVERIFY(!results->isShowing());
}

void SearchTest::noMatchesShowsNothing()
{
    QWidget host;
    auto *box = new QLineEdit(&host);
    auto *results = new SearchResults(box, &host);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    results->showMatches({}, QStringLiteral("nothing"));
    QVERIFY(!results->isShowing());
    // And Enter on an empty list is not a choice, nor a crash.
    QSignalSpy chosen(results, &SearchResults::chosen);
    press(box, Qt::Key_Return);
    QCOMPARE(chosen.count(), 0);
}

QTEST_MAIN(SearchTest)
#include "search_test.moc"
