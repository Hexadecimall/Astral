#include "app/mainwindow.hh"
#include "app/titlebar.hh"
#include "app/functionspane.hh"
#include "model/functionlistmodel.hh"
#include "app/programtab.hh"
#include "app/tablepane.hh"
#include "app/welcomepage.hh"
#include "views/codeview.hh"
#include "views/listingview.hh"

#include <QDateTime>

#include <cstdio>
#include <QFile>
#include <QTimer>
#include <QInputDialog>
#include <QTreeWidgetItem>
#include <QKeyEvent>
#include <QTextEdit>
#include "platform/window.hh"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
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
#include <QPainter>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStackedWidget>
#include <QLineEdit>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace astral::gui {
namespace {

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
    headerLayout->addWidget(programBar_);
    headerLayout->addWidget(divider);
    headerLayout->addWidget(viewBar_);
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
    qApp->installEventFilter(this);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress || !isActiveWindow())
        return QMainWindow::eventFilter(watched, event);
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier))
        return false;
    QAction *action = plainKeys_.value(key->key());
    if (!action || !action->isEnabled())
        return false;
    // An editor owns its keys; everything else lets the letter act.
    QWidget *focus = QApplication::focusWidget();
    if (auto *edit = qobject_cast<QPlainTextEdit *>(focus); edit && !edit->isReadOnly())
        return false;
    if (qobject_cast<QLineEdit *>(focus) || qobject_cast<QTextEdit *>(focus) || qobject_cast<QComboBox *>(focus))
        return false;
    if (stack_->currentWidget() != workspace_)
        return false;
    action->trigger();
    return true;
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
    if (path.endsWith(QStringLiteral(".astralproj"))) {
        QMessageBox::information(this, tr("Astral"), tr("Projects arrive in a later step; open a binary for now."));
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
        auto *tab = new ProgramTab(std::move(document));
        connect(tab, &ProgramTab::logMessage, this, &MainWindow::appendLog);
        connect(tab, &ProgramTab::viewChanged, this, [this, tab](int index) {
            if (tab == currentTab() && viewBar_->currentIndex() != index)
                viewBar_->setCurrentIndex(index);
        });
        ProgramDocument *doc = tab->document();
        connect(doc, &ProgramDocument::analysisProgress, this, [this, tab](int done, int total, const QString &name) {
            if (tab == currentTab())
                statusAnalysis_->setText(tr("analysis: %1/%2 %3").arg(done).arg(total).arg(name));
        });
        connect(doc, &ProgramDocument::analysisFinished, this, [this, tab](int done, int failed, qint64 ms) {
            appendLog(tr("analysis of %1: %2 functions in %3 ms, %4 failed")
                          .arg(QFileInfo(tab->document()->path()).fileName()).arg(done).arg(ms).arg(failed));
            if (tab == currentTab()) {
                statusAnalysis_->setText(tr("idle"));
                analyzeAction_->setEnabled(true);
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
            if (tab == currentTab())
                listingView_->setPlainText(listing);
        });
        programStack_->addWidget(tab);
        const int index = programBar_->addTab(QFileInfo(path).fileName());
        programBar_->setTabToolTip(index, path);
        programBar_->setCurrentIndex(index);
        showWorkspace();
        bindCurrentTab();
        tab->showFunction(tab->document()->entryPoint());
        statusBar()->clearMessage();
    });
}

void MainWindow::runEditHook(const QString &sourceFile, const QString &outPath)
{
    ProgramTab *tab = currentTab();
    if (!tab || !tab->document()->cached(tab->currentAddress())) {
        std::fprintf(stderr, "edit hook: waiting (tab=%p)\n", static_cast<void *>(tab));
        // Opening and the first decompile are asynchronous; try again shortly.
        QTimer::singleShot(250, this, [this, sourceFile, outPath] { runEditHook(sourceFile, outPath); });
        return;
    }
    QFile file(sourceFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    tab->replaceCodeText(QString::fromUtf8(file.readAll()));
    std::fprintf(stderr, "edit hook: text replaced, patching\n");
    connect(tab->document(), &ProgramDocument::patchesChanged, this, [this, tab, outPath] {
        QString error;
        if (!tab->document()->writePatched(outPath, error))
            appendLog(error);
        QTimer::singleShot(200, qApp, &QCoreApplication::quit);
    });
    tab->compileCurrent();
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
        listingView_->setPlainText(QString());
        statusArch_->clear();
        statusAddress_->clear();
        return;
    }
    ProgramDocument *document = tab->document();
    functionsPane_->setSourceModel(tab->functionModel());
    listingView_->setPlainText(tab->listing());
    statusArch_->setText(QStringLiteral("%1 · %2 · %3 functions")
                             .arg(document->languageId(), document->formatName())
                             .arg(document->functions().size()));
    setWindowTitle(QStringLiteral("%1 - Astral").arg(QFileInfo(document->path()).fileName()));
    if (tab->currentAddress() != 0)
        functionsPane_->selectAddress(tab->currentAddress());
    savePatchedAction_->setEnabled(document->patchCount() > 0);
    analyzeAction_->setEnabled(!document->analyzing());
    statusAnalysis_->setText(document->analyzing() ? tr("analysis running") : tr("idle"));
    fillProjectTree();
    fillTables();
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
    ProgramTab *tab = currentTab();
    if (!tab)
        return;
    ProgramDocument *document = tab->document();
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    auto *root = new QTreeWidgetItem(projectTree_, {QFileInfo(document->path()).fileName()});
    root->setToolTip(0, document->path());
    root->setExpanded(true);

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
}

void MainWindow::analyzeCurrent()
{
    ProgramTab *tab = currentTab();
    if (!tab)
        return;
    analyzeAction_->setEnabled(false);
    appendLog(tr("analysis of %1 started").arg(QFileInfo(tab->document()->path()).fileName()));
    tab->document()->analyzeAll();
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
    const QString original = tab->document()->path();
    const QString suggested = original + QStringLiteral(".patched");
    const QString out = QFileDialog::getSaveFileName(this, tr("Save Patched Binary"), suggested);
    if (out.isEmpty())
        return;
    QString error;
    if (tab->document()->writePatched(out, error))
        appendLog(tr("wrote patched binary to %1").arg(out));
    else
        QMessageBox::warning(this, tr("Save Patched Binary"), error);
}

void MainWindow::goToTarget()
{
    ProgramTab *tab = currentTab();
    if (!tab)
        return;
    const QString target = addressBox_->currentText();
    if (tab->navigateTo(target))
        addressBox_->lineEdit()->clear();
    else
        statusBar()->showMessage(tr("No function or address matches %1").arg(target), 3000);
}

void MainWindow::newProject()
{
    QString dir = QFileDialog::getSaveFileName(this, tr("New Project"), QString(),
                                               tr("Astral Project (*.astralproj)"));
    if (dir.isEmpty())
        return;
    if (!dir.endsWith(QStringLiteral(".astralproj")))
        dir += QStringLiteral(".astralproj");
    openPath(dir);
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

void MainWindow::renameCurrent()
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    const quint64 address = tab->currentAddress();
    ProgramDocument *doc = tab->document();
    const auto entry = doc->functionAt(address);
    const QString current = entry ? entry->name : QString();
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Rename Function"),
                                               tr("Name for the function at 0x%1:")
                                                   .arg(address, 0, 16),
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
    QString error;
    if (!doc->rename(address, name.trimmed(), learn, error)) {
        QMessageBox::warning(this, tr("Rename Failed"), error);
        return;
    }
    appendLog(tr("renamed 0x%1 to %2%3").arg(address, 0, 16).arg(name.trimmed(),
                                                                 learn ? tr(" (remembered)")
                                                                       : QString()));
    tab->functionModel()->setFunctions(doc->functions());
    fillTables();
    tab->showFunction(address);
}

void MainWindow::updateReferences(quint64 address)
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
    for (const Reference &ref : doc->callersOf(address)) {
        rows.push_back({{tr("called by"), ref.fromName,
                         QStringLiteral("0x%1").arg(ref.from, 0, 16)},
                        ref.from});
    }
    if (const auto function = doc->cached(address)) {
        for (const CallSite &call : function->callees) {
            rows.push_back({{tr("calls"), call.name,
                             QStringLiteral("0x%1").arg(call.address, 0, 16)},
                            call.address});
        }
    }
    referencesPane_->setRows(rows, {2});
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
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    bool accepted = false;
    const QString needle = QInputDialog::getText(this, tr("Find"),
                                                 tr("Function, symbol or string containing:"),
                                                 QLineEdit::Normal, QString(), &accepted);
    if (!accepted || needle.isEmpty())
        return;
    ProgramDocument *doc = tab->document();
    std::vector<TablePane::Row> rows;
    for (const FunctionEntry &fn : doc->functions())
        if (fn.name.contains(needle, Qt::CaseInsensitive))
            rows.push_back({{tr("function"), fn.name,
                             QStringLiteral("0x%1").arg(fn.address, 0, 16)}, fn.address});
    for (const SymbolEntry &sym : doc->symbols())
        if (!sym.isFunction && sym.name.contains(needle, Qt::CaseInsensitive))
            rows.push_back({{tr("symbol"), sym.name,
                             QStringLiteral("0x%1").arg(sym.address, 0, 16)}, sym.address});
    for (const StringEntry &str : doc->strings())
        if (str.text.contains(needle, Qt::CaseInsensitive))
            rows.push_back({{tr("string"), str.text,
                             QStringLiteral("0x%1").arg(str.address, 0, 16)}, str.address});
    searchPane_->setRows(rows, {2});
    if (searchDock_ != nullptr) {
        searchDock_->show();
        searchDock_->raise();
    }
    statusBar()->showMessage(tr("%n match(es) for \"%1\"", nullptr, static_cast<int>(rows.size()))
                                 .arg(needle), 4000);
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

void MainWindow::showFunctionFacts()
{
    ProgramTab *tab = currentTab();
    if (tab == nullptr)
        return;
    const auto function = tab->document()->cached(tab->currentAddress());
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
    QMessageBox::information(this, tr("What Astral Knows"), lines.join(QStringLiteral("\n")));
}

void MainWindow::buildMenus()
{
    // Single letters act only when focus is not inside an editor; see
    // eventFilter. The menu shows the key without owning it.
    auto plainKey = [this](QMenu *menu, const QString &text, int key, const QString &label, auto slot) {
        QAction *action = menu->addAction(QStringLiteral("%1\t%2").arg(text, label), this, slot);
        plainKeys_.insert(key, action);
        return action;
    };
    QMenu *file = titleBar_->menuBar()->addMenu(tr("&File"));
    file->addAction(tr("New Project..."), QKeySequence::New, this, &MainWindow::newProject);
    file->addAction(tr("Open..."), QKeySequence::Open, this, [this] {
        QString path = QFileDialog::getOpenFileName(this, tr("Open Binary or Project"));
        if (!path.isEmpty())
            openPath(path);
    });
    file->addSeparator();
    file->addAction(tr("Save Project"), QKeySequence::Save, this, [] {});
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
    plainKey(edit, tr("Rename..."), Qt::Key_N, QStringLiteral("N"), &MainWindow::renameCurrent);
    plainKey(edit, tr("Change Type"), Qt::Key_Y, QStringLiteral("Y"), [] {});
    plainKey(edit, tr("Comment"), Qt::Key_Semicolon, QStringLiteral(";"), [] {});
    edit->addSeparator();
    edit->addAction(tr("Find..."), QKeySequence::Find, this, &MainWindow::findInProgram);

    viewMenu_ = titleBar_->menuBar()->addMenu(tr("&View"));

    QMenu *navigate = titleBar_->menuBar()->addMenu(tr("&Navigate"));
    plainKey(navigate, tr("Back"), Qt::Key_Escape, QStringLiteral("Esc"), [this] { navigateHistory(-1); });
    navigate->addAction(tr("Forward"), QKeySequence(Qt::CTRL | Qt::Key_Right), this,
                        [this] { navigateHistory(1); });
    navigate->addSeparator();
    plainKey(navigate, tr("Go to Address..."), Qt::Key_G, QStringLiteral("G"), [this] {
        addressBox_->setFocus();
        addressBox_->lineEdit()->selectAll();
    });
    plainKey(navigate, tr("Cross References"), Qt::Key_X, QStringLiteral("X"), &MainWindow::showReferences);
    plainKey(navigate, tr("Toggle Listing / Graph"), Qt::Key_Space, QStringLiteral("Space"), [] {});

    QMenu *analysis = titleBar_->menuBar()->addMenu(tr("&Analysis"));
    analysis->addAction(tr("Analyze Program"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), this,
                        &MainWindow::analyzeCurrent);
    plainKey(analysis, tr("Define Function"), Qt::Key_P, QStringLiteral("P"), [] {});
    plainKey(analysis, tr("Undefine"), Qt::Key_U, QStringLiteral("U"), [] {});

    QMenu *tools = titleBar_->menuBar()->addMenu(tr("&Tools"));
    plainKey(tools, tr("What Astral Knows About This Function"), Qt::Key_K, QStringLiteral("K"), &MainWindow::showFunctionFacts);
    tools->addAction(tr("Learn Names From This Program"), this, &MainWindow::learnNames);
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

    bar->addAction(QStringLiteral("◀"), this, [] {})->setToolTip(tr("Back"));
    bar->addAction(QStringLiteral("▶"), this, [] {})->setToolTip(tr("Forward"));
    bar->addSeparator();

    addressBox_ = new QComboBox;
    addressBox_->setEditable(true);
    addressBox_->setInsertPolicy(QComboBox::InsertAtTop);
    addressBox_->setMinimumWidth(220);
    addressBox_->lineEdit()->setPlaceholderText(tr("address or name"));
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    addressBox_->setFont(mono);
    connect(addressBox_->lineEdit(), &QLineEdit::returnPressed, this, &MainWindow::goToTarget);
    bar->addWidget(addressBox_);

    auto *search = new QLineEdit;
    search->setPlaceholderText(tr("Search everything"));
    search->setClearButtonEnabled(true);
    search->setMinimumWidth(260);
    bar->addSeparator();
    bar->addWidget(search);

    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);
    analyzeAction_ = bar->addAction(tr("Analyze"), this, &MainWindow::analyzeCurrent);
    analyzeAction_->setToolTip(tr("Decompile every function now, so navigation is instant"));
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
    connect(projectTree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        const QVariant address = item->data(0, Qt::UserRole);
        if (address.isValid())
            if (ProgramTab *tab = currentTab())
                tab->showFunction(address.toULongLong());
    });
    QDockWidget *project = addPane(tr("Project"), QStringLiteral("projectPane"), projectTree_,
                                   Qt::LeftDockWidgetArea);
    functionsPane_ = new FunctionsPane;
    connect(functionsPane_, &FunctionsPane::functionActivated, this, [this](quint64 address) {
        if (ProgramTab *tab = currentTab())
            tab->showFunction(address);
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
    connect(listingView_, &ListingView::nopRequested, this, [this](quint64 address, int count) {
        applyPatch([this, address, count](QString &error) {
            return currentTab()->document()->patchNop(address, count, error);
        }, tr("no-op %n instruction(s) at 0x%1", nullptr, count).arg(address, 0, 16));
    });
    connect(listingView_, &ListingView::invertRequested, this, [this](quint64 address) {
        applyPatch([this, address](QString &error) {
            return currentTab()->document()->patchInvert(address, error);
        }, tr("invert branch at 0x%1").arg(address, 0, 16));
    });
    connect(listingView_, &ListingView::returnRequested, this, [this](quint64 address) {
        bool ok = false;
        const QString text = QInputDialog::getText(this, tr("Return value"),
                                                   tr("Make the function at 0x%1 return:").arg(address, 0, 16),
                                                   QLineEdit::Normal, QStringLiteral("0"), &ok);
        if (!ok)
            return;
        bool parsed = false;
        const quint64 value = text.startsWith(QStringLiteral("0x")) ? text.mid(2).toULongLong(&parsed, 16)
                                                                     : text.toULongLong(&parsed, 10);
        if (!parsed) {
            statusBar()->showMessage(tr("Not a number: %1").arg(text), 3000);
            return;
        }
        applyPatch([this, address, value](QString &error) {
            return currentTab()->document()->patchReturn(address, value, error);
        }, tr("return %1 from 0x%2").arg(value).arg(address, 0, 16));
    });
    QDockWidget *listing = addPane(tr("Listing"), QStringLiteral("listingPane"), listingView_,
                                   Qt::RightDockWidgetArea);
    listingDock_ = listing;

    // Bottom: tables and tools, as one tab group. The tab is the title, so
    // these panes draw no title bar of their own.
    referencesPane_ = new TablePane({tr("Direction"), tr("Function"), tr("Address")},
                                    tr("Filter references"));
    QDockWidget *xrefs = addPane(tr("Cross References"), QStringLiteral("xrefsPane"),
                                 referencesPane_, Qt::BottomDockWidgetArea);
    referencesDock_ = xrefs;
    searchPane_ = new TablePane({tr("Kind"), tr("Name"), tr("Address")}, tr("Filter results"));
    searchDock_ = addPane(tr("Search"), QStringLiteral("searchPane"), searchPane_,
                          Qt::BottomDockWidgetArea);
    for (TablePane *pane : {referencesPane_, searchPane_})
        connect(pane, &TablePane::addressActivated, this, [this](quint64 address) {
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
    resizeDocks({project, listing}, {300, 440}, Qt::Horizontal);
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
    saveLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
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

void MainWindow::restoreLayout()
{
    QSettings settings;
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
