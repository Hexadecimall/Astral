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
class QPlainTextEdit;
class QAction;
class QToolBar;
class QStackedWidget;
class QMenu;

namespace astral::gui {

class FunctionsPane;
class CodeView;
class ListingView;
class TablePane;
class ProgramTab;
class TitleBar;
class WelcomePage;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    void openPath(const QString &path);
    // Selects a view tab by name: code, pseudo, graph, hex.
    void selectView(const QString &name);
    // Developer hooks used by the automated run: edit, patch, write, quit.
    void runEditHook(const QString &sourceFile, const QString &outPath);

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
    void restoreLayout();
    void saveLayout();
    void resetLayout();
    void showWelcome();
    void showWorkspace();
    void newProject();
    void bindCurrentTab();
    ProgramTab *currentTab() const;
    void goToTarget();
    void closeProgram(int index);
    void fillProjectTree();
    void fillTables();
    void analyzeCurrent();
    void appendLog(const QString &line);
    void savePatched();
    bool eventFilter(QObject *watched, QEvent *event) override;
    QHash<int, QAction *> plainKeys_;
    QByteArray builtinState_;
    void applyPatch(const std::function<bool(QString &)> &patch, const QString &what);
    // Renames the function under the cursor and re-reads everything that
    // printed the old name.
    void renameCurrent();
    // Fills the references pane with what calls the current function and what
    // it calls, so a double click walks the call graph either way.
    void showReferences();
    void updateReferences(quint64 address);
    // Searches functions, symbols and strings for a piece of text.
    void findInProgram();
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
    TablePane *searchPane_ = nullptr;
    QDockWidget *referencesDock_ = nullptr;
    QDockWidget *searchDock_ = nullptr;
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
    QTreeWidget *projectTree_ = nullptr;
    TablePane *symbolsPane_ = nullptr;
    TablePane *stringsPane_ = nullptr;
    TablePane *segmentsPane_ = nullptr;
    TablePane *importsPane_ = nullptr;
    QPlainTextEdit *logView_ = nullptr;
    QAction *analyzeAction_ = nullptr;
    QAction *savePatchedAction_ = nullptr;
    QDockWidget *listingDock_ = nullptr;
    QToolBar *navigationBar_ = nullptr;
    QComboBox *addressBox_ = nullptr;
    QLabel *statusArch_ = nullptr;
    QLabel *statusAddress_ = nullptr;
    QLabel *statusAnalysis_ = nullptr;
    QMenu *viewMenu_ = nullptr;
    QByteArray defaultState_;
    QByteArray defaultGeometry_;
};

} // namespace astral::gui

#endif
