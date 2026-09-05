// The theme file is the one place a user tunes the look, so its parser gets
// checked on its own: a typo in a key must not silently fall back to defaults.
#include "theme/theme.hh"

#include <QtTest/QtTest>

class ThemeTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void parsesKeyValueLines();
    void ignoresCommentsAndBlankLines();
    void rejectsUnknownKeys();
    void rejectsBadColours();
    void missingKeysFallBackToDefaults();
    void styleSheetMentionsEveryColour();
};

void ThemeTest::parsesKeyValueLines()
{
    QString error;
    auto theme = astral::gui::Theme::parse("background = #101112\naccent=#3574f0\n", &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(theme.colour("background"), QColor("#101112"));
    QCOMPARE(theme.colour("accent"), QColor("#3574f0"));
}

void ThemeTest::ignoresCommentsAndBlankLines()
{
    QString error;
    auto theme = astral::gui::Theme::parse("# a comment\n\n  \nbackground = #000000 # trailing\n", &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(theme.colour("background"), QColor("#000000"));
}

void ThemeTest::rejectsUnknownKeys()
{
    QString error;
    astral::gui::Theme::parse("backgruond = #000000\n", &error);
    QVERIFY(error.contains("backgruond"));
    QVERIFY(error.contains("line 1"));
}

void ThemeTest::rejectsBadColours()
{
    QString error;
    astral::gui::Theme::parse("background = notacolour\n", &error);
    QVERIFY(error.contains("line 1"));
}

void ThemeTest::missingKeysFallBackToDefaults()
{
    QString error;
    auto theme = astral::gui::Theme::parse("", &error);
    QVERIFY(error.isEmpty());
    QVERIFY(theme.colour("background").isValid());
    QVERIFY(theme.colour("text").isValid());
}

void ThemeTest::styleSheetMentionsEveryColour()
{
    auto theme = astral::gui::Theme::defaults();
    QString sheet = theme.styleSheet();
    for (const QString &key : astral::gui::Theme::keys()) {
        QVERIFY2(sheet.contains(theme.colour(key).name(), Qt::CaseInsensitive)
                     || key.startsWith("token."),
                 qPrintable(key));
    }
}

QTEST_MAIN(ThemeTest)
#include "theme_test.moc"
