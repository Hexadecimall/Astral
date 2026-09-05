// The one top-level window. It owns the dock layout, the menus and the
// actions; what each pane shows is the business of the pane's own widget.
#ifndef ASTRAL_GUI_MAINWINDOW_HH
#define ASTRAL_GUI_MAINWINDOW_HH

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
class QToolBar;
class QStackedWidget;
class QMenu;

namespace astral::gui {

class FunctionsPane;
class CodeView;
class ListingView;
class ListingPane;
class TablePane;
class SearchResults;
class ProgramTab;
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
    void dumpListing();
    void runAssembleHook(const QString &edit, const QString &outPath);
    // Developer hooks used by the automated run: edit, patch, write, quit.
    void runAnalyzeHook();
    void runExportHook(const QString &outPath);

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
    ProgramTab *currentTab() const;
    void goToTarget();
    void closeProgram(int index);
    void fillProjectTree();
    // The entry points, segments, functions and imports of one program, under
    // the item that stands for it.
    void fillProgramNode(QTreeWidgetItem *root, ProgramDocument *document);
    void fillTables();
    void analyzeCurrent();
    void appendLog(const QString &line);
    void savePatched();
    void onPatchApplied(ProgramTab *tab);
    void writePatchedWithBackup(ProgramTab *tab, const QString &out);
    QByteArray builtinState_;
    void applyPatch(const std::function<bool(QString &)> &patch, const QString &what);
    // Renames the function under the cursor and re-reads everything that
    // printed the old name.
    void renameCurrent();
    void goToDefinition();
    void fillContextMenu(QMenu *menu, const QString &word);
    void offerAnalysis(ProgramTab *tab);
    void showAnalysisRundown(ProgramTab *tab, int done, int failed, int discovered, qint64 ms);
    // Fills the references pane with what calls the current function and what
    // it calls, so a double click walks the call graph either way.
    void showReferences();
    void updateReferences(quint64 address);
    // Searches functions, symbols and strings for a piece of text.
    void findInProgram();
    void runSearch(const QString &needle);
    // Writes the whole program, or one function, as compilable C.
    void exportSource(bool wholeProgram);
    // Records every recovered name against a fingerprint of its body.
    void learnNames();
    // What Astral knows about the current function: why it chose the name,
    // what it recovered, and what it warned about.
    void showFunctionFacts();
    void navigateHistory(int delta);
    void rememberLocation(quint64 address);

    TablePane *referencesPane_ = nullptr;
    QLineEdit *searchBox_ = nullptr;
    SearchResults *searchResults_ = nullptr;
    QDockWidget *referencesDock_ = nullptr;
    std::vector<quint64> history_;
    int historyAt_ = -1;
    bool navigatingHistory_ = false;

    TitleBar *titleBar_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    WelcomePage *welcome_ = nullptr;
    QWidget *workspace_ = nullptr;
    QTabBar *programBar_ = nullptr;
    QTabBar *viewBar_ = nullptr;
    QStackedWidget *programStack_ = nullptr;
    QList<QDockWidget *> panes_;
    FunctionsPane *functionsPane_ = nullptr;
    ListingView *listingView_ = nullptr;
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
    QAction *savePatchedAction_ = nullptr;
    QDockWidget *listingDock_ = nullptr;
    QToolBar *navigationBar_ = nullptr;
    QLabel *statusArch_ = nullptr;
    QLabel *statusAddress_ = nullptr;
    QLabel *statusAnalysis_ = nullptr;
    QMenu *viewMenu_ = nullptr;
    QByteArray defaultState_;
    QByteArray defaultGeometry_;
};

} // namespace astral::gui

#endif
