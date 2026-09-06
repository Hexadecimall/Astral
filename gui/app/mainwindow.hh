// The one top-level window. It owns the dock layout, the menus and the
// actions; what each pane shows is the business of the pane's own widget.
#ifndef ASTRAL_GUI_MAINWINDOW_HH
#define ASTRAL_GUI_MAINWINDOW_HH

#include "model/programdocument.hh"

#include <QMainWindow>

#include <functional>

#include <QHash>
#include <vector>

class QDockWidget;
class QLabel;
class QComboBox;
class QTabWidget;
class QTabBar;
class QTreeWidget;
class QTreeWidgetItem;
class QPlainTextEdit;
class QLineEdit;
class QAction;
class QToolButton;
class QToolBar;
class QStackedWidget;
class QMenu;

namespace astral::gui {

class FunctionsPane;
class CodeView;
class ListingView;
class DebuggerPane;
class ListingPane;
class TablePane;
class SearchResults;
class ProgramTab;
class TerminalView;
class ProgramDocument;
class ProjectController;
class TitleBar;
class WelcomePage;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    void openPath(const QString &path);
    // Selects a view tab by name: code, pseudo, graph, hex.
    void selectView(const QString &name);
    void typeInSearch(const QString &text);
    void pressEnterInSearch();
    void dumpListing();
    void runAssembleHook(const QString &edit, const QString &outPath);
    // Developer hooks used by the automated run: edit, patch, write, quit.
    void runEditHook(const QString &sourceFile, const QString &outPath);
    void runDebugHook(const QString &target, const QString &arguments);
    void runAnalyzeHook();
    void runExportHook(const QString &outPath);
    // Prints what the context menu offers for a word. A popup is not part of
    // the window, so a picture of the window cannot show it.
    void runMenuHook(const QString &word);
    // Renames from a script, through the same code the menu item runs, and
    // reports what every view would then show.
    void runRenameHook();
    // Opens the decompiler settings and writes out its groups, controls,
    // values and defaults. A modal dialog is not part of the window, so a
    // picture of the window cannot show one. `spec` is an optional list of
    // `name=value` settings to put in and apply, and `program:` at its front
    // asks for the open program's own values rather than the shared ones.
    void runOptionsHook(const QString &spec, bool quitAfter = true);

    // Tools > Decompiler Settings.
    void showDecompilerSettings();

    // What a context menu is about: the thing under the cursor, and where the
    // click came from. Whatever of this is known is what decides which actions
    // the menu offers, so a menu never carries an item that cannot act.
    struct ContextTarget {
        enum Origin { Source, Listing, Table, FunctionList, ProjectTree };
        Origin origin = Source;
        // The identifier under the cursor, empty when there was none.
        QString word;
        // What that word stands for in the program.
        quint64 address = 0;
        bool hasAddress = false;
        bool isFunction = false;
        // The word names a value inside `owner` rather than anything global.
        bool isLocal = false;
        quint64 owner = 0;
        // The address of the listing line clicked, when there was one.
        quint64 line = 0;
        bool hasLine = false;
        // The whole line the cursor sat in, for copying.
        QString lineText;
    };
    // Fills a menu with everything that applies to `target`. Every menu in the
    // window comes through here, so what right-clicking a name offers cannot
    // drift from what right-clicking the same name in a table offers.
    void fillContextMenu(QMenu *menu, const ContextTarget &target);
    ContextTarget targetForWord(const QString &word, ContextTarget::Origin origin,
                                const QString &lineText = QString()) const;
    ContextTarget targetForAddress(quint64 address, ContextTarget::Origin origin) const;

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void buildMenus();
    void buildToolBar();
    void buildDocks();
    void buildStatusBar();
    QDockWidget *addPane(const QString &title, const QString &objectName, QWidget *body,
                         Qt::DockWidgetArea area);
    QString layoutSignature() const;
    void restoreLayout();
    void saveLayout();
    void resetLayout();
    void showWelcome();
    void showWorkspace();
    void newProject();
    // Opens a project directory: every member program becomes a tab, with the
    // state the project stored for it applied.
    void openProject(const QString &directory);
    void addProgramToProject();
    void removeProgramFromProject(const QString &binaryPath);
    // Writes every open member's state. Quiet when no project is open.
    bool saveProject(bool announce);
    // Applies the project's stored state to a tab that has just opened.
    void applyProjectState(ProgramTab *tab);
    ProgramTab *tabForPath(const QString &path) const;
    void showProjectTreeMenu(const QPoint &at);
    void bindCurrentTab();
    // The source tab drops down to the readings of the same program. Which
    // one is showing is the tab's label, so the bar says what you are looking
    // at without a second row of tabs saying it again.
    void showSourceMenu();
    // Starts the shell the terminal pane shows, the first time it is seen.
    void startTerminal();
public:
    // Raises the terminal and types a command into it, for a scripted run.
    void runTerminalHook(const QString &command);
    // Leaves debugging, for a scripted run that wants to check the window is
    // put back the way it was.
    void leaveDebuggingHook() { setDebugging(false); }
private:
    // The dock on the right shows the disassembly, unless the centre already
    // is, in which case it shows Nova instead of the same thing twice.
    void refreshListingDock();
    ProgramTab *currentTab() const;
    void goToTarget();
    void closeProgram(int index);
    void fillProjectTree();
    // The entry points, segments, functions and imports of one program, under
    // the item that stands for it.
    void fillProgramNode(QTreeWidgetItem *root, ProgramDocument *document);
    void fillTables();
    void refreshBreakpointMarks();
    // Debugging takes the window over: the panes that matter while a program
    // is running come forward, and the layout goes back when it stops.
    void setDebugging(bool debugging);
    void analyzeCurrent();
    // The Analysis settings, as a menu, so they can be changed where they are used.
    QMenu *buildAnalyzeMenu();
    // Everything about how the program is run, hung off the Debug button. Built
    // fresh each time it is opened, so it shows what is set now.
    QMenu *buildDebugMenu();
    void showDebugSettings();
    // The one button in the right corner: Analyze while reading a program,
    // Debug while running one.
    void updateCornerButton();
    void appendLog(const QString &line);
    void savePatched();
    void onPatchApplied(ProgramTab *tab);
    void writePatchedWithBackup(ProgramTab *tab, const QString &out);
    QByteArray builtinState_;
    void applyPatch(const std::function<bool(QString &)> &patch, const QString &what);
    // Renames the function under the cursor and re-reads everything that
    // printed the old name.
    void renameCurrent();
    // Asks for a name for whatever `target` stands for, then applies it.
    void renameTarget(const ContextTarget &target);
    // Renames without asking, and re-reads every view that printed the old
    // name. The dialog and the scripted run share this.
    bool applyRename(const ContextTarget &target, const QString &name, bool learn);
    // Every pane that could be showing a name reads it again.
    void refreshAfterEdit(quint64 address);
    // An edit was made that only a project can keep. Marks the project dirty,
    // or says once that there is nowhere for it to go.
    void noteProjectEdit();
    // Makes a project around the program already open and puts it in.
    void createProjectForCurrent();
    void setCommentAt(quint64 address, const QString &kind);
    void toggleBookmarkAt(quint64 address);
    void forceReanalyse(quint64 address);
    void showInListing(quint64 address);
    void showNamingReason(quint64 address);
    void copyToClipboard(const QString &text, const QString &what);
    // The name to call an address in a menu item.
    QString labelForAddress(quint64 address) const;
    void goToDefinition();
    void offerAnalysis(ProgramTab *tab);
    void showAnalysisRundown(ProgramTab *tab, int done, int failed, int discovered, qint64 ms);
    // Fills the references pane with what calls the current function and what
    // it calls, so a double click walks the call graph either way.
    void showReferences();
    // `incoming` and `outgoing` say which half of the call graph is wanted, so
    // asking what calls a function and what it calls are different answers.
    void showReferencesFor(quint64 address, bool incoming, bool outgoing);
    void updateReferences(quint64 address, bool incoming = true, bool outgoing = true);
    // Searches functions, symbols and strings for a piece of text.
    void findInProgram();
    void runSearch(const QString &needle);
    // Writes the whole program, or one function, as compilable C.
    void exportSource(bool wholeProgram);
    // Records every recovered name against a fingerprint of its body.
    void learnNames();
    // What Astral knows about the current function: why it chose the name,
    // what it recovered, and what it warned about.
    void showFunctionFacts(quint64 address = 0);
    void navigateHistory(int delta);
    void rememberLocation(quint64 address);

    TablePane *referencesPane_ = nullptr;
    QLineEdit *searchBox_ = nullptr;
    SearchResults *searchResults_ = nullptr;
    QDockWidget *referencesDock_ = nullptr;
    std::vector<quint64> history_;
    int historyAt_ = -1;
    // An edit with no project to keep it is worth saying once, not every time.
    bool warnedNoProject_ = false;
    bool navigatingHistory_ = false;

    TitleBar *titleBar_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    WelcomePage *welcome_ = nullptr;
    QWidget *workspace_ = nullptr;
    QTabBar *programBar_ = nullptr;
    // The terminal in the bottom row of panes, and the shell on the far end
    // of it, which is only started once someone looks at it.
    TerminalView *terminal_ = nullptr;
    QTabBar *viewBar_ = nullptr;
    // Set while the bar is being put in step with the tab, so echoing back a
    // selection does not look like the user choosing it.
    bool updatingViewBar_ = false;
    QStackedWidget *programStack_ = nullptr;
    QList<QDockWidget *> panes_;
    FunctionsPane *functionsPane_ = nullptr;
    ListingView *listingView_ = nullptr;
    DebuggerPane *debuggerPane_ = nullptr;
    QDockWidget *registersDock_ = nullptr;
    QDockWidget *stackDock_ = nullptr;
    QDockWidget *outputDock_ = nullptr;
    QDockWidget *memoryDock_ = nullptr;
    QDockWidget *breakpointsDock_ = nullptr;
    QByteArray beforeDebugging_;
    bool debugging_ = false;
    ListingPane *listingPane_ = nullptr;
    QTreeWidget *projectTree_ = nullptr;
    ProjectController *project_ = nullptr;
    QAction *addProgramAction_ = nullptr;
    TablePane *symbolsPane_ = nullptr;
    TablePane *stringsPane_ = nullptr;
    TablePane *segmentsPane_ = nullptr;
    TablePane *importsPane_ = nullptr;
    QPlainTextEdit *logView_ = nullptr;
    QAction *analyzeAction_ = nullptr;
    QToolButton *analyzeButton_ = nullptr;
    QAction *savePatchedAction_ = nullptr;
    QDockWidget *listingDock_ = nullptr;
    QToolBar *navigationBar_ = nullptr;
    // The transport, on a bar of its own that is only there while debugging.
    QToolBar *debugBar_ = nullptr;
    QAction *debuggerAction_ = nullptr;
    // The panes debugging put away, so leaving it puts back exactly those and
    // nothing else. Asking the saved layout to do it was not enough: what
    // comes back has to be what was there, not what a restore decides.
    QList<QDockWidget *> hiddenForDebugging_;
    QLabel *statusArch_ = nullptr;
    QLabel *statusAddress_ = nullptr;
    QLabel *statusAnalysis_ = nullptr;
    QMenu *viewMenu_ = nullptr;
    QByteArray defaultState_;
    QByteArray defaultGeometry_;
};

} // namespace astral::gui

#endif
