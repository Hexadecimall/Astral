// Entry point. Applies the theme before the first window so nothing flashes
// the platform look, then hands over to the main window.
#include "app/mainwindow.hh"
#include "theme/theme.hh"

#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QStyleFactory>
#include <QTimer>

#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Astral"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("astral.invalid"));
    QCoreApplication::setApplicationName(QStringLiteral("Astral"));
    QCoreApplication::setApplicationVersion(QStringLiteral(ASTRAL_GUI_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Astral decompiler"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption themeOption(QStringLiteral("theme"), QStringLiteral("Theme file to use"),
                                   QStringLiteral("file"));
    parser.addOption(themeOption);
    QCommandLineOption shotOption(QStringLiteral("screenshot"),
                                  QStringLiteral("Save a picture of the window after it settles, then quit"),
                                  QStringLiteral("png"));
    parser.addOption(shotOption);
    QCommandLineOption viewOption(QStringLiteral("view"), QStringLiteral("View tab to show: code, pseudo, graph, hex"),
                                  QStringLiteral("name"));
    parser.addOption(viewOption);
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("Binary or project to open"));
    parser.process(app);

    // Fusion draws from the palette alone, so the theme applies the same on
    // every platform instead of fighting a native style.
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QString error;
    astral::gui::Theme theme;
    if (parser.isSet(themeOption)) {
        theme = astral::gui::Theme::load(parser.value(themeOption), &error);
    } else {
        QFile bundled(QStringLiteral(":/theme/dark.astraltheme"));
        if (bundled.open(QIODevice::ReadOnly | QIODevice::Text))
            theme = astral::gui::Theme::parse(QString::fromUtf8(bundled.readAll()), &error);
        else
            error = QStringLiteral("bundled theme is missing");
    }
    if (!error.isEmpty())
        std::fprintf(stderr, "theme: %s\n", qPrintable(error));
    astral::gui::Theme::setCurrent(theme);
    app.setPalette(theme.palette());
    app.setStyleSheet(theme.styleSheet());

    astral::gui::MainWindow window;
    window.show();
    const QStringList files = parser.positionalArguments();
    if (!files.isEmpty())
        window.openPath(files.first());
    if (parser.isSet(viewOption)) {
        const QString name = parser.value(viewOption);
        QTimer::singleShot(1500, &window, [&window, name] { window.selectView(name); });
    }
    const QString editFile = qEnvironmentVariable("ASTRAL_GUI_EDIT");
    const QString writeOut = qEnvironmentVariable("ASTRAL_GUI_WRITE");
    if (!editFile.isEmpty())
        QTimer::singleShot(2000, &window, [&window, editFile, writeOut] { window.runEditHook(editFile, writeOut); });
    const QString exportOut = qEnvironmentVariable("ASTRAL_GUI_EXPORT");
    if (!exportOut.isEmpty())
        QTimer::singleShot(1500, &window, [&window, exportOut] { window.runExportHook(exportOut); });
    const QString asmEdit = qEnvironmentVariable("ASTRAL_GUI_ASM");
    const QString asmOut = qEnvironmentVariable("ASTRAL_GUI_ASM_OUT");
    if (!asmEdit.isEmpty() && !asmOut.isEmpty())
        QTimer::singleShot(1500, &window, [&window, asmEdit, asmOut] { window.runAssembleHook(asmEdit, asmOut); });
    if (qEnvironmentVariableIsSet("ASTRAL_GUI_DUMP_LISTING"))
        QTimer::singleShot(1500, &window, &astral::gui::MainWindow::dumpListing);
    if (qEnvironmentVariableIsSet("ASTRAL_GUI_ANALYZE"))
        QTimer::singleShot(1500, &window, [&window] { window.runAnalyzeHook(); });
    const QString probe = qEnvironmentVariable("ASTRAL_GUI_SEARCH");
    if (!probe.isEmpty())
        QTimer::singleShot(1800, &window, [&window, probe] { window.typeInSearch(probe); });
    if (parser.isSet(shotOption)) {
        const QString target = parser.value(shotOption);
        const int delay = qEnvironmentVariableIntValue("ASTRAL_GUI_SHOT_DELAY") > 0
                              ? qEnvironmentVariableIntValue("ASTRAL_GUI_SHOT_DELAY")
                              : 2500;
        QTimer::singleShot(delay, &window, [&window, target] {
            window.grab().save(target);
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
