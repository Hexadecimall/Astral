#include "app/mainwindow.hh"
#include "app/titlebar.hh"
#include "app/functionspane.hh"
#include "model/functionlistmodel.hh"
#include "app/programtab.hh"
#include "app/searchresults.hh"
#include "model/decompilersettings.hh"
#include "model/settings.hh"
#include "app/projectcontroller.hh"
#include "app/tablepane.hh"
#include "app/welcomepage.hh"
#include "views/codeview.hh"
#include "views/listingpane.hh"
#include "views/debuggerpane.hh"
#include "views/listingview.hh"
#include "views/optionsdialog.hh"
#include "model/decompilersettings.hh"

#include <QDateTime>

#include <cstdio>
#include <memory>
#include <QFile>
#include <QThread>
#include <QTimer>
#include <QInputDialog>
#include <QTreeWidgetItem>
#include <QKeyEvent>
#include <QTextEdit>
#include "platform/window.hh"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QCheckBox>
#include <QClipboard>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSettings>
#include <QUrl>
#include <QStackedWidget>
#include <QLineEdit>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <utility>

namespace astral::gui {
namespace {

// Tree items carry an address to jump to in UserRole; a project's member
// items carry the binary they stand for here instead.
constexpr int kProgramPathRole = Qt::UserRole + 1;


// True while one of the hooks is driving the window. A question put to
// nobody stops a run that has no one to answer it.
bool scriptedRun()
{
    static const char *const hooks[] = {
        "ASTRAL_GUI_EXPORT",  "ASTRAL_GUI_EDIT",    "ASTRAL_GUI_ASM",  "ASTRAL_GUI_DUMP_LISTING",
        "ASTRAL_GUI_DEBUG",   "ASTRAL_GUI_ANALYZE", "ASTRAL_GUI_MENU", "ASTRAL_GUI_RENAME",
        "ASTRAL_GUI_PROJECT"};
    for (const char *hook : hooks)
        if (qEnvironmentVariableIsSet(hook))
            return true;
    return false;
}

// A pane body used until the real widget for it exists: a filter box over an
// empty list, so the layout can be judged with the right proportions.
QWidget *placeholderList(const QString &hint)
{
    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *filter = new QLineEdit;
    filter->setPlaceholderText(hint);
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);
    auto *list = new QListWidget;
    layout->addWidget(list);
    return body;
}

QWidget *placeholderText(const QString &text)
{
    auto *edit = new QPlainTextEdit;
    edit->setReadOnly(true);
    edit->setPlaceholderText(text);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(12);
    edit->setFont(mono);
    return edit;
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Astral"));
    setDockNestingEnabled(true);
    setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks | GroupedDragging);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    resize(1440, 900);

    project_ = new ProjectController(this);

    platform::adoptCustomTitleBar(this);
    titleBar_ = new TitleBar(this);
    setMenuWidget(titleBar_);
    connect(this, &QWidget::windowTitleChanged, titleBar_, &TitleBar::setTitle);
    titleBar_->setTitle(windowTitle());

    // One header row: the open programs, then the views of the current one,
    // side by side. Below it, the current program's widget.
    workspace_ = new QWidget;
    auto *workspaceLayout = new QVBoxLayout(workspace_);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(0);
    auto *header = new QWidget;
    header->setObjectName(QStringLiteral("tabHeader"));
    header->setAttribute(Qt::WA_StyledBackground, true);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);
    programBar_ = new QTabBar;
    programBar_->setObjectName(QStringLiteral("programTabs"));
    programBar_->setDocumentMode(false);
    programBar_->setTabsClosable(true);
    programBar_->setMovable(true);
    programBar_->setExpanding(false);
    programBar_->setDrawBase(false);
    // Never wider than its tabs, so the view tabs sit right beside them.
    programBar_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    programBar_->setUsesScrollButtons(false);
    viewBar_ = new QTabBar;
    viewBar_->setObjectName(QStringLiteral("viewTabs"));
    viewBar_->setDrawBase(false);
    viewBar_->setExpanding(false);
    for (const QString &name : {tr("Code"), tr("Pseudo-C"), tr("Graph"), tr("Hex")})
        viewBar_->addTab(name);
    auto *divider = new QFrame;
    divider->setObjectName(QStringLiteral("tabDivider"));
    divider->setFixedWidth(1);
    headerLayout->addWidget(programBar_, 0, Qt::AlignBottom);
    headerLayout->addWidget(divider);
    headerLayout->addWidget(viewBar_, 0, Qt::AlignBottom);
    headerLayout->addStretch(1);
    programStack_ = new QStackedWidget;
    workspaceLayout->addWidget(header);
    workspaceLayout->addWidget(programStack_, 1);

    connect(programBar_, &QTabBar::tabCloseRequested, this, &MainWindow::closeProgram);
    connect(programBar_, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index < programStack_->count())
            programStack_->setCurrentIndex(index);
        bindCurrentTab();
    });
    connect(programBar_, &QTabBar::tabMoved, this, [this](int from, int to) {
        QWidget *moved = programStack_->widget(from);
        programStack_->removeWidget(moved);
        programStack_->insertWidget(to, moved);
        programStack_->setCurrentIndex(programBar_->currentIndex());
    });
    connect(viewBar_, &QTabBar::currentChanged, this, [this](int index) {
        if (ProgramTab *tab = currentTab())
            tab->setView(static_cast<ProgramTab::View>(index));
    });

    welcome_ = new WelcomePage;
    connect(welcome_, &WelcomePage::newProjectRequested, this, &MainWindow::newProject);
    connect(welcome_, &WelcomePage::openRequested, this, [this] {
        QString path = QFileDialog::getOpenFileName(this, tr("Open Binary or Project"));
        if (!path.isEmpty())
            openPath(path);
    });
    connect(welcome_, &WelcomePage::recentRequested, this, &MainWindow::openPath);

    stack_ = new QStackedWidget;
    stack_->addWidget(welcome_);
    stack_->addWidget(workspace_);
    setCentralWidget(stack_);

    buildMenus();
    buildToolBar();
    buildDocks();
    buildStatusBar();

    builtinState_ = saveState();
    defaultGeometry_ = saveGeometry();
    restoreLayout();
    showWelcome();
}


void MainWindow::showWelcome()
{
    welcome_->refresh();
    stack_->setCurrentWidget(welcome_);
    for (QDockWidget *pane : panes_)
        pane->hide();
    navigationBar_->hide();
    statusBar()->hide();
    setWindowTitle(QStringLiteral("Astral"));
}

void MainWindow::showWorkspace()
{
    stack_->setCurrentWidget(workspace_);
    navigationBar_->show();
    statusBar()->show();
    restoreLayout();
    // A restored layout can bring back a toolbar that belongs to a state the
    // window is not in, so the debugger's bar is put right after the layout
    // rather than trusting what was saved.
    if (debugBar_ != nullptr)
        debugBar_->setVisible(debugging_);

    // A layout with nothing visible is never what anyone meant; it is what
    // the welcome screen looks like to saveState.
    bool anyVisible = false;
    for (QDockWidget *pane : panes_)
        anyVisible = anyVisible || pane->isVisible();
    if (!anyVisible)
        resetLayout();
}

void MainWindow::openPath(const QString &path)
{
    if (Project::looksLikeProject(path)) {
        openProject(path);
        return;
    }
    if (ProgramTab *already = tabForPath(path)) {
        programBar_->setCurrentIndex(programStack_->indexOf(already));
        showWorkspace();
        bindCurrentTab();
        return;
    }
    statusBar()->showMessage(tr("Opening %1").arg(path));
    setEnabled(false);
    ProgramDocument::open(path, this, [this, path](std::unique_ptr<ProgramDocument> document,
                                                     const QString &error) {
        setEnabled(true);
        if (!document) {
            QMessageBox::warning(this, tr("Cannot open"), QStringLiteral("%1\n\n%2").arg(path, error));
            statusBar()->clearMessage();
            return;
        }
        WelcomePage::rememberRecent(path);
        // Configured before the first function is read, so nothing is
        // recovered under settings that are about to change.
        QStringList refused;
        if (!DecompilerSettings::applyTo(*document, refused))
            for (const QString &problem : refused)
                appendLog(tr("settings: %1").arg(problem));
        auto *tab = new ProgramTab(std::move(document));
        connect(tab, &ProgramTab::logMessage, this, &MainWindow::appendLog);
        connect(tab, &ProgramTab::patchApplied, this, [this, tab] { onPatchApplied(tab); });
        connect(tab, &ProgramTab::contextActionsWanted, this,
                [this](QMenu *menu, const QString &word, const QString &line) {
                    fillContextMenu(menu, targetForWord(word, ContextTarget::Source, line));
                });
        connect(tab, &ProgramTab::viewChanged, this, [this, tab](int index) {
            if (tab == currentTab() && viewBar_->currentIndex() != index)
                viewBar_->setCurrentIndex(index);
        });
        ProgramDocument *doc = tab->document();
        connect(doc, &ProgramDocument::analysisProgress, this, [this, tab](int done, int total, const QString &name) {
            if (tab == currentTab())
                statusAnalysis_->setText(tr("analysis: %1/%2 %3").arg(done).arg(total).arg(name));
        });
        connect(doc, &ProgramDocument::analysisFinished, this,
                [this, tab](int done, int failed, int discovered, qint64 ms) {
                    appendLog(tr("analysis of %1: %2 functions in %3 ms, %4 discovered, %5 failed")
                                  .arg(QFileInfo(tab->document()->path()).fileName())
                                  .arg(done).arg(ms).arg(discovered).arg(failed));
                    if (tab == currentTab()) {
                        statusAnalysis_->setText(tr("idle"));
                        analyzeAction_->setEnabled(true);
                        showAnalysisRundown(tab, done, failed, discovered, ms);
                    }
                });
        connect(doc, &ProgramDocument::functionsChanged, this, [this, tab] {
            if (tab == currentTab()) {
                fillProjectTree();
                fillTables();
                statusArch_->setText(QStringLiteral("%1 · %2 · %3 functions")
                                         .arg(tab->document()->languageId(), tab->document()->formatName())
                                         .arg(tab->document()->functions().size()));
            }
        });
        connect(doc, &ProgramDocument::patchesChanged, this, [this, tab] {
            project_->markDirty();
            if (tab == currentTab())
                savePatchedAction_->setEnabled(tab->document()->patchCount() > 0);
        });
        appendLog(tr("opened %1: %2, %3, %4 functions").arg(path, doc->formatName(), doc->languageId())
                      .arg(doc->functions().size()));
        connect(tab, &ProgramTab::locationChanged, this, [this, tab](quint64 address, const QString &name) {
            if (tab != currentTab())
                return;
            statusAddress_->setText(QStringLiteral("0x%1  %2").arg(address, 0, 16).arg(name));
            functionsPane_->selectAddress(address);
            rememberLocation(address);
            updateReferences(address);
        });
        connect(tab, &ProgramTab::listingChanged, this, [this, tab](const QString &listing) {
            QTimer::singleShot(0, this, &MainWindow::refreshBreakpointMarks);
            if (tab != currentTab())
                return;
            listingPane_->setListing(listing);
            listingPane_->setProgram(tab->document(), tab->currentAddress());
        });
        programStack_->addWidget(tab);
        const int index = programBar_->addTab(QFileInfo(path).fileName());
        programBar_->setTabToolTip(index, path);
        programBar_->setCurrentIndex(index);
        applyProjectState(tab);
        showWorkspace();
        bindCurrentTab();
        tab->showFunction(tab->document()->entryPoint());
        // Which view a program opens in is a setting like any other, so it is
        // read from the same place rather than fixed here.
        const QString wantedView = DecompilerSettings::value(
            QStringLiteral("openView"), DecompilerSettings::programKey(path));
        if (wantedView == QStringLiteral("pseudo"))
            selectView(wantedView);
        statusBar()->clearMessage();
        offerAnalysis(tab);
    });
}

void MainWindow::runMenuHook(const QString &word)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr || !tab->document()->cached(tab->currentAddress())) {
        QTimer::singleShot(250, this, [this, word] { runMenuHook(word); });
        return;
    }
    // `listing:0x...` asks for the menu a listing line raises, which is the
    // only way to see the patch items without a pointer.
    ContextTarget target;
    if (word.startsWith(QStringLiteral("listing:"))) {
        bool ok = false;
        const quint64 line = QStringView(word).mid(QStringLiteral("listing:0x").size())
                                 .toULongLong(&ok, 16);
        target = targetForWord(QString(), ContextTarget::Listing);
        target.line = ok ? line : 0;
        target.hasLine = ok;
    } else {
        target = targetForWord(word, ContextTarget::Source);
    }
    QMenu menu(this);
    fillContextMenu(&menu, target);
    std::fprintf(stderr, "menu for %s:\n", qPrintable(word));
    for (QAction *action : menu.actions()) {
        if (action->isSeparator()) {
            std::fprintf(stderr, "  --\n");
            continue;
        }
        const QString keys = action->shortcut().toString(QKeySequence::PortableText);
        std::fprintf(stderr, "  %s%s\n", qPrintable(action->text()),
                     keys.isEmpty() ? "" : qPrintable(QStringLiteral("  [%1]").arg(keys)));
    }
    QTimer::singleShot(100, qApp, &QCoreApplication::quit);
}

void MainWindow::showDecompilerSettings()
{
    ProgramTab *tab = currentTab();
    OptionsDialog dialog(tab != nullptr ? tab->document() : nullptr, this);
    connect(&dialog, &OptionsDialog::printingChanged, this, [this] {
        // Printing settings change the text alone, so the function is written
        // out again from what was already recovered.
        if (ProgramTab *open = currentTab()) {
            open->document()->forgetAll();
            open->refreshCurrent();
        }
    });
    connect(&dialog, &OptionsDialog::analysisChanged, this, [this] {
        // What is recovered changes only when the code is read again, so the
        // window says so rather than leaving the settings looking inert.
        if (currentTab() == nullptr)
            return;
        const auto answer = QMessageBox::question(
            this, tr("Analysis settings changed"),
            tr("These settings change what the decompiler recovers, so they show only once "
               "the code has been read again. Analyze the program now?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes)
            analyzeCurrent();
    });
    dialog.exec();
}

void MainWindow::runOptionsHook(const QString &spec, bool quitAfter)
{
    ProgramTab *tab = currentTab();
    OptionsDialog dialog(tab != nullptr ? tab->document() : nullptr, this);
    QString rest = spec;
    if (rest.startsWith(QStringLiteral("program:"))) {
        dialog.setPerProgram(true);
        rest = rest.mid(QStringLiteral("program:").size());
    }
    std::fprintf(stderr, "options-before\n%s", qPrintable(dialog.describe()));
    // A modal dialog is not part of the window, so a picture of the window
    // cannot show one; it has to be asked for its own.
    const QString picture = qEnvironmentVariable("ASTRAL_GUI_OPTIONS_SHOT");
    if (!picture.isEmpty()) {
        dialog.show();
        dialog.grab().save(picture);
    }
    QStringList problems;
    for (const QString &pair : rest.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const int equals = pair.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        const QString name = pair.left(equals).trimmed();
        const QString value = pair.mid(equals + 1).trimmed();
        if (!dialog.setShown(name, value))
            problems << tr("no setting named %1").arg(name);
    }
    if (!rest.isEmpty())
        dialog.apply(problems);
    std::fprintf(stderr, "options-after\n%s", qPrintable(dialog.describe()));
    for (const QString &problem : problems)
        std::fprintf(stderr, "options-refused %s\n", qPrintable(problem));
    if (ProgramTab *open = currentTab()) {
        open->document()->forgetAll();
        open->refreshCurrent();
    }
    // A picture of the window is taken on its own timer, so the hook leaves
    // the application running when one was asked for.
    if (quitAfter)
        QTimer::singleShot(2500, qApp, &QCoreApplication::quit);
}

void MainWindow::runRenameHook()
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr || !tab->document()->cached(tab->currentAddress())) {
        QTimer::singleShot(250, this, &MainWindow::runRenameHook);
        return;
    }

    // A project first, so what the renames record has somewhere to go.
    const QString directory = qEnvironmentVariable("ASTRAL_GUI_PROJECT");
    if (!directory.isEmpty() && !project_->isOpen()) {
        QString error;
        if (!project_->openAt(directory, error) && !project_->createAt(directory, error))
            appendLog(tr("project %1: %2").arg(directory, error));
        if (project_->isOpen() && !project_->project()->contains(tab->document()->path())
            && !project_->addProgram(tab->document()->path(), error))
            appendLog(tr("project %1: %2").arg(directory, error));
        addProgramAction_->setEnabled(project_->isOpen());
    }

    // Each step is `word=name`. The word is looked up exactly as the menu
    // looks it up, so this runs the same code the menu item runs. A rename
    // throws away what was read, and reading it again is not immediate, so the
    // steps are taken one at a time as the body comes back.
    auto steps = std::make_shared<QStringList>(
        qEnvironmentVariable("ASTRAL_GUI_RENAME").split(QLatin1Char(';'), Qt::SkipEmptyParts));
    auto next = std::make_shared<std::function<void()>>();
    *next = [this, steps, next] {
        ProgramTab *here = currentTab();
        if (here == nullptr || !here->document()->cached(here->currentAddress())) {
            QTimer::singleShot(100, this, [next] { (*next)(); });
            return;
        }
        if (!steps->isEmpty()) {
            const QString step = steps->takeFirst();
            const int split = step.indexOf(QLatin1Char('='));
            if (split > 0) {
                const QString word = step.left(split).trimmed();
                const QString name = step.mid(split + 1).trimmed();
                ContextTarget target = targetForWord(word, ContextTarget::Source);
                if (!target.hasAddress && !target.isLocal) {
                    std::fprintf(stderr, "rename: %s is not a name here\n", qPrintable(word));
                } else {
                    std::fprintf(stderr, "rename: %s -> %s (%s)\n", qPrintable(word),
                                 qPrintable(name), target.isLocal ? "value" : "symbol");
                    applyRename(target, name, false);
                }
            }
            QTimer::singleShot(100, this, [next] { (*next)(); });
            return;
        }

        if (project_->isOpen())
            saveProject(false);

        // Everything a view would be showing goes to stderr, where the test
        // reads it back.
        ProgramDocument *doc = here->document();
        std::fprintf(stderr, "== functions\n");
        for (const FunctionEntry &f : doc->functions())
            std::fprintf(stderr, "  %s 0x%llx\n", qPrintable(f.name),
                         static_cast<unsigned long long>(f.address));
        std::fprintf(stderr, "== symbols\n");
        for (const SymbolEntry &sym : doc->symbols())
            std::fprintf(stderr, "  %s 0x%llx\n", qPrintable(sym.name),
                         static_cast<unsigned long long>(sym.address));
        if (const auto body = doc->cached(here->currentAddress())) {
            std::fprintf(stderr, "== signature\n  %s\n", qPrintable(body->signature));
            std::fprintf(stderr, "== code\n%s\n", qPrintable(body->code));
            std::fprintf(stderr, "== pseudo\n%s\n", qPrintable(body->pseudoCode));
        }
        std::fprintf(stderr, "== listing\n%s\n", qPrintable(here->listing()));
        QTimer::singleShot(100, qApp, &QCoreApplication::quit);
    };
    (*next)();
}

void MainWindow::runExportHook(const QString &outPath)
{
    ProgramTab *tab = currentTab();
    if (!tab) {
        QTimer::singleShot(250, this, [this, outPath] { runExportHook(outPath); });
        return;
    }
    QString error;
    const QString code = tab->document()->exportC(error);
    if (!error.isEmpty()) {
        appendLog(tr("export failed: %1").arg(error));
    } else {
        QFile file(outPath);
        file.open(QIODevice::WriteOnly | QIODevice::Text);
        file.write(code.toUtf8());
        appendLog(tr("exported %1 bytes of C to %2").arg(code.size()).arg(outPath));
    }
    QTimer::singleShot(200, qApp, &QCoreApplication::quit);
}

void MainWindow::runDebugHook(const QString &target, const QString &arguments)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr) {
        QTimer::singleShot(250, this, [this, target, arguments] { runDebugHook(target, arguments); });
        return;
    }
    setDebugging(true);
    const auto address = tab->document()->resolveName(target);
    debuggerPane_->runForTesting(address.value_or(0),
                                 arguments.split(QLatin1Char(' '), Qt::SkipEmptyParts));
}

void MainWindow::runAnalyzeHook()
{
    ProgramTab *tab = currentTab();
    if (!tab) {
        QTimer::singleShot(250, this, &MainWindow::runAnalyzeHook);
        return;
    }
    // A scripted run writes the settings the same way the dialog does, so it
    // exercises the path the window uses rather than one of its own.
    const QString scope = qEnvironmentVariable("ASTRAL_GUI_ANALYZE_SCOPE");
    if (!scope.isEmpty())
        Settings::instance().setString(QStringLiteral("analysis.scope"),
                                       scope == QStringLiteral("entry")
                                           ? QStringLiteral("entrypoints")
                                           : scope == QStringLiteral("one")
                                                 ? QStringLiteral("function")
                                                 : scope);
    if (qEnvironmentVariable("ASTRAL_GUI_ANALYZE_DISCOVER") == QStringLiteral("0"))
        Settings::instance().setBool(QStringLiteral("analysis.discover"), false);
    connect(tab->document(), &ProgramDocument::analysisFinished, this,
            [](int, int, qint64) { QTimer::singleShot(300, qApp, &QCoreApplication::quit); });
    analyzeCurrent();
}

void MainWindow::runEditHook(const QString &sourceFile, const QString &outPath)
{
    ProgramTab *tab = currentTab();
    if (!tab || !tab->document()->cached(tab->currentAddress())) {
        // Opening and the first decompile are asynchronous; try again shortly.
        QTimer::singleShot(250, this, [this, sourceFile, outPath] { runEditHook(sourceFile, outPath); });
        return;
    }
    QFile file(sourceFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    tab->replaceCodeText(QString::fromUtf8(file.readAll()));
    connect(tab->document(), &ProgramDocument::patchesChanged, this, [this, tab, outPath] {
        if (!outPath.isEmpty()) {
            QString error;
            if (!tab->document()->writePatched(outPath, error))
                appendLog(error);
        }
        QTimer::singleShot(outPath.isEmpty() ? 3000 : 400, qApp, &QCoreApplication::quit);
    });
    tab->compileCurrent();
    // Reproduces a view switch while the compiler runs.
    if (qEnvironmentVariableIsSet("ASTRAL_GUI_SWITCH"))
        for (int ms = 20; ms < 1500; ms += 45)
            QTimer::singleShot(ms, this, [this, ms] {
                selectView((ms / 45) % 2 ? QStringLiteral("pseudo") : QStringLiteral("code"));
            });
}

void MainWindow::runAssembleHook(const QString &edit, const QString &outPath)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr || tab->listing().isEmpty()) {
        QTimer::singleShot(250, this, [this, edit, outPath] { runAssembleHook(edit, outPath); });
        return;
    }
    // `edit` is `<from>|<to>`: the first line holding <from> is rewritten.
    const QStringList parts = edit.split(QLatin1Char('|'));
    QStringList lines = tab->listing().split(QLatin1Char('\n'));
    for (QString &line : lines)
        if (parts.size() == 2 && line.contains(parts[0])) {
            line.replace(parts[0], parts[1]);
            break;
        }
    listingPane_->view()->setPlainText(lines.join(QLatin1Char('\n')));
    connect(tab->document(), &ProgramDocument::patchesChanged, this, [this, tab, outPath] {
        QString error;
        if (!tab->document()->writePatched(outPath, error))
            appendLog(error);
        QTimer::singleShot(300, qApp, &QCoreApplication::quit);
    });
    listingPane_->assembleForTesting();
}

void MainWindow::dumpListing()
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr || tab->listing().isEmpty()) {
        QTimer::singleShot(250, this, &MainWindow::dumpListing);
        return;
    }
    std::fprintf(stderr, "%s\n", qPrintable(tab->listing()));
    QTimer::singleShot(100, qApp, &QCoreApplication::quit);
}

void MainWindow::typeInSearch(const QString &text)
{
    searchBox_->setFocus();
    searchBox_->setText(text);
}

void MainWindow::selectView(const QString &name)
{
    const QStringList names = {QStringLiteral("code"), QStringLiteral("pseudo"), QStringLiteral("graph"), QStringLiteral("hex")};
    const int index = names.indexOf(name.toLower());
    if (index >= 0) {
        viewBar_->setCurrentIndex(index);
        return;
    }
    // Otherwise a pane: raise the dock whose object name starts with it.
    for (QDockWidget *pane : panes_)
        if (pane->objectName().startsWith(name.toLower())) {
            pane->show();
            pane->raise();
        }
}

ProgramTab *MainWindow::currentTab() const
{
    return qobject_cast<ProgramTab *>(programStack_->currentWidget());
}

void MainWindow::closeProgram(int index)
{
    if (index < 0 || index >= programStack_->count())
        return;
    QWidget *tab = programStack_->widget(index);
    programStack_->removeWidget(tab);
    programBar_->removeTab(index);
    delete tab;
    if (programStack_->count() == 0)
        showWelcome();
}

void MainWindow::bindCurrentTab()
{
    ProgramTab *tab = currentTab();
    viewBar_->setEnabled(tab != nullptr);
    if (tab)
        viewBar_->setCurrentIndex(static_cast<int>(tab->view()));
    if (!tab) {
        functionsPane_->setSourceModel(nullptr);
        listingPane_->setListing(QString());
        listingPane_->setProgram(nullptr, 0);
        statusArch_->clear();
        statusAddress_->clear();
        return;
    }
    ProgramDocument *document = tab->document();
    functionsPane_->setSourceModel(tab->functionModel());
    listingPane_->setListing(tab->listing());
    listingPane_->setProgram(document, tab->currentAddress());
    statusArch_->setText(QStringLiteral("%1 · %2 · %3 functions")
                             .arg(document->languageId(), document->formatName())
                             .arg(document->functions().size()));
    setWindowTitle(QStringLiteral("%1 - Astral").arg(QFileInfo(document->path()).fileName()));
    if (tab->currentAddress() != 0)
        functionsPane_->selectAddress(tab->currentAddress());
    debuggerPane_->setProgram(document->path());
    savePatchedAction_->setEnabled(document->patchCount() > 0);
    analyzeAction_->setEnabled(!document->analyzing());
    statusAnalysis_->setText(document->analyzing() ? tr("analysis running") : tr("idle"));
    fillProjectTree();
    fillTables();
}

void MainWindow::setDebugging(bool debugging)
{
    if (debugging == debugging_) {
        // The menu entry and a run that started on its own both arrive here,
        // so the tick stays true to what is showing either way.
        if (debuggerAction_ != nullptr)
            debuggerAction_->setChecked(debugging);
        return;
    }
    debugging_ = debugging;
    if (debuggerAction_ != nullptr)
        debuggerAction_->setChecked(debugging);
    if (debugBar_ != nullptr)
        debugBar_->setVisible(debugging);
    if (debugging) {
        // Keep the layout the program was being read in, so stopping puts it
        // back rather than leaving the window rearranged.
        beforeDebugging_ = saveState();
        for (QDockWidget *pane : {registersDock_, stackDock_, outputDock_}) {
            pane->show();
            pane->raise();
        }
        if (listingDock_ != nullptr) {
            listingDock_->show();
            listingDock_->raise();
        }
        resizeDocks({registersDock_, stackDock_}, {260, 200}, Qt::Vertical);
        statusAnalysis_->setText(tr("debugging"));
    } else {
        if (!beforeDebugging_.isEmpty())
            restoreState(beforeDebugging_);
        for (QDockWidget *pane : {registersDock_, stackDock_, outputDock_})
            pane->hide();
        statusAnalysis_->setText(tr("idle"));
    }
}

void MainWindow::refreshBreakpointMarks()
{
    if (listingView_ == nullptr || debuggerPane_ == nullptr)
        return;
    std::vector<quint64> marks;
    for (int line = 0; line < listingView_->document()->blockCount(); ++line)
        if (const auto address = listingView_->addressAtLine(line))
            if (debuggerPane_->hasBreakpoint(*address))
                marks.push_back(*address);
    listingView_->setGutterMarks(marks, debuggerPane_->currentAddress());
}

void MainWindow::fillTables()
{
    ProgramTab *tab = currentTab();
    if (!tab) {
        for (TablePane *pane : {symbolsPane_, stringsPane_, segmentsPane_, importsPane_})
            pane->clear();
        return;
    }
    ProgramDocument *document = tab->document();
    auto hex = [](quint64 value) { return QStringLiteral("%1").arg(value, 0, 16); };

    std::vector<TablePane::Row> rows;
    for (const SymbolEntry &sym : document->symbols())
        rows.push_back({{sym.name, hex(sym.address), sym.size ? QString::number(sym.size) : QString(),
                         sym.isImport ? tr("import") : sym.isFunction ? tr("function") : tr("data")},
                        sym.address});
    symbolsPane_->setRows(rows, {1}, {2});

    rows.clear();
    for (const StringEntry &str : document->strings())
        rows.push_back({{hex(str.address), QString::number(str.text.size()), str.segment, str.text}, str.address});
    stringsPane_->setRows(rows, {0, 3}, {1});

    rows.clear();
    for (const SegmentEntry &seg : document->segments())
        rows.push_back({{seg.name, hex(seg.address), hex(seg.address + seg.size), QString::number(seg.size),
                         QStringLiteral("r%1%2").arg(seg.writable ? QStringLiteral("w") : QStringLiteral("-"),
                                                      seg.executable ? QStringLiteral("x") : QStringLiteral("-"))},
                        seg.address});
    segmentsPane_->setRows(rows, {1, 2, 4}, {3});

    rows.clear();
    for (const FunctionEntry &f : document->functions())
        if (f.isImport)
            rows.push_back({{f.name, hex(f.address)}, f.address});
    importsPane_->setRows(rows, {1});
}

void MainWindow::fillProjectTree()
{
    projectTree_->clear();
    if (project_->isOpen()) {
        Project *project = project_->project();
        const int count = static_cast<int>(project->members().size());
        const QString heading = count == 1 ? tr("%1 · 1 program").arg(project->name())
                                           : tr("%1 · %2 programs").arg(project->name()).arg(count);
        auto *root = new QTreeWidgetItem(projectTree_, {heading});
        root->setToolTip(0, project->directory());
        root->setExpanded(true);
        const auto paths = project_->memberPaths();
        for (size_t i = 0; i < paths.size(); ++i) {
            const QString &path = paths[i];
            ProgramTab *open = tabForPath(path);
            auto *item = new QTreeWidgetItem(root, {project->members()[i].displayName});
            item->setToolTip(0, path);
            item->setData(0, kProgramPathRole, path);
            if (open != nullptr) {
                // The program being looked at is the one whose detail is
                // wanted; the rest stay folded away.
                item->setExpanded(paths.size() == 1 || open == currentTab());
                fillProgramNode(item, open->document());
            } else if (!QFileInfo::exists(path)) {
                item->setText(0, tr("%1 (missing)").arg(project->members()[i].displayName));
            }
        }
        return;
    }
    ProgramTab *tab = currentTab();
    if (!tab)
        return;
    ProgramDocument *document = tab->document();
    auto *root = new QTreeWidgetItem(projectTree_, {QFileInfo(document->path()).fileName()});
    root->setToolTip(0, document->path());
    root->setExpanded(true);
    fillProgramNode(root, document);
}

void MainWindow::fillProgramNode(QTreeWidgetItem *root, ProgramDocument *document)
{
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    auto *info = new QTreeWidgetItem(root, {tr("%1 · %2").arg(document->formatName(), document->languageId())});
    info->setToolTip(0, tr("image base 0x%1, %2-byte pointers").arg(document->imageBase(), 0, 16).arg(document->pointerSize()));
    info->setFlags(Qt::ItemIsEnabled);

    auto *entries = new QTreeWidgetItem(root, {tr("Entry points")});
    for (quint64 e : document->entryPoints()) {
        const auto f = document->functionAt(e);
        auto *item = new QTreeWidgetItem(entries, {QStringLiteral("0x%1  %2").arg(e, 0, 16).arg(f ? f->name : QString())});
        item->setFont(0, mono);
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(e));
    }
    entries->setExpanded(true);

    const auto segments = document->segments();
    auto *segs = new QTreeWidgetItem(root, {tr("Segments (%1)").arg(segments.size())});
    for (const SegmentEntry &seg : segments) {
        auto *item = new QTreeWidgetItem(segs, {QStringLiteral("%1  0x%2  %3 bytes  %4%5")
                                                     .arg(seg.name).arg(seg.address, 0, 16).arg(seg.size)
                                                     .arg(seg.executable ? QStringLiteral("x") : QString(),
                                                          seg.writable ? QStringLiteral("w") : QString())});
        item->setFont(0, mono);
    }

    int imports = 0, own = 0;
    for (const FunctionEntry &f : document->functions())
        (f.isImport ? imports : own)++;
    auto *functions = new QTreeWidgetItem(root, {tr("Functions (%1)").arg(own)});
    auto *importItems = new QTreeWidgetItem(root, {tr("Imports (%1)").arg(imports)});
    for (const FunctionEntry &f : document->functions()) {
        auto *item = new QTreeWidgetItem(f.isImport ? importItems : functions, {f.name});
        item->setToolTip(0, QStringLiteral("0x%1").arg(f.address, 0, 16));
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(f.address));
    }

    // What the user marked, so a bookmark and a note are places that can be
    // walked back to rather than rows in a file nothing reads out.
    const ProgramState kept = document->journal();
    if (!kept.bookmarks.empty()) {
        auto *marks = new QTreeWidgetItem(root, {tr("Bookmarks (%1)").arg(kept.bookmarks.size())});
        marks->setExpanded(true);
        for (const BookmarkRecord &mark : kept.bookmarks) {
            auto *item = new QTreeWidgetItem(marks, {QStringLiteral("0x%1  %2")
                                                         .arg(mark.address, 0, 16).arg(mark.label)});
            item->setFont(0, mono);
            item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(mark.address));
        }
    }
    if (!kept.comments.empty()) {
        auto *notes = new QTreeWidgetItem(root, {tr("Notes (%1)").arg(kept.comments.size())});
        notes->setExpanded(true);
        for (const CommentRecord &note : kept.comments) {
            auto *item = new QTreeWidgetItem(notes, {QStringLiteral("0x%1  %2")
                                                         .arg(note.address, 0, 16).arg(note.body)});
            item->setFont(0, mono);
            item->setToolTip(0, note.body);
            item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(note.address));
        }
    }
}

void MainWindow::offerAnalysis(ProgramTab *tab)
{
    Settings &settings = Settings::instance();
    const QString key = QStringLiteral("analysis.offerOnOpen");
    if (!settings.boolValue(key, true))
        return;
    const int functions = static_cast<int>(tab->document()->functions().size());
    // One or two functions decompile as fast as the question is read.
    if (functions < 4)
        return;

    ProgramDocument *doc = tab->document();
    int imports = 0;
    for (const FunctionEntry &f : doc->functions())
        if (f.isImport)
            ++imports;
    quint64 mapped = 0;
    for (const SegmentEntry &segment : doc->segments())
        mapped += segment.size;

    QMessageBox box(this);
    box.setWindowTitle(tr("Analyze this program?"));
    box.setIcon(QMessageBox::Question);
    box.setText(tr("%1").arg(QFileInfo(doc->path()).fileName()));
    box.setInformativeText(tr("%1 · %2\n%3 functions, %4 imported\n%5 segments, %6 KB mapped")
                               .arg(doc->formatName(), doc->languageId())
                               .arg(functions).arg(imports)
                               .arg(doc->segments().size()).arg(mapped / 1024));
    QPushButton *run = box.addButton(tr("Analyze Now"), QMessageBox::AcceptRole);
    QPushButton *later = box.addButton(tr("Not Now"), QMessageBox::RejectRole);
    // Declining is the default, so a stray Return does not start a long job.
    box.setDefaultButton(later);
    box.setEscapeButton(later);
    QCheckBox *remember = new QCheckBox(tr("Don't ask again"), &box);
    remember->setToolTip(tr("Recorded as %1 in %2.").arg(key, Settings::path()));
    box.setCheckBox(remember);
    box.exec();
    if (remember->isChecked())
        settings.setBool(key, false);
    if (box.clickedButton() == run)
        analyzeCurrent();
}

void MainWindow::showAnalysisRundown(ProgramTab *tab, int done, int failed, int discovered, qint64 ms)
{
    Settings &settings = Settings::instance();
    const QString key = QStringLiteral("analysis.showRundown");
    if (!settings.boolValue(key, true))
        return;
    ProgramDocument *doc = tab->document();
    int imports = 0, named = 0;
    for (const FunctionEntry &f : doc->functions()) {
        if (f.isImport)
            ++imports;
        else if (!f.name.startsWith(QStringLiteral("sub")) && !f.name.startsWith(QStringLiteral("FUN_")))
            ++named;
    }
    const int total = static_cast<int>(doc->functions().size());

    QStringList rows;
    rows << tr("Decompiled: %1").arg(done);
    rows << tr("Newly found: %1").arg(discovered);
    rows << tr("Failed: %1").arg(failed);
    rows << tr("Functions now known: %1 (%2 named, %3 imported)").arg(total).arg(named).arg(imports);
    rows << tr("Strings: %1").arg(doc->strings().size());
    rows << tr("Time: %1 ms").arg(ms);

    QMessageBox box(this);
    box.setWindowTitle(tr("Analysis finished"));
    box.setIcon(failed > 0 ? QMessageBox::Warning : QMessageBox::Information);
    box.setText(tr("%1 is analyzed.").arg(QFileInfo(doc->path()).fileName()));
    box.setInformativeText(rows.join(QLatin1Char('\n')));
    box.addButton(QMessageBox::Ok);
    QCheckBox *remember = new QCheckBox(tr("Don't show this again"), &box);
    remember->setToolTip(tr("Recorded as %1 in %2.").arg(key, Settings::path()));
    box.setCheckBox(remember);
    box.exec();
    if (remember->isChecked())
        settings.setBool(key, false);
}

QMenu *MainWindow::buildAnalyzeMenu()
{
    // Every Analysis setting there is, read from the same table the settings
    // dialog and the command line read, so nothing here is invented and
    // nothing drifts. Changing one writes the setting; the next run reads it.
    auto *menu = new QMenu(this);
    menu->setToolTipsVisible(true);
    Settings &settings = Settings::instance();

    for (const OptionInfo &option : DecompilerSettings::options()) {
        if (option.group != QStringLiteral("Analysis")
            || option.scope != OptionInfo::Scope::Interface)
            continue;
        const QString name = option.name;
        const QString label = option.label;
        const QString tip = option.explanation;
        const QString fallback = option.defaultValue;

        switch (option.kind) {
        case OptionInfo::Kind::Boolean: {
            QAction *action = menu->addAction(label);
            action->setToolTip(tip);
            action->setCheckable(true);
            action->setChecked(settings.boolValue(name, fallback == QStringLiteral("on")));
            connect(action, &QAction::toggled, this,
                    [name](bool on) { Settings::instance().setBool(name, on); });
            break;
        }
        case OptionInfo::Kind::Choice: {
            QMenu *sub = menu->addMenu(label);
            sub->setToolTipsVisible(true);
            auto *group = new QActionGroup(sub);
            const QString held = settings.stringValue(name, fallback);
            for (const QString &value : option.choices) {
                QAction *action = sub->addAction(value);
                action->setToolTip(tip);
                action->setCheckable(true);
                action->setChecked(value == held);
                action->setActionGroup(group);
                connect(action, &QAction::toggled, this, [name, value](bool on) {
                    if (on)
                        Settings::instance().setString(name, value);
                });
            }
            break;
        }
        case OptionInfo::Kind::Integer: {
            QMenu *sub = menu->addMenu(label);
            sub->setToolTipsVisible(true);
            auto *group = new QActionGroup(sub);
            const int held = settings.intValue(name, fallback.toInt());
            for (int value : {0, 1, 2, 4, 8, 16}) {
                if (value < option.minimum || value > option.maximum)
                    continue;
                QAction *action = sub->addAction(value == 0 ? tr("one per core") : QString::number(value));
                action->setToolTip(tip);
                action->setCheckable(true);
                action->setChecked(value == held);
                action->setActionGroup(group);
                connect(action, &QAction::toggled, this, [name, value](bool on) {
                    if (on)
                        Settings::instance().setInt(name, value);
                });
            }
            break;
        }
        }
    }

    menu->addSeparator();
    menu->addAction(tr("All Decompiler Settings..."), this, &MainWindow::showDecompilerSettings);
    menu->addAction(tr("Stop"), this, [this] {
        if (ProgramTab *tab = currentTab())
            tab->document()->cancelAnalysis();
    });
    return menu;
}

void MainWindow::analyzeCurrent()
{
    ProgramTab *tab = currentTab();
    if (!tab)
        return;
    analyzeAction_->setEnabled(false);
    AnalysisRequest request;
    const QString scope = Settings::instance().stringValue(QStringLiteral("analysis.scope"),
                                                           QStringLiteral("missing"));
    request.scope = scope == QStringLiteral("everything")  ? AnalysisRequest::Scope::Everything
                    : scope == QStringLiteral("entrypoints") ? AnalysisRequest::Scope::FromEntryPoints
                    : scope == QStringLiteral("function")    ? AnalysisRequest::Scope::OneFunction
                                                             : AnalysisRequest::Scope::Missing;
    request.forget = request.scope == AnalysisRequest::Scope::Everything;
    request.discover = Settings::instance().boolValue(QStringLiteral("analysis.discover"), true);
    request.threads = Settings::instance().intValue(QStringLiteral("analysis.engines"), 0);
    request.only = tab->currentAddress();
    static const QStringList names = {tr("everything, again"), tr("what is missing"),
                                      tr("from the entry points"), tr("this function")};
    const int which = request.scope == AnalysisRequest::Scope::Everything        ? 0
                      : request.scope == AnalysisRequest::Scope::Missing         ? 1
                      : request.scope == AnalysisRequest::Scope::FromEntryPoints ? 2
                                                                                 : 3;
    appendLog(tr("analysis of %1 started: %2%3")
                  .arg(QFileInfo(tab->document()->path()).fileName(), names[which],
                       request.discover ? tr(", finding unnamed functions") : QString()));
    tab->document()->analyzeAll(request);
}

void MainWindow::appendLog(const QString &line)
{
    // The same line on stderr, so a scripted run leaves a trace.
    std::fprintf(stderr, "%s\n", qPrintable(line));
    logView_->appendPlainText(QStringLiteral("%1  %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), line));
}

void MainWindow::applyPatch(const std::function<bool(QString &)> &patch, const QString &what)
{
    ProgramTab *tab = currentTab();
    if (!tab)
        return;
    QString error;
    if (patch(error)) {
        appendLog(tr("patch queued: %1 (%n total)", nullptr, tab->document()->patchCount()).arg(what));
        statusBar()->showMessage(tr("Patch queued: %1. Save Patched Binary writes it out.").arg(what), 5000);
    } else {
        appendLog(tr("patch refused: %1: %2").arg(what, error));
        QMessageBox::warning(this, tr("Patch"), QStringLiteral("%1\n\n%2").arg(what, error));
    }
}

void MainWindow::savePatched()
{
    ProgramTab *tab = currentTab();
    if (!tab)
        return;
    const QString suggested = tab->document()->path() + QStringLiteral(".patched");
    const QString out = QFileDialog::getSaveFileName(this, tr("Save Patched Binary"), suggested);
    if (out.isEmpty())
        return;
    writePatchedWithBackup(tab, out);
}

void MainWindow::writePatchedWithBackup(ProgramTab *tab, const QString &out)
{
    // Overwriting a file the engine read from is only safe with a copy set
    // aside first: a .bak beside it, kept from being clobbered on repeats.
    if (QFileInfo::exists(out)) {
        QString backup = out + QStringLiteral(".bak");
        for (int n = 1; QFileInfo::exists(backup); ++n)
            backup = out + QStringLiteral(".bak%1").arg(n);
        if (!QFile::copy(out, backup)) {
            QMessageBox::warning(this, tr("Save Patched Binary"),
                                 tr("Could not back up %1 before writing it.").arg(out));
            return;
        }
        appendLog(tr("backed up %1 to %2").arg(out, QFileInfo(backup).fileName()));
    }

    // The engine reads the file it opened while it writes, so the result is
    // built beside the target and moved into place once it is complete.
    const QString staged = out + QStringLiteral(".astral-new");
    QFile::remove(staged);
    QString error;
    if (!tab->document()->writePatched(staged, error)) {
        QFile::remove(staged);
        appendLog(tr("writing the patched binary failed: %1").arg(error));
        tab->reportPatchFailed(error);
        QMessageBox::warning(this, tr("Save Patched Binary"), error);
        return;
    }
    QFile::remove(out);
    if (!QFile::rename(staged, out)) {
        const QString problem = tr("Wrote %1 but could not move it onto %2.").arg(staged, out);
        appendLog(problem);
        tab->reportPatchFailed(problem);
        QMessageBox::warning(this, tr("Save Patched Binary"), problem);
        return;
    }
    QFile::setPermissions(out, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                   | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                   | QFileDevice::ReadOther | QFileDevice::ExeOther);
    appendLog(tr("wrote patched binary to %1").arg(out));
    statusBar()->showMessage(tr("Patched binary written to %1").arg(out), 5000);
    tab->reportPatchWritten();
}

void MainWindow::onPatchApplied(ProgramTab *tab)
{
    if (tab != currentTab())
        return;
    // A scripted run says where the result goes; it must not stop on a dialog.
    if (!qEnvironmentVariable("ASTRAL_GUI_WRITE").isEmpty())
        return;
    Settings &settings = Settings::instance();
    const QString original = tab->document()->path();

    // With the question turned off every patch writes straight over the
    // original, with a backup. The setting is a line in the settings file, so
    // the choice can be taken back without hunting through dialogs.
    if (!settings.boolValue(QStringLiteral("patch.askWhereToWrite"), true)) {
        writePatchedWithBackup(tab, original);
        return;
    }

    QMessageBox box(this);
    box.setWindowTitle(tr("Patch applied"));
    box.setIcon(QMessageBox::Question);
    box.setText(tr("The patch is queued. Where should the patched binary be written?"));
    box.setInformativeText(tr("A backup of the target is made first."));
    QPushButton *inPlace = box.addButton(tr("Overwrite Original"), QMessageBox::AcceptRole);
    QPushButton *choose = box.addButton(tr("Choose File..."), QMessageBox::ActionRole);
    box.addButton(tr("Later"), QMessageBox::RejectRole);
    QCheckBox *remember = new QCheckBox(tr("Always overwrite the original, don't ask again"), &box);
    remember->setToolTip(tr("Recorded as patch.askWhereToWrite in %1, where it can be changed back.")
                             .arg(Settings::path()));
    box.setCheckBox(remember);
    box.exec();

    if (remember->isChecked())
        settings.setBool(QStringLiteral("patch.askWhereToWrite"), false);

    if (box.clickedButton() == inPlace) {
        writePatchedWithBackup(tab, original);
    } else if (box.clickedButton() == choose) {
        const QString out = QFileDialog::getSaveFileName(this, tr("Save Patched Binary"),
                                                         original + QStringLiteral(".patched"));
        if (!out.isEmpty())
            writePatchedWithBackup(tab, out);
    }
}

void MainWindow::goToTarget()
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    const QString target = searchBox_->text().trimmed();
    if (target.isEmpty())
        return;
    // An exact address or name is a destination; anything else is a search.
    // A highlighted match wins: the list is what the user was looking at.
    if (searchResults_->isShowing() && !tab->document()->functionNamed(target)
        && !tab->document()->resolveName(target))
        return;
    if (tab->navigateTo(target)) {
        searchBox_->clear();
        searchResults_->hideMatches();
        return;
    }
    runSearch(target);
}

void MainWindow::newProject()
{
    QString dir = QFileDialog::getSaveFileName(this, tr("New Project"), QString(),
                                               tr("Astral Project (*.astralproj)"));
    if (dir.isEmpty())
        return;
    if (!dir.endsWith(Project::suffix()))
        dir += Project::suffix();
    QString error;
    if (!project_->createAt(dir, error)) {
        QMessageBox::warning(this, tr("Cannot Create Project"), error);
        return;
    }
    WelcomePage::rememberRecent(project_->directory());
    appendLog(tr("created project %1").arg(project_->directory()));
    setWindowTitle(QStringLiteral("%1 - Astral").arg(project_->displayName()));
    addProgramAction_->setEnabled(true);
    showWorkspace();
    fillProjectTree();
    // A project with nothing in it can do nothing, so the obvious next step
    // is offered rather than left to be found in a menu.
    if (QMessageBox::question(this, tr("Add a Program"),
                              tr("%1 is empty. Add a program to it now?").arg(project_->displayName()),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes)
        addProgramToProject();
}

void MainWindow::openProject(const QString &directory)
{
    QString error;
    if (!project_->openAt(directory, error)) {
        QMessageBox::warning(this, tr("Cannot Open Project"), error);
        return;
    }
    WelcomePage::rememberRecent(project_->directory());
    appendLog(tr("opened project %1 with %2 programs")
                  .arg(project_->displayName()).arg(project_->memberPaths().size()));
    setWindowTitle(QStringLiteral("%1 - Astral").arg(project_->displayName()));
    addProgramAction_->setEnabled(true);
    showWorkspace();
    fillProjectTree();
    for (const QString &member : project_->memberPaths()) {
        if (!QFileInfo::exists(member)) {
            appendLog(tr("%1 is in the project but no longer on disk").arg(member));
            continue;
        }
        openPath(member);
    }
}

void MainWindow::addProgramToProject()
{
    if (!project_->isOpen()) {
        QMessageBox::information(this, tr("No Project"),
                                 tr("Create or open a project before adding programs to it."));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Add Program to Project"));
    if (path.isEmpty())
        return;
    QString error;
    if (!project_->addProgram(path, error)) {
        QMessageBox::warning(this, tr("Cannot Add Program"), error);
        return;
    }
    appendLog(tr("added %1 to %2").arg(QFileInfo(path).fileName(), project_->displayName()));
    openPath(path);
    fillProjectTree();
}

void MainWindow::removeProgramFromProject(const QString &binaryPath)
{
    if (!project_->isOpen())
        return;
    const QString name = QFileInfo(binaryPath).fileName();
    if (QMessageBox::question(this, tr("Remove Program"),
                              tr("Remove %1 from the project? Everything recorded about it goes "
                                 "with it; the binary itself is left alone.").arg(name),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    QString error;
    if (!project_->removeProgram(binaryPath, error)) {
        QMessageBox::warning(this, tr("Cannot Remove Program"), error);
        return;
    }
    if (ProgramTab *tab = tabForPath(binaryPath))
        closeProgram(programStack_->indexOf(tab));
    appendLog(tr("removed %1 from %2").arg(name, project_->displayName()));
    fillProjectTree();
}

bool MainWindow::saveProject(bool announce)
{
    if (!project_->isOpen()) {
        if (announce)
            QMessageBox::information(this, tr("No Project"),
                                     tr("There is no project open to save. File > New Project "
                                        "makes one."));
        return false;
    }
    QString error;
    if (!project_->project()->save(error)) {
        QMessageBox::warning(this, tr("Cannot Save Project"), error);
        return false;
    }
    int written = 0;
    for (int i = 0; i < programStack_->count(); ++i) {
        auto *tab = qobject_cast<ProgramTab *>(programStack_->widget(i));
        if (tab == nullptr || !project_->project()->contains(tab->document()->path()))
            continue;
        if (!project_->captureFrom(tab->document(), error)) {
            QMessageBox::warning(this, tr("Cannot Save Project"), error);
            return false;
        }
        ++written;
    }
    project_->markClean();
    appendLog(tr("saved %1: %2 programs").arg(project_->displayName()).arg(written));
    if (announce)
        statusBar()->showMessage(tr("Saved %1").arg(project_->displayName()), 3000);
    return true;
}

void MainWindow::applyProjectState(ProgramTab *tab)
{
    if (tab == nullptr || !project_->isOpen())
        return;
    if (!project_->project()->contains(tab->document()->path()))
        return;
    QStringList warnings;
    QString error;
    if (!project_->applyStateTo(tab->document(), warnings, error)) {
        appendLog(tr("project state for %1 could not be read: %2")
                      .arg(QFileInfo(tab->document()->path()).fileName(), error));
        return;
    }
    for (const QString &warning : warnings)
        appendLog(warning);
    tab->functionModel()->setFunctions(tab->document()->functions());
}

ProgramTab *MainWindow::tabForPath(const QString &path) const
{
    const QString wanted = QFileInfo(path).canonicalFilePath();
    for (int i = 0; i < programStack_->count(); ++i) {
        auto *tab = qobject_cast<ProgramTab *>(programStack_->widget(i));
        if (tab == nullptr)
            continue;
        if (QFileInfo(tab->document()->path()).canonicalFilePath() == wanted && !wanted.isEmpty())
            return tab;
    }
    return nullptr;
}

void MainWindow::showProjectTreeMenu(const QPoint &at)
{
    QTreeWidgetItem *item = projectTree_->itemAt(at);
    QMenu menu(this);
    // An item that stands for an address gets exactly what the same address
    // gets anywhere else in the window.
    const QVariant address = item ? item->data(0, Qt::UserRole) : QVariant();
    if (address.isValid() && address.toULongLong() != 0) {
        projectTree_->setCurrentItem(item);
        fillContextMenu(&menu, targetForAddress(address.toULongLong(),
                                                ContextTarget::ProjectTree));
        if (!menu.isEmpty())
            menu.addSeparator();
    }
    if (project_->isOpen()) {
        menu.addAction(tr("Add Program to Project..."), this, &MainWindow::addProgramToProject);
        const QVariant member = item ? item->data(0, kProgramPathRole) : QVariant();
        if (member.isValid()) {
            const QString path = member.toString();
            menu.addAction(tr("Open %1").arg(QFileInfo(path).fileName()), this,
                           [this, path] { openPath(path); });
            menu.addAction(tr("Remove %1 from Project").arg(QFileInfo(path).fileName()), this,
                           [this, path] { removeProgramFromProject(path); });
        }
        menu.addSeparator();
        menu.addAction(tr("Save Project"), this, [this] { saveProject(true); });
    } else {
        QAction *empty = menu.addAction(tr("No project is open"));
        empty->setEnabled(false);
    }
    menu.exec(projectTree_->viewport()->mapToGlobal(at));
}


void MainWindow::rememberLocation(quint64 address)
{
    if (navigatingHistory_)
        return;
    if (historyAt_ >= 0 && historyAt_ < static_cast<int>(history_.size()) &&
        history_[historyAt_] == address)
        return;
    // A new jump abandons whatever was ahead of the cursor.
    history_.resize(static_cast<size_t>(historyAt_ + 1));
    history_.push_back(address);
    historyAt_ = static_cast<int>(history_.size()) - 1;
}

void MainWindow::navigateHistory(int delta)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    const int want = historyAt_ + delta;
    if (want < 0 || want >= static_cast<int>(history_.size())) {
        statusBar()->showMessage(delta < 0 ? tr("nothing further back")
                                           : tr("nothing further forward"), 2000);
        return;
    }
    historyAt_ = want;
    navigatingHistory_ = true;
    tab->showAddress(history_[static_cast<size_t>(want)]);
    navigatingHistory_ = false;
}

void MainWindow::goToDefinition()
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    const QString word = tab->currentWord();
    const auto address = tab->document()->resolveName(word);
    if (!address) {
        statusBar()->showMessage(word.isEmpty() ? tr("Put the cursor on a name first")
                                                : tr("%1 is not something this program defines").arg(word),
                                 3000);
        return;
    }
    tab->showAddress(*address);
}

QString MainWindow::labelForAddress(quint64 address) const
{
    ProgramTab *tab = currentTab();
    if (tab != nullptr) {
        if (const auto function = tab->document()->functionAt(address))
            return function->name;
        for (const SymbolEntry &symbol : tab->document()->symbols())
            if (symbol.address == address && !symbol.name.isEmpty())
                return symbol.name;
    }
    return QStringLiteral("0x%1").arg(address, 0, 16);
}

MainWindow::ContextTarget MainWindow::targetForWord(const QString &word,
                                                    ContextTarget::Origin origin,
                                                    const QString &lineText) const
{
    ContextTarget target;
    target.origin = origin;
    target.word = word.trimmed();
    target.lineText = lineText;
    ProgramTab *tab = currentTab();
    if (tab == nullptr || target.word.isEmpty())
        return target;
    ProgramDocument *doc = tab->document();
    if (const auto address = doc->resolveName(target.word)) {
        target.address = *address;
        target.hasAddress = true;
        target.isFunction = doc->functionAt(*address).has_value();
        return target;
    }
    // Not a name the program defines. It may still be one of the values inside
    // the function on screen, which only the decompiled body knows about.
    const quint64 here = tab->currentAddress();
    if (const auto function = doc->cached(here)) {
        for (const std::vector<VariableEntry> *values : {&function->parameters, &function->locals})
            for (const VariableEntry &value : *values)
                if (value.name == target.word) {
                    target.isLocal = true;
                    target.owner = here;
                    return target;
                }
    }
    return target;
}

MainWindow::ContextTarget MainWindow::targetForAddress(quint64 address,
                                                       ContextTarget::Origin origin) const
{
    ContextTarget target;
    target.origin = origin;
    target.address = address;
    target.hasAddress = address != 0;
    target.word = target.hasAddress ? labelForAddress(address) : QString();
    ProgramTab *tab = currentTab();
    if (tab != nullptr && target.hasAddress)
        target.isFunction = tab->document()->functionAt(address).has_value();
    return target;
}

void MainWindow::fillContextMenu(QMenu *menu, const ContextTarget &target)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    ProgramDocument *doc = tab->document();

    // Groups are divided by a line, and a group that turns out to have nothing
    // in it leaves none behind.
    int marker = static_cast<int>(menu->actions().size());
    auto section = [menu, &marker] {
        if (static_cast<int>(menu->actions().size()) > marker)
            menu->addSeparator();
        marker = static_cast<int>(menu->actions().size());
    };

    const QString label = target.hasAddress ? labelForAddress(target.address) : target.word;
    const quint64 lineAddress = target.hasLine ? target.line : 0;
    // The function the menu is about. A word that names one names it; a value
    // belongs to the one it is declared in; anything else belongs to the one
    // being read, so a click in the middle of a body still offers what can be
    // said about that body.
    const quint64 subject = target.isFunction  ? target.address
                            : target.isLocal   ? target.owner
                                               : tab->currentAddress();
    const QString subjectLabel = subject != 0 ? labelForAddress(subject) : QString();
    const auto function = subject != 0 ? doc->cached(subject) : std::nullopt;
    const QString commentKind = target.origin == ContextTarget::Listing
                                    ? QStringLiteral("listing")
                                    : QStringLiteral("code");

    // ------------------------------------------------------------ navigate
    if (target.hasAddress) {
        menu->addAction(tr("Go to Definition of %1").arg(label), QKeySequence(Qt::CTRL | Qt::Key_B),
                        this, [this, target] { currentTab()->showAddress(target.address); });
        menu->addAction(tr("Cross References to %1").arg(label),
                        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X), this,
                        [this, target] { showReferencesFor(target.address, true, false); });
        if (target.isFunction && function && !function->callees.empty()) {
            menu->addAction(tr("Cross References from %1").arg(label), this,
                            [this, target] { showReferencesFor(target.address, false, true); });
        }
        menu->addAction(tr("Show %1 in Listing").arg(label), this,
                        [this, target] { showInListing(target.address); });
        menu->addAction(tr("Show %1 in Hex").arg(label), this, [this, target] {
            ProgramTab *here = currentTab();
            here->setView(ProgramTab::Hex);
            here->showAddress(target.address);
        });
    }
    if (target.hasLine && !target.hasAddress) {
        menu->addAction(tr("Show 0x%1 in Hex").arg(lineAddress, 0, 16), this,
                        [this, lineAddress] {
                            ProgramTab *here = currentTab();
                            here->setView(ProgramTab::Hex);
                            here->showAddress(lineAddress);
                        });
    }
    menu->addAction(tr("Go to Address..."), QKeySequence(Qt::CTRL | Qt::Key_G), this, [this] {
        searchBox_->setFocus();
        searchBox_->selectAll();
    });
    if (historyAt_ > 0)
        menu->addAction(tr("Back"), QKeySequence(Qt::CTRL | Qt::Key_BracketLeft), this,
                        [this] { navigateHistory(-1); });

    // ---------------------------------------------------------------- edit
    section();
    if (target.isLocal) {
        menu->addAction(tr("Rename %1...").arg(target.word), QKeySequence(Qt::CTRL | Qt::Key_R),
                        this, [this, target] { renameTarget(target); });
    } else if (target.hasAddress) {
        menu->addAction(tr("Rename %1...").arg(label), QKeySequence(Qt::CTRL | Qt::Key_R), this,
                        [this, target] { renameTarget(target); });
    }
    const quint64 noteAt = target.hasAddress ? target.address : lineAddress;
    if (noteAt != 0) {
        const bool has = !doc->commentAt(noteAt, commentKind).isEmpty();
        menu->addAction(has ? tr("Edit Note on %1...").arg(labelForAddress(noteAt))
                            : tr("Set Note on %1...").arg(labelForAddress(noteAt)),
                        QKeySequence(Qt::CTRL | Qt::Key_Semicolon), this,
                        [this, noteAt, commentKind] { setCommentAt(noteAt, commentKind); });
        if (has) {
            menu->addAction(tr("Remove Note"), this, [this, noteAt, commentKind] {
                currentTab()->document()->removeComment(noteAt, commentKind);
                noteProjectEdit();
                fillProjectTree();
            });
        }
    }

    // ------------------------------------------------------------ analysis
    section();
    if (subject != 0 && doc->functionAt(subject)) {
        menu->addAction(tr("Decompile %1").arg(subjectLabel), this,
                        [this, subject] { currentTab()->showFunction(subject); });
        menu->addAction(tr("Force Re-analyse %1").arg(subjectLabel), this,
                        [this, subject] { forceReanalyse(subject); });
    }
    if (function) {
        menu->addAction(tr("What Astral Knows About %1").arg(subjectLabel),
                        QKeySequence(Qt::CTRL | Qt::Key_K), this,
                        [this, subject] { showFunctionFacts(subject); });
        if (!function->namingReason.isEmpty())
            menu->addAction(tr("Why Is It Called %1?").arg(subjectLabel), this,
                            [this, subject] { showNamingReason(subject); });
    }

    // --------------------------------------------------------------- marks
    section();
    const quint64 markAt = lineAddress != 0 ? lineAddress : target.address;
    if (markAt != 0) {
        menu->addAction(debuggerPane_->hasBreakpoint(markAt)
                            ? tr("Clear Breakpoint at 0x%1").arg(markAt, 0, 16)
                            : tr("Set Breakpoint at 0x%1").arg(markAt, 0, 16),
                        this, [this, markAt] {
                            debuggerPane_->toggleBreakpoint(markAt);
                            refreshBreakpointMarks();
                        });
        menu->addAction(doc->hasBookmark(markAt) ? tr("Remove Bookmark") : tr("Bookmark This..."),
                        this, [this, markAt] { toggleBookmarkAt(markAt); });
    }

    // --------------------------------------------------------------- patch
    if (target.origin == ContextTarget::Listing && lineAddress != 0) {
        section();
        menu->addAction(tr("Patch: no-op this instruction"), this, [this, lineAddress] {
            applyPatch([this, lineAddress](QString &error) {
                return currentTab()->document()->patchNop(lineAddress, 1, error);
            }, tr("no-op 1 instruction at 0x%1").arg(lineAddress, 0, 16));
        });
        menu->addAction(tr("Patch: no-op N instructions..."), this, [this, lineAddress] {
            bool ok = false;
            const int count = QInputDialog::getInt(this, tr("No-op"), tr("Instructions"), 1, 1,
                                                   4096, 1, &ok);
            if (!ok)
                return;
            applyPatch([this, lineAddress, count](QString &error) {
                return currentTab()->document()->patchNop(lineAddress, count, error);
            }, tr("no-op %n instruction(s) at 0x%1", nullptr, count).arg(lineAddress, 0, 16));
        });
        menu->addAction(tr("Patch: invert this branch"), this, [this, lineAddress] {
            applyPatch([this, lineAddress](QString &error) {
                return currentTab()->document()->patchInvert(lineAddress, error);
            }, tr("invert branch at 0x%1").arg(lineAddress, 0, 16));
        });
        menu->addAction(tr("Patch: make function return a value..."), this, [this, lineAddress] {
            bool ok = false;
            const QString text = QInputDialog::getText(
                this, tr("Return value"),
                tr("Make the function at 0x%1 return:").arg(lineAddress, 0, 16),
                QLineEdit::Normal, QStringLiteral("0"), &ok);
            if (!ok)
                return;
            bool parsed = false;
            const quint64 value = text.startsWith(QStringLiteral("0x"))
                                      ? QStringView(text).mid(2).toULongLong(&parsed, 16)
                                      : text.toULongLong(&parsed, 10);
            if (!parsed) {
                statusBar()->showMessage(tr("Not a number: %1").arg(text), 3000);
                return;
            }
            applyPatch([this, lineAddress, value](QString &error) {
                return currentTab()->document()->patchReturn(lineAddress, value, error);
            }, tr("return %1 from the function at 0x%2").arg(value).arg(lineAddress, 0, 16));
        });
    }

    // ---------------------------------------------------------------- copy
    section();
    if (!target.word.isEmpty()) {
        menu->addAction(tr("Copy Name"), this,
                        [this, target] { copyToClipboard(target.word, tr("name")); });
    }
    if (target.hasAddress || lineAddress != 0) {
        const quint64 address = target.hasAddress ? target.address : lineAddress;
        menu->addAction(tr("Copy Address"), this, [this, address] {
            copyToClipboard(QStringLiteral("0x%1").arg(address, 0, 16), tr("address"));
        });
    }
    if (!target.lineText.trimmed().isEmpty()) {
        menu->addAction(tr("Copy Line"), this, [this, target] {
            copyToClipboard(target.lineText, tr("line"));
        });
    }
    if (function) {
        menu->addAction(tr("Copy %1 as C").arg(subjectLabel), this, [this, subject] {
            if (const auto body = currentTab()->document()->cached(subject))
                copyToClipboard(body->code, tr("C"));
        });
        menu->addAction(tr("Copy %1 as Pseudo-C").arg(subjectLabel), this, [this, subject] {
            if (const auto body = currentTab()->document()->cached(subject))
                copyToClipboard(body->pseudoCode, tr("pseudo-C"));
        });
    }
    if (subject != 0 && doc->functionAt(subject)) {
        menu->addAction(tr("Copy Disassembly of %1").arg(subjectLabel), this, [this, subject] {
            ProgramDocument *here = currentTab()->document();
            const auto entry = here->functionAt(subject);
            const auto body = here->cached(subject);
            const quint64 size = body ? body->size : entry ? entry->size : 0;
            copyToClipboard(here->disassemble(subject, size), tr("disassembly"));
        });
    }
}

void MainWindow::copyToClipboard(const QString &text, const QString &what)
{
    QApplication::clipboard()->setText(text);
    statusBar()->showMessage(tr("Copied the %1").arg(what), 2000);
}

void MainWindow::showInListing(quint64 address)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    const auto owner = tab->document()->functionAt(address);
    if (owner && owner->address != tab->currentAddress())
        tab->showFunction(owner->address);
    if (listingDock_ != nullptr) {
        listingDock_->show();
        listingDock_->raise();
    }
    // Changing function replaces the listing, so the scroll waits for the
    // text that replaces it.
    QTimer::singleShot(0, this, [this, address] { listingView_->scrollToAddress(address); });
}

void MainWindow::forceReanalyse(quint64 address)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    tab->document()->invalidate(address);
    appendLog(tr("re-reading the function at 0x%1").arg(address, 0, 16));
    tab->showFunction(address);
}

void MainWindow::showNamingReason(quint64 address)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    const auto function = tab->document()->cached(address);
    if (!function || function->namingReason.isEmpty()) {
        statusBar()->showMessage(tr("that name came from the program itself"), 3000);
        return;
    }
    QMessageBox::information(this, tr("Why It Is Called That"),
                             tr("%1 is called that because %2.")
                                 .arg(function->name, function->namingReason));
}

void MainWindow::setCommentAt(quint64 address, const QString &kind)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr || address == 0)
        return;
    ProgramDocument *doc = tab->document();
    bool accepted = false;
    const QString body = QInputDialog::getText(this, tr("Note"),
                                               tr("Note on %1:").arg(labelForAddress(address)),
                                               QLineEdit::Normal, doc->commentAt(address, kind),
                                               &accepted);
    if (!accepted)
        return;
    if (body.trimmed().isEmpty())
        doc->removeComment(address, kind);
    else
        doc->recordComment(address, kind, body.trimmed());
    appendLog(tr("note on 0x%1: %2").arg(address, 0, 16).arg(body.trimmed()));
    noteProjectEdit();
    fillProjectTree();
}

void MainWindow::toggleBookmarkAt(quint64 address)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr || address == 0)
        return;
    ProgramDocument *doc = tab->document();
    if (doc->hasBookmark(address)) {
        doc->removeBookmark(address);
        appendLog(tr("bookmark removed from 0x%1").arg(address, 0, 16));
    } else {
        bool accepted = false;
        const QString label = QInputDialog::getText(this, tr("Bookmark"), tr("Call it:"),
                                                    QLineEdit::Normal, labelForAddress(address),
                                                    &accepted);
        if (!accepted)
            return;
        doc->recordBookmark(address, label.trimmed().isEmpty() ? labelForAddress(address)
                                                               : label.trimmed());
        appendLog(tr("bookmarked 0x%1").arg(address, 0, 16));
    }
    noteProjectEdit();
    fillProjectTree();
}

void MainWindow::noteProjectEdit()
{
    if (project_->isOpen()) {
        project_->markDirty();
        return;
    }
    if (warnedNoProject_)
        return;
    warnedNoProject_ = true;
    const QString message = tr("No project is open, so this edit lasts only as long as Astral "
                               "is running.");
    appendLog(message);
    if (scriptedRun())
        return;
    if (QMessageBox::question(this, tr("Nothing Is Keeping This"),
                              tr("%1\n\nMake a project around this program now?").arg(message),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
        == QMessageBox::Yes)
        createProjectForCurrent();
}

void MainWindow::createProjectForCurrent()
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    QString directory = QFileDialog::getSaveFileName(this, tr("New Project"), QString(),
                                                     tr("Astral Project (*.astralproj)"));
    if (directory.isEmpty())
        return;
    if (!directory.endsWith(Project::suffix()))
        directory += Project::suffix();
    QString error;
    if (!project_->createAt(directory, error)) {
        QMessageBox::warning(this, tr("Cannot Create Project"), error);
        return;
    }
    if (!project_->addProgram(tab->document()->path(), error)) {
        QMessageBox::warning(this, tr("Cannot Add Program"), error);
        return;
    }
    WelcomePage::rememberRecent(project_->directory());
    setWindowTitle(QStringLiteral("%1 - Astral").arg(project_->displayName()));
    addProgramAction_->setEnabled(true);
    appendLog(tr("created project %1 around %2")
                  .arg(project_->directory(), QFileInfo(tab->document()->path()).fileName()));
    saveProject(false);
    fillProjectTree();
}

void MainWindow::refreshAfterEdit(quint64 address)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    tab->functionModel()->setFunctions(tab->document()->functions());
    fillTables();
    fillProjectTree();
    // Whatever is on screen printed the old name too, so that is what gets
    // read again: renaming from a call site leaves the reader where they were,
    // looking at the new name. The renamed function is read afresh whenever it
    // is next opened, because the edit dropped everything the document held.
    const quint64 showing = tab->currentAddress() != 0 ? tab->currentAddress() : address;
    updateReferences(showing);
    tab->showFunction(showing);
}

bool MainWindow::applyRename(const ContextTarget &target, const QString &name, bool learn)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return false;
    ProgramDocument *doc = tab->document();
    const QString wanted = name.trimmed();
    if (wanted.isEmpty())
        return false;
    QString error;

    if (target.isLocal) {
        if (!doc->renameLocal(target.owner, target.word, wanted, error)) {
            QMessageBox::warning(this, tr("Rename Failed"), error);
            return false;
        }
        appendLog(tr("renamed %1 to %2 in the function at 0x%3")
                      .arg(target.word, wanted).arg(target.owner, 0, 16));
        noteProjectEdit();
        refreshAfterEdit(target.owner);
        return true;
    }

    if (!target.hasAddress)
        return false;
    if (!doc->rename(target.address, wanted, learn, error)) {
        QMessageBox::warning(this, tr("Rename Failed"), error);
        return false;
    }
    appendLog(tr("renamed 0x%1 to %2%3").arg(target.address, 0, 16)
                  .arg(wanted, learn ? tr(" (remembered)") : QString()));
    noteProjectEdit();
    // Every caller printed the old name, so the whole reading of the program
    // is stale; the document drops it, and the panes are filled again here.
    refreshAfterEdit(target.address);
    return true;
}

void MainWindow::renameTarget(const ContextTarget &target)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    if (!target.isLocal && !target.hasAddress) {
        statusBar()->showMessage(target.word.isEmpty()
                                     ? tr("Put the cursor on a name first")
                                     : tr("%1 is not something this program names").arg(target.word),
                                 3000);
        return;
    }

    if (target.isLocal) {
        bool accepted = false;
        const QString name = QInputDialog::getText(
            this, tr("Rename Value"),
            tr("Name for %1 in %2:").arg(target.word, labelForAddress(target.owner)),
            QLineEdit::Normal, target.word, &accepted);
        if (accepted)
            applyRename(target, name, false);
        return;
    }

    const auto entry = tab->document()->functionAt(target.address);
    const QString current = entry ? entry->name : labelForAddress(target.address);
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Rename"),
                                               tr("Name for %1 at 0x%2:")
                                                   .arg(current).arg(target.address, 0, 16),
                                               QLineEdit::Normal, current, &accepted);
    if (!accepted || name.trimmed().isEmpty())
        return;
    // Learning records the name against a fingerprint of the body, so the same
    // code is recognised the next time it turns up in another program.
    const bool learn = QMessageBox::question(this, tr("Remember This Name"),
                                             tr("Recognise this code as %1 in other programs?")
                                                 .arg(name.trimmed()),
                                             QMessageBox::Yes | QMessageBox::No,
                                             QMessageBox::Yes) == QMessageBox::Yes;
    applyRename(target, name, learn);
}

void MainWindow::renameCurrent()
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    // The keyboard shortcut acts on whatever the cursor sits in, the same as
    // the menu item does; the current function is what it falls back to.
    ContextTarget target = targetForWord(tab->currentWord(), ContextTarget::Source);
    if (!target.isLocal && !target.hasAddress)
        target = targetForAddress(tab->currentAddress(), ContextTarget::Source);
    renameTarget(target);
}

void MainWindow::updateReferences(quint64 address, bool incoming, bool outgoing)
{
    if (referencesPane_ == nullptr)
        return;
    ProgramTab *tab = currentTab();
    if (tab == nullptr) {
        referencesPane_->clear();
        return;
    }
    ProgramDocument *doc = tab->document();
    std::vector<TablePane::Row> rows;
    if (incoming) {
        for (const Reference &ref : doc->callersOf(address)) {
            rows.push_back({{tr("called by"), ref.fromName,
                             QStringLiteral("0x%1").arg(ref.from, 0, 16)},
                            ref.from});
        }
    }
    if (outgoing) {
        if (const auto function = doc->cached(address)) {
            for (const CallSite &call : function->callees) {
                rows.push_back({{tr("calls"), call.name,
                                 QStringLiteral("0x%1").arg(call.address, 0, 16)},
                                call.address});
            }
        }
    }
    referencesPane_->setRows(rows, {2});
}

void MainWindow::showReferencesFor(quint64 address, bool incoming, bool outgoing)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    // What refers to a function is only known from bodies already read, so
    // the one asked about is read first if it has not been.
    if (!tab->document()->cached(address))
        tab->document()->decompile(address);
    updateReferences(address, incoming, outgoing);
    if (referencesDock_ != nullptr) {
        referencesDock_->show();
        referencesDock_->raise();
    }
    if (!incoming)
        return;
    ProgramDocument *doc = tab->document();
    const int indexed = doc->indexedFunctions();
    const int total = static_cast<int>(doc->functions().size());
    if (indexed < total) {
        statusBar()->showMessage(tr("callers known for %1 of %2 functions; "
                                    "run Analyze Program for the rest")
                                     .arg(indexed).arg(total), 6000);
    }
}

void MainWindow::showReferences()
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    updateReferences(tab->currentAddress());
    if (referencesDock_ != nullptr) {
        referencesDock_->show();
        referencesDock_->raise();
    }
    ProgramDocument *doc = tab->document();
    // Callers are only known for functions already decompiled; say so rather
    // than showing an empty pane that looks like an answer.
    const int indexed = doc->indexedFunctions();
    const int total = static_cast<int>(doc->functions().size());
    if (indexed < total) {
        statusBar()->showMessage(tr("callers known for %1 of %2 functions; "
                                    "run Analyze Program for the rest")
                                     .arg(indexed).arg(total), 6000);
    }
}

void MainWindow::findInProgram()
{
    searchBox_->setFocus();
    searchBox_->selectAll();
}

void MainWindow::runSearch(const QString &needle)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr || needle.isEmpty()) {
        searchResults_->hideMatches();
        return;
    }
    ProgramDocument *doc = tab->document();
    std::vector<SearchResults::Match> matches;
    // An address typed straight in is a destination, and leads.
    if (const auto address = doc->resolveName(needle))
        matches.push_back({tr("address"), QStringLiteral("0x%1").arg(*address, 0, 16), *address});
    for (const FunctionEntry &fn : doc->functions())
        if (fn.name.contains(needle, Qt::CaseInsensitive))
            matches.push_back({fn.isImport ? tr("import") : tr("function"), fn.name, fn.address});
    for (const SymbolEntry &sym : doc->symbols())
        if (!sym.isFunction && sym.name.contains(needle, Qt::CaseInsensitive))
            matches.push_back({tr("symbol"), sym.name, sym.address});
    for (const StringEntry &str : doc->strings())
        if (str.text.contains(needle, Qt::CaseInsensitive))
            matches.push_back({tr("string"), str.text.simplified(), str.address});
    searchResults_->showMatches(matches, needle);
}

void MainWindow::exportSource(bool wholeProgram)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    ProgramDocument *doc = tab->document();
    const QString suggestion =
        wholeProgram ? QFileInfo(doc->path()).completeBaseName() + QStringLiteral(".c")
                     : (doc->functionAt(tab->currentAddress())
                            ? doc->functionAt(tab->currentAddress())->name
                            : QStringLiteral("function")) + QStringLiteral(".c");
    const QString out = QFileDialog::getSaveFileName(this, tr("Export C"), suggestion,
                                                     tr("C source (*.c)"));
    if (out.isEmpty())
        return;
    statusBar()->showMessage(tr("emitting C..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const QString code = wholeProgram ? doc->exportC(error)
                                      : doc->exportFunctionC(tab->currentAddress(), error);
    QApplication::restoreOverrideCursor();
    statusBar()->clearMessage();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Export Failed"), error);
        return;
    }
    QFile file(out);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export Failed"), file.errorString());
        return;
    }
    file.write(code.toUtf8());
    file.close();
    appendLog(tr("wrote %1 (%2 lines)").arg(out).arg(code.count(QLatin1Char('\n')) + 1));
}

void MainWindow::learnNames()
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    const int learned = tab->document()->learnSymbols();
    appendLog(tr("learned %n name(s) from this program", nullptr, learned));
    statusBar()->showMessage(tr("learned %n name(s)", nullptr, learned), 4000);
}

void MainWindow::showFunctionFacts(quint64 address)
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    if (address == 0)
        address = tab->currentAddress();
    const auto function = tab->document()->cached(address);
    if (!function) {
        statusBar()->showMessage(tr("decompile this function first"), 3000);
        return;
    }
    QStringList lines;
    lines << tr("%1 at 0x%2, %3 bytes")
                 .arg(function->name).arg(function->address, 0, 16).arg(function->size);
    lines << tr("signature: %1").arg(function->signature);
    if (!function->callingConvention.isEmpty())
        lines << tr("calling convention: %1").arg(function->callingConvention);
    if (!function->namingReason.isEmpty())
        lines << tr("named because: %1").arg(function->namingReason);
    if (!function->parameters.empty()) {
        QStringList parts;
        for (const VariableEntry &v : function->parameters)
            parts << QStringLiteral("%1 %2").arg(v.type, v.name);
        lines << tr("parameters: %1").arg(parts.join(QStringLiteral(", ")));
    }
    lines << tr("locals: %1, calls: %2, basic blocks: %3")
                 .arg(function->locals.size()).arg(function->callees.size())
                 .arg(function->blocks.size());
    if (!function->appliedRenames.isEmpty())
        lines << tr("names chosen: %1").arg(function->appliedRenames.join(QStringLiteral("; ")));
    for (const QString &comment : function->comments)
        lines << tr("note: %1").arg(comment);
    // Notes the user wrote are facts about the function too, and the only
    // place they are otherwise visible is the project tree.
    for (const QString &kind : {QStringLiteral("code"), QStringLiteral("listing")}) {
        const QString written = tab->document()->commentAt(address, kind);
        if (!written.isEmpty())
            lines << tr("your note: %1").arg(written);
    }
    QMessageBox::information(this, tr("What Astral Knows"), lines.join(QStringLiteral("\n")));
}


void MainWindow::buildMenus()
{
    QMenu *file = titleBar_->menuBar()->addMenu(tr("&File"));
    file->addAction(tr("New Project..."), QKeySequence::New, this, &MainWindow::newProject);
    file->addAction(tr("Open..."), QKeySequence::Open, this, [this] {
        QString path = QFileDialog::getOpenFileName(this, tr("Open Binary or Project"));
        if (!path.isEmpty())
            openPath(path);
    });
    addProgramAction_ = file->addAction(tr("Add Program to Project..."), this,
                                        &MainWindow::addProgramToProject);
    addProgramAction_->setEnabled(false);
    file->addSeparator();
    file->addAction(tr("Save Project"), QKeySequence::Save, this, [this] { saveProject(true); });
    file->addAction(tr("Export C for This Function..."), this,
                    [this] { exportSource(false); });
    file->addAction(tr("Export C for the Whole Program..."), this,
                    [this] { exportSource(true); });
    savePatchedAction_ = file->addAction(tr("Save Patched Binary..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S),
                                         this, &MainWindow::savePatched);
    savePatchedAction_->setEnabled(false);
    file->addSeparator();
    file->addAction(tr("Close Tab"), QKeySequence::Close, this, [this] {
        closeProgram(programBar_->currentIndex());
    });
    file->addAction(tr("Quit"), QKeySequence::Quit, qApp, &QApplication::closeAllWindows);

    QMenu *edit = titleBar_->menuBar()->addMenu(tr("&Edit"));
    edit->addAction(tr("Undo"), QKeySequence::Undo, this, [] {});
    edit->addAction(tr("Redo"), QKeySequence::Redo, this, [] {});
    edit->addSeparator();
    edit->addAction(tr("Rename..."), QKeySequence(Qt::CTRL | Qt::Key_R), this, &MainWindow::renameCurrent);
    edit->addAction(tr("Change Type"), QKeySequence(Qt::CTRL | Qt::Key_T), this, [] {});
    edit->addAction(tr("Note..."), QKeySequence(Qt::CTRL | Qt::Key_Semicolon), this, [this] {
        if (ProgramTab *tab = currentTab())
            setCommentAt(tab->currentAddress(), QStringLiteral("code"));
    });
    edit->addSeparator();
    edit->addAction(tr("Find..."), QKeySequence::Find, this, &MainWindow::findInProgram);

    viewMenu_ = titleBar_->menuBar()->addMenu(tr("&View"));

    QMenu *navigate = titleBar_->menuBar()->addMenu(tr("&Navigate"));
    navigate->addAction(tr("Back"), QKeySequence(Qt::CTRL | Qt::Key_BracketLeft), this,
                        [this] { navigateHistory(-1); });
    navigate->addAction(tr("Forward"), QKeySequence(Qt::CTRL | Qt::Key_BracketRight), this,
                        [this] { navigateHistory(1); });
    navigate->addSeparator();
    navigate->addAction(tr("Go to Address..."), QKeySequence(Qt::CTRL | Qt::Key_G), this, [this] {
        searchBox_->setFocus();
        searchBox_->selectAll();
    });
    navigate->addAction(tr("Go to Definition"), QKeySequence(Qt::CTRL | Qt::Key_B), this,
                        &MainWindow::goToDefinition);
    navigate->addAction(tr("Cross References"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X), this,
                        &MainWindow::showReferences);
    navigate->addAction(tr("Show in Listing"), this, [this] {
        if (ProgramTab *tab = currentTab())
            showInListing(tab->currentAddress());
    });
    navigate->addSeparator();
    // The views of the current program, in the order the tabs sit in.
    const QStringList viewNames = {tr("Code"), tr("Pseudo-C"), tr("Graph"), tr("Hex")};
    for (int i = 0; i < viewNames.size(); ++i)
        navigate->addAction(viewNames[i], QKeySequence(Qt::CTRL | (Qt::Key_1 + i)), this,
                            [this, i] { viewBar_->setCurrentIndex(i); });
    navigate->addAction(tr("Toggle Code / Graph"), QKeySequence(Qt::CTRL | Qt::Key_E), this, [this] {
        if (currentTab() != nullptr)
            viewBar_->setCurrentIndex(viewBar_->currentIndex() == ProgramTab::Graph ? ProgramTab::Code
                                                                                    : ProgramTab::Graph);
    });

    QMenu *analysis = titleBar_->menuBar()->addMenu(tr("&Analysis"));
    analysis->addAction(tr("Analyze Program"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), this,
                        &MainWindow::analyzeCurrent);
    analysis->addAction(tr("Define Function"), this, [] {})->setEnabled(false);
    analysis->addAction(tr("Undefine"), this, [] {})->setEnabled(false);

    QMenu *tools = titleBar_->menuBar()->addMenu(tr("&Tools"));
    tools->addAction(tr("What Astral Knows About This Function"), QKeySequence(Qt::CTRL | Qt::Key_K),
                     this, [this] { showFunctionFacts(); });
    tools->addAction(tr("Learn Names From This Program"), this, &MainWindow::learnNames);
    tools->addSeparator();
    tools->addAction(tr("Decompiler Settings..."), QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
                     &MainWindow::showDecompilerSettings);
    tools->addSeparator();
    tools->addAction(tr("Show Queued Patches"), this, [this] {
        if (ProgramTab *tab = currentTab())
            appendLog(tr("patches:\n%1").arg(tab->document()->patchText().trimmed()));
    });
    tools->addAction(tr("Undo Last Patch"), this, [this] {
        if (ProgramTab *tab = currentTab()) {
            tab->document()->patchUndo();
            appendLog(tr("patch: undid last, %n queued", nullptr, tab->document()->patchCount()));
        }
    });
    tools->addAction(tr("Discard All Patches"), this, [this] {
        if (ProgramTab *tab = currentTab()) {
            tab->document()->patchClear();
            appendLog(tr("patch: discarded all"));
        }
    });

    QMenu *window = titleBar_->menuBar()->addMenu(tr("&Window"));
    window->addAction(tr("Welcome Screen"), this, &MainWindow::showWelcome);
    window->addSeparator();
    window->addAction(tr("Save Current Layout as Default"), this, [this] {
        QSettings().setValue(QStringLiteral("window/defaultState"), saveState());
        statusBar()->showMessage(tr("This layout is now the default. Reset Layout returns to it."), 4000);
    });
    window->addAction(tr("Reset Layout"), this, &MainWindow::resetLayout);
    window->addAction(tr("Restore Built-in Layout"), this, [this] {
        QSettings().remove(QStringLiteral("window/defaultState"));
        resetLayout();
    });

    QMenu *help = titleBar_->menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("Documentation"), this, [] {});
    help->addAction(tr("Knowledge Base"), this, [] {});
    help->addAction(tr("Check for Updates..."), this, [] {});
    help->addSeparator();
    help->addAction(tr("About Astral"), this, [this] {
        QMessageBox::about(this, tr("About Astral"),
                           tr("<b>Astral</b><br>A decompiler that emits C which compiles."));
    });
}

void MainWindow::buildToolBar()
{
    QToolBar *bar = addToolBar(tr("Navigation"));
    navigationBar_ = bar;
    bar->setObjectName(QStringLiteral("navigationBar"));
    bar->setMovable(false);
    bar->setIconSize(QSize(16, 16));

    auto *strip = new QWidget;
    auto *row = new QHBoxLayout(strip);
    row->setContentsMargins(4, 0, 8, 0);
    row->setSpacing(6);
    auto *back = new QToolButton;
    back->setObjectName(QStringLiteral("transportButton"));
    back->setText(QStringLiteral("\u25C2"));
    back->setToolTip(tr("Back"));
    auto *forward = new QToolButton;
    forward->setObjectName(QStringLiteral("transportButton"));
    forward->setText(QStringLiteral("\u25B8"));
    forward->setToolTip(tr("Forward"));
    connect(back, &QToolButton::clicked, this, [this] { navigateHistory(-1); });
    connect(forward, &QToolButton::clicked, this, [this] { navigateHistory(1); });
    row->addWidget(back);
    row->addWidget(forward);
    row->addSpacing(6);

    // One box: it goes to an address or a name outright, and shows every
    // other match as it is typed. Two boxes asked the user to know which
    // kind of thing they were about to type.
    searchBox_ = new QLineEdit;
    searchBox_->setPlaceholderText(tr("Go to an address or name, or search"));
    searchBox_->setClearButtonEnabled(true);
    searchBox_->setMinimumWidth(420);
    searchBox_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    searchBox_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    // Searching runs a moment after typing stops, so every keystroke does not
    // walk the whole program.
    auto *typing = new QTimer(this);
    typing->setSingleShot(true);
    typing->setInterval(180);
    connect(typing, &QTimer::timeout, this, [this] { runSearch(searchBox_->text()); });
    connect(searchBox_, &QLineEdit::textChanged, this, [this, typing](const QString &text) {
        if (text.isEmpty()) {
            searchResults_->hideMatches();
            return;
        }
        typing->start();
    });
    connect(searchBox_, &QLineEdit::returnPressed, this, [this, typing] {
        typing->stop();
        goToTarget();
    });
    row->addWidget(searchBox_);
    searchResults_ = new SearchResults(searchBox_, this);
    connect(searchResults_, &SearchResults::chosen, this, [this](quint64 address) {
        searchBox_->clear();
        if (ProgramTab *tab = currentTab())
            tab->showAddress(address);
    });

    // Analysis acts on the whole program rather than on what is typed, so it
    // sits at the far end rather than beside the box.
    row->addStretch(1);
    // Analysis is the slow thing this tool does, so the button runs it and the
    // arrow beside it says how much of it to run.
    analyzeButton_ = new QToolButton;
    analyzeButton_->setObjectName(QStringLiteral("analyzeButton"));
    analyzeButton_->setText(tr("Analyze"));
    analyzeButton_->setPopupMode(QToolButton::MenuButtonPopup);
    analyzeButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    analyzeButton_->setMenu(buildAnalyzeMenu());
    analyzeAction_ = new QAction(tr("Analyze"), this);
    analyzeAction_->setToolTip(tr("Decompile what the settings beside this ask for"));
    connect(analyzeAction_, &QAction::triggered, this, &MainWindow::analyzeCurrent);
    analyzeButton_->setDefaultAction(analyzeAction_);
    row->addWidget(analyzeButton_);
    bar->addWidget(strip);
    // The strip is the toolbar's only item, so it has to be allowed to grow
    // with it or the stretch inside it has nothing to spend.
    strip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

QDockWidget *MainWindow::addPane(const QString &title, const QString &objectName, QWidget *body,
                                 Qt::DockWidgetArea area)
{
    auto *dock = new QDockWidget(title, this);
    dock->setObjectName(objectName);
    dock->setWidget(body);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable
                      | QDockWidget::DockWidgetFloatable);
    addDockWidget(area, dock);
    panes_.append(dock);
    viewMenu_->addAction(dock->toggleViewAction());
    return dock;
}

void MainWindow::buildDocks()
{
    // Left: the project tree over the function list. Two panes only, so
    // neither is squeezed into an elided tab.
    projectTree_ = new QTreeWidget;
    projectTree_->setHeaderHidden(true);
    projectTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(projectTree_, &QTreeWidget::customContextMenuRequested, this,
            &MainWindow::showProjectTreeMenu);
    connect(projectTree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        // A member program raises the tab it is already in, or opens it.
        const QVariant member = item->data(0, kProgramPathRole);
        if (member.isValid()) {
            openPath(member.toString());
            return;
        }
        const QVariant address = item->data(0, Qt::UserRole);
        if (!address.isValid())
            return;
        // Under a project the tree spans several programs, so the jump goes to
        // the tab the item sits under rather than whichever is in front.
        ProgramTab *tab = currentTab();
        for (QTreeWidgetItem *walk = item; walk != nullptr; walk = walk->parent()) {
            const QVariant owner = walk->data(0, kProgramPathRole);
            if (owner.isValid()) {
                if (ProgramTab *found = tabForPath(owner.toString())) {
                    programBar_->setCurrentIndex(programStack_->indexOf(found));
                    bindCurrentTab();
                    tab = found;
                }
                break;
            }
        }
        if (tab != nullptr)
            tab->showFunction(address.toULongLong());
    });
    QDockWidget *project = addPane(tr("Project"), QStringLiteral("projectPane"), projectTree_,
                                   Qt::LeftDockWidgetArea);
    functionsPane_ = new FunctionsPane;
    connect(functionsPane_, &FunctionsPane::functionActivated, this, [this](quint64 address) {
        if (ProgramTab *tab = currentTab())
            tab->showFunction(address);
    });
    connect(functionsPane_, &FunctionsPane::contextActionsWanted, this,
            [this](QMenu *menu, quint64 address) {
                fillContextMenu(menu, targetForAddress(address, ContextTarget::FunctionList));
            });
    QDockWidget *functions = addPane(tr("Functions"), QStringLiteral("functionsPane"),
                                     functionsPane_, Qt::LeftDockWidgetArea);
    splitDockWidget(project, functions, Qt::Vertical);

    // Right: the listing, secondary to the decompiler in the centre.
    listingView_ = new ListingView;
    listingView_->setPlaceholderText(tr("Disassembly of the current function"));
    connect(listingView_, &ListingView::navigateRequested, this, [this](quint64 address) {
        if (ProgramTab *tab = currentTab())
            tab->showFunction(address);
    });
    connect(listingView_, &CodeView::contextMenuAboutToShow, this,
            [this](QMenu *menu, const QString &word) {
                ContextTarget target = targetForWord(word, ContextTarget::Listing,
                                                     listingView_->textCursor().block().text());
                if (const auto at = listingView_->addressAtCursor()) {
                    target.line = *at;
                    target.hasLine = true;
                }
                fillContextMenu(menu, target);
            });
    listingPane_ = new ListingPane(listingView_);
    connect(listingPane_, &ListingPane::logMessage, this, &MainWindow::appendLog);
    connect(listingPane_, &ListingPane::patchApplied, this, [this] {
        if (ProgramTab *tab = currentTab())
            onPatchApplied(tab);
    });
    QDockWidget *listing = addPane(tr("Listing"), QStringLiteral("listingPane"), listingPane_,
                                   Qt::RightDockWidgetArea);
    listingDock_ = listing;

    // Bottom: tables and tools, as one tab group. The tab is the title, so
    // these panes draw no title bar of their own.
    referencesPane_ = new TablePane({tr("Direction"), tr("Function"), tr("Address")},
                                    tr("Filter references"));
    QDockWidget *xrefs = addPane(tr("Cross References"), QStringLiteral("xrefsPane"),
                                 referencesPane_, Qt::BottomDockWidgetArea);
    referencesDock_ = xrefs;
    connect(referencesPane_, &TablePane::addressActivated, this, [this](quint64 address) {
        if (ProgramTab *tab = currentTab())
            tab->showAddress(address);
    });
    symbolsPane_ = new TablePane({tr("Name"), tr("Address"), tr("Size"), tr("Kind")}, tr("Filter symbols"));
    stringsPane_ = new TablePane({tr("Address"), tr("Length"), tr("Segment"), tr("Text")}, tr("Filter strings"));
    segmentsPane_ = new TablePane({tr("Name"), tr("Start"), tr("End"), tr("Size"), tr("Flags")}, tr("Filter segments"));
    importsPane_ = new TablePane({tr("Name"), tr("Address")}, tr("Filter imports"));
    for (TablePane *pane : {symbolsPane_, stringsPane_, segmentsPane_, importsPane_})
        connect(pane, &TablePane::addressActivated, this, [this](quint64 address) {
            if (ProgramTab *tab = currentTab())
                tab->showAddress(address);
        });
    // Every table offers the same actions on the row it is showing, because
    // they all go through the one builder.
    for (TablePane *pane : {referencesPane_, symbolsPane_, stringsPane_, segmentsPane_, importsPane_})
        connect(pane, &TablePane::contextActionsWanted, this, [this](QMenu *menu, quint64 address) {
            fillContextMenu(menu, targetForAddress(address, ContextTarget::Table));
        });
    debuggerPane_ = new DebuggerPane;
    connect(debuggerPane_, &DebuggerPane::logMessage, this, &MainWindow::appendLog);
    connect(debuggerPane_, &DebuggerPane::breakpointsChanged, this, &MainWindow::refreshBreakpointMarks);
    connect(debuggerPane_, &DebuggerPane::debuggingChanged, this, &MainWindow::setDebugging);
    connect(debuggerPane_, &DebuggerPane::locationChanged, this, [this](quint64 address) {
        // Follow the program: show where it stopped, mark the instruction,
        // and bring it into view rather than leaving it scrolled away.
        if (ProgramTab *tab = currentTab())
            tab->showAddress(address);
        refreshBreakpointMarks();
        if (listingView_ != nullptr)
            listingView_->scrollToAddress(address);
    });
    // While a program is running, its registers, its stack and what it wrote
    // are the window, so each gets a dock of its own beside the code.
    registersDock_ = addPane(tr("Registers"), QStringLiteral("registersPane"),
                             debuggerPane_->registersView(), Qt::RightDockWidgetArea);
    stackDock_ = addPane(tr("Call Stack"), QStringLiteral("stackPane"),
                         debuggerPane_->stackView(), Qt::RightDockWidgetArea);
    outputDock_ = addPane(tr("Program Output"), QStringLiteral("programOutputPane"),
                          debuggerPane_->outputView(), Qt::BottomDockWidgetArea);
    for (QDockWidget *pane : {registersDock_, stackDock_, outputDock_})
        pane->hide();
    // The transport gets a bar of its own rather than crowding the one that
    // is always there. It appears with the debugger and goes with it.
    debugBar_ = addToolBar(tr("Debugger"));
    debugBar_->setObjectName(QStringLiteral("debugBar"));
    debugBar_->setMovable(false);
    debugBar_->addWidget(debuggerPane_->controls());
    debugBar_->hide();

    // A click in the listing's gutter is how a breakpoint is set.
    connect(listingView_, &CodeView::gutterClicked, this, [this](int line) {
        if (const auto address = listingView_->addressAtLine(line))
            debuggerPane_->toggleBreakpoint(*address);
    });

    QList<QDockWidget *> bottom = {
        xrefs,
        addPane(tr("Symbols"), QStringLiteral("symbolsPane"), symbolsPane_, Qt::BottomDockWidgetArea),
        addPane(tr("Strings"), QStringLiteral("stringsPane"), stringsPane_, Qt::BottomDockWidgetArea),
        addPane(tr("Segments"), QStringLiteral("segmentsPane"), segmentsPane_, Qt::BottomDockWidgetArea),
        addPane(tr("Imports"), QStringLiteral("importsPane"), importsPane_, Qt::BottomDockWidgetArea),
        addPane(tr("Console"), QStringLiteral("consolePane"), placeholderText(tr("Astral console")),
                Qt::BottomDockWidgetArea),
        addPane(tr("Log"), QStringLiteral("logPane"), logView_ = qobject_cast<QPlainTextEdit *>(placeholderText(tr("Analysis log"))),
                Qt::BottomDockWidgetArea),
    };
    for (int i = 1; i < bottom.size(); ++i)
        tabifyDockWidget(bottom[i - 1], bottom[i]);
    for (QDockWidget *pane : bottom) {
        // Tabbed, the tab is the title. Floating, the pane needs its real
        // title bar back or there is nothing to drag it home by.
        pane->setTitleBarWidget(new QWidget(pane));
        connect(pane, &QDockWidget::topLevelChanged, pane, [pane](bool floating) {
            if (floating) {
                delete pane->titleBarWidget();
                pane->setTitleBarWidget(nullptr);
            } else {
                pane->setTitleBarWidget(new QWidget(pane));
            }
        });
    }
    xrefs->raise();

    resizeDocks({project}, {200}, Qt::Vertical);
    resizeDocks({project, listing}, {300, 560}, Qt::Horizontal);
    resizeDocks({xrefs}, {220}, Qt::Vertical);
}

void MainWindow::buildStatusBar()
{
    statusArch_ = new QLabel;
    statusArch_->setObjectName(QStringLiteral("muted"));
    statusAddress_ = new QLabel;
    statusAddress_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    statusAnalysis_ = new QLabel(tr("idle"));
    statusAnalysis_->setObjectName(QStringLiteral("muted"));
    statusBar()->addWidget(statusArch_);
    statusBar()->addWidget(statusAddress_, 1);
    statusBar()->addPermanentWidget(statusAnalysis_);
    statusBar()->showMessage(tr("Ready"), 2000);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (project_->isOpen() && project_->dirty()) {
        const auto answer = QMessageBox::question(
            this, tr("Save Project"),
            tr("%1 has unsaved work. Save it before closing?").arg(project_->displayName()),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
        if (answer == QMessageBox::Save && !saveProject(false)) {
            event->ignore();
            return;
        }
    }
    saveLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (searchResults_ != nullptr && searchResults_->isVisible())
        searchResults_->hideMatches();
    platform::applyWindowShape(this, isMaximized() || isFullScreen() ? 0 : platform::kCornerRadius);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange)
        platform::applyWindowShape(this, isMaximized() || isFullScreen() ? 0 : platform::kCornerRadius);
}

void MainWindow::paintEvent(QPaintEvent *event)
{
    // The window is translucent so the corners can be clipped; everything
    // inside the shape is painted opaque here, in the theme's base colour.
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int radius = isMaximized() || isFullScreen() ? 0 : platform::kCornerRadius;
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette().color(QPalette::Window));
    painter.drawRoundedRect(rect(), radius, radius);
    QMainWindow::paintEvent(event);
}

QString MainWindow::layoutSignature() const
{
    // A saved layout only describes the panes that existed when it was
    // written. Adding one leaves it stranded in whatever corner Qt picks, so
    // the set of panes is recorded with the layout and a mismatch discards it.
    QStringList names;
    for (const QDockWidget *pane : panes_)
        names << pane->objectName();
    names.sort();
    return names.join(QLatin1Char(','));
}

void MainWindow::restoreLayout()
{
    QSettings settings;
    if (settings.value(QStringLiteral("window/panes")).toString() != layoutSignature()) {
        settings.remove(QStringLiteral("window/state"));
        settings.remove(QStringLiteral("window/defaultState"));
        settings.setValue(QStringLiteral("window/panes"), layoutSignature());
    }
    if (settings.contains(QStringLiteral("window/geometry")))
        restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    if (settings.contains(QStringLiteral("window/state")))
        restoreState(settings.value(QStringLiteral("window/state")).toByteArray());
}

void MainWindow::saveLayout()
{
    // Only the workspace layout is worth keeping; the welcome screen hides
    // every pane and would overwrite it with nothing.
    if (stack_->currentWidget() != workspace_)
        return;
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    settings.setValue(QStringLiteral("window/panes"), layoutSignature());
}

void MainWindow::resetLayout()
{
    // The layout the user saved as default, else the built-in one.
    const QByteArray saved = QSettings().value(QStringLiteral("window/defaultState")).toByteArray();
    restoreState(saved.isEmpty() ? builtinState_ : saved);
    if (saved.isEmpty())
        for (QDockWidget *pane : panes_)
            pane->show();
    // On the welcome screen the panes stay out of sight whatever the layout says.
    if (stack_->currentWidget() != workspace_)
        for (QDockWidget *pane : panes_)
            pane->hide();
}

} // namespace astral::gui
