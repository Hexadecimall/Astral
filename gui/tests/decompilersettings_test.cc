// The settings model: where a value is written, which of two values wins, and
// what happens to one the library will not accept.
//
// The settings file is the only thing here that outlives the session, so every
// check goes through a real file in a temporary home rather than a stand-in.
#include "model/decompilersettings.hh"
#include "model/settings.hh"

#include <astral/astral.h>

#include <QtTest/QtTest>

#include <QTemporaryDir>

using astral::gui::DecompilerSettings;
using astral::gui::OptionInfo;
using astral::gui::Settings;

class DecompilerSettingsTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void init();

    void theTableIsTheLibrarys();
    void commandOptionsAreNotOffered();
    void everyDefaultIsAValueTheSettingTakes();
    void anUnsetSettingReadsBackAsItsDefault();
    void aValueSetForEveryProgramIsWhatIsRead();
    void aProgramsOwnValueWins();
    void clearingAProgramsValueLetsTheSharedOneThroughAgain();
    void aRefusedValueIsNotWritten();
    void resettingAGroupLeavesOtherGroupsAlone();
    void twoProgramsOfTheSameNameGetDifferentKeys();
    void theWrittenFileReadsBack();
};

void DecompilerSettingsTest::initTestCase()
{
    // The settings file lives under the home directory, and this test must
    // never touch the one belonging to whoever is running it.
    static QTemporaryDir home;
    QVERIFY(home.isValid());
    qputenv("HOME", home.path().toUtf8());
    QVERIFY(Settings::path().startsWith(home.path()));
}

void DecompilerSettingsTest::init()
{
    DecompilerSettings::resetAll(QString());
    DecompilerSettings::resetAll(DecompilerSettings::programKey(QStringLiteral("/tmp/one")));
    DecompilerSettings::resetAll(DecompilerSettings::programKey(QStringLiteral("/tmp/two")));
}

void DecompilerSettingsTest::theTableIsTheLibrarys()
{
    const QVector<OptionInfo> &table = DecompilerSettings::options();
    QCOMPARE(table.size(), astral_option_count());
    QVERIFY(table.size() > 20);
    QVERIFY(DecompilerSettings::find(QStringLiteral("maxlinewidth")) != nullptr);
    QVERIFY(!DecompilerSettings::groups().isEmpty());
    // The dialog is built from the groups, so a setting outside them all would
    // exist and be unreachable.
    for (const OptionInfo &info : table)
        QVERIFY(DecompilerSettings::groups().contains(info.group));
}

void DecompilerSettingsTest::commandOptionsAreNotOffered()
{
    // These four name an action, a rule or a language rather than carrying a
    // value, and are commands to the decompiler rather than settings.
    for (const char *withheld : {"setlanguage", "setaction", "currentaction", "togglerule"})
        QVERIFY(DecompilerSettings::find(QString::fromLatin1(withheld)) == nullptr);
}

void DecompilerSettingsTest::everyDefaultIsAValueTheSettingTakes()
{
    for (const OptionInfo &info : DecompilerSettings::options()) {
        QString error;
        QVERIFY2(DecompilerSettings::setValue(info.name, info.defaultValue, QString(), &error),
                 qPrintable(QStringLiteral("%1: %2").arg(info.name, error)));
    }
}

void DecompilerSettingsTest::anUnsetSettingReadsBackAsItsDefault()
{
    const OptionInfo *width = DecompilerSettings::find(QStringLiteral("maxlinewidth"));
    QVERIFY(width != nullptr);
    QCOMPARE(DecompilerSettings::value(QStringLiteral("maxlinewidth")), width->defaultValue);
    QCOMPARE(width->defaultValue, QStringLiteral("100"));
}

void DecompilerSettingsTest::aValueSetForEveryProgramIsWhatIsRead()
{
    QVERIFY(DecompilerSettings::setValue(QStringLiteral("maxlinewidth"), QStringLiteral("80"),
                                         QString(), nullptr));
    QCOMPARE(DecompilerSettings::globalValue(QStringLiteral("maxlinewidth")),
             QStringLiteral("80"));
    const QString one = DecompilerSettings::programKey(QStringLiteral("/tmp/one"));
    QCOMPARE(DecompilerSettings::value(QStringLiteral("maxlinewidth"), one),
             QStringLiteral("80"));
    QVERIFY(!DecompilerSettings::overridden(QStringLiteral("maxlinewidth"), one));
}

void DecompilerSettingsTest::aProgramsOwnValueWins()
{
    const QString one = DecompilerSettings::programKey(QStringLiteral("/tmp/one"));
    const QString two = DecompilerSettings::programKey(QStringLiteral("/tmp/two"));
    QVERIFY(DecompilerSettings::setValue(QStringLiteral("maxlinewidth"), QStringLiteral("80"),
                                         QString(), nullptr));
    QVERIFY(DecompilerSettings::setValue(QStringLiteral("maxlinewidth"), QStringLiteral("40"),
                                         one, nullptr));

    QCOMPARE(DecompilerSettings::value(QStringLiteral("maxlinewidth"), one), QStringLiteral("40"));
    QVERIFY(DecompilerSettings::overridden(QStringLiteral("maxlinewidth"), one));
    // The other program never asked for anything, so it keeps the shared value.
    QCOMPARE(DecompilerSettings::value(QStringLiteral("maxlinewidth"), two), QStringLiteral("80"));
    QCOMPARE(DecompilerSettings::value(QStringLiteral("maxlinewidth")), QStringLiteral("80"));
}

void DecompilerSettingsTest::clearingAProgramsValueLetsTheSharedOneThroughAgain()
{
    const QString one = DecompilerSettings::programKey(QStringLiteral("/tmp/one"));
    QVERIFY(DecompilerSettings::setValue(QStringLiteral("maxlinewidth"), QStringLiteral("80"),
                                         QString(), nullptr));
    QVERIFY(DecompilerSettings::setValue(QStringLiteral("maxlinewidth"), QStringLiteral("40"),
                                         one, nullptr));
    DecompilerSettings::clearValue(QStringLiteral("maxlinewidth"), one);
    QVERIFY(!DecompilerSettings::overridden(QStringLiteral("maxlinewidth"), one));
    QCOMPARE(DecompilerSettings::value(QStringLiteral("maxlinewidth"), one), QStringLiteral("80"));
}

void DecompilerSettingsTest::aRefusedValueIsNotWritten()
{
    QString error;
    QVERIFY(!DecompilerSettings::setValue(QStringLiteral("maxlinewidth"),
                                          QStringLiteral("as wide as it likes"), QString(),
                                          &error));
    QVERIFY2(error.contains(QStringLiteral("maxlinewidth")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("number")), qPrintable(error));
    // Nothing was written, so the setting still reads as its default.
    QCOMPARE(DecompilerSettings::value(QStringLiteral("maxlinewidth")), QStringLiteral("100"));
    QVERIFY(Settings::instance()
                .stringValue(QStringLiteral("decompiler.maxlinewidth"))
                .isEmpty());

    QVERIFY(!DecompilerSettings::setValue(QStringLiteral("braceformat.function"),
                                          QStringLiteral("sideways"), QString(), &error));
    QVERIFY2(error.contains(QStringLiteral("same")), qPrintable(error));
    QVERIFY(!DecompilerSettings::setValue(QStringLiteral("noSuchSetting"), QStringLiteral("on"),
                                          QString(), &error));
    QVERIFY(!error.isEmpty());
}

void DecompilerSettingsTest::resettingAGroupLeavesOtherGroupsAlone()
{
    QVERIFY(DecompilerSettings::setValue(QStringLiteral("maxlinewidth"), QStringLiteral("80"),
                                         QString(), nullptr));
    QVERIFY(DecompilerSettings::setValue(QStringLiteral("readonly"), QStringLiteral("on"),
                                         QString(), nullptr));
    const OptionInfo *width = DecompilerSettings::find(QStringLiteral("maxlinewidth"));
    QVERIFY(width != nullptr);
    DecompilerSettings::resetGroup(width->group, QString());
    QCOMPARE(DecompilerSettings::value(QStringLiteral("maxlinewidth")), QStringLiteral("100"));
    QCOMPARE(DecompilerSettings::value(QStringLiteral("readonly")), QStringLiteral("on"));
}

void DecompilerSettingsTest::twoProgramsOfTheSameNameGetDifferentKeys()
{
    const QString here = DecompilerSettings::programKey(QStringLiteral("/bin/ls"));
    const QString there = DecompilerSettings::programKey(QStringLiteral("/usr/local/bin/ls"));
    QVERIFY(here != there);
    // The name is what makes the key readable in a file someone edits by hand.
    QVERIFY(here.startsWith(QStringLiteral("ls-")));
    QCOMPARE(here, DecompilerSettings::programKey(QStringLiteral("/bin/ls")));
}

void DecompilerSettingsTest::theWrittenFileReadsBack()
{
    const QString one = DecompilerSettings::programKey(QStringLiteral("/tmp/one"));
    QVERIFY(DecompilerSettings::setValue(QStringLiteral("commentstyle"),
                                         QStringLiteral("cplusplus"), one, nullptr));
    QCOMPARE(DecompilerSettings::keyFor(QStringLiteral("commentstyle"), one),
             QStringLiteral("decompiler.program.%1.commentstyle").arg(one));

    QFile file(Settings::path());
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(file.readAll());
    QVERIFY2(text.contains(QStringLiteral("decompiler.program.%1.commentstyle = cplusplus").arg(one)),
             qPrintable(text));

    // What the file holds is what a fresh read finds, so hand editing works.
    Settings::instance().reload();
    QCOMPARE(DecompilerSettings::value(QStringLiteral("commentstyle"), one),
             QStringLiteral("cplusplus"));
}

QTEST_MAIN(DecompilerSettingsTest)
#include "decompilersettings_test.moc"
