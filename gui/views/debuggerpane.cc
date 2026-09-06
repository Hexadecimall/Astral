#include "views/debuggerpane.hh"

#include "model/debugsettings.hh"

#include <QAbstractItemView>
#include <QLineEdit>
#include <QMenu>
#include <QAction>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTabWidget>
#include "views/runconfigdialog.hh"

#include <QComboBox>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace astral::gui {

namespace {

QToolButton *control(const QString &text, const QString &tip)
{
    auto *button = new QToolButton;
    button->setObjectName(QStringLiteral("headerButton"));
    button->setText(text);
    button->setToolTip(tip);
    button->setEnabled(false);
    return button;
}

QTreeWidget *table(const QStringList &headers)
{
    auto *view = new QTreeWidget;
    view->setHeaderLabels(headers);
    view->setRootIsDecorated(false);
    view->setUniformRowHeights(true);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->header()->setStretchLastSection(true);
    return view;
}

} // namespace

DebuggerPane::DebuggerPane(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    buildControls(layout);

    // The three views are docked by the window, not stacked in here.
    registers_ = table({tr("Register"), tr("Value")});
    stack_ = table({tr("Address"), tr("Function")});
    output_ = new QPlainTextEdit;
    output_->setReadOnly(true);
    output_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output_->setPlaceholderText(tr("What the program writes, and the calls it makes"));
    // A register is written by typing over its value, so the column that holds
    // it is the one that can be edited.
    registers_->setEditTriggers(QAbstractItemView::DoubleClicked |
                                QAbstractItemView::SelectedClicked |
                                QAbstractItemView::EditKeyPressed);
    connect(registers_, &QTreeWidget::itemChanged, this, &DebuggerPane::writeRegister);
    // Double-clicking a frame goes to it, which is how a stack is read.
    connect(stack_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        bool ok = false;
        const quint64 address = QStringView(item->text(0)).mid(2).toULongLong(&ok, 16);
        if (ok)
            Q_EMIT locationChanged(address);
    });
    buildMemory();
    buildBreakpoints();
}

DebuggerPane::~DebuggerPane() = default;

QWidget *DebuggerPane::registersView() const { return registers_; }
QWidget *DebuggerPane::stackView() const { return stack_; }
QWidget *DebuggerPane::outputView() const { return output_; }
QWidget *DebuggerPane::memoryView() const { return memory_; }
QWidget *DebuggerPane::breakpointsView() const { return breakpoints_; }
bool DebuggerPane::isDebugging() const { return debugging_; }

void DebuggerPane::buildControls(QVBoxLayout *layout)
{
    auto *bar = new QWidget;
    controls_ = bar;
    bar->setObjectName(QStringLiteral("decompilerHeader"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(8, 5, 8, 5);
    row->setSpacing(6);

    // Which way to run it, and the editor for those ways. This is where
    // arguments and input live now, kept and named rather than retyped.
    configBox_ = new QComboBox;
    configBox_->setObjectName(QStringLiteral("runConfigBox"));
    configBox_->setMinimumWidth(150);
    configBox_->setToolTip(tr("How to run it. Edit these to set arguments, input and where to begin."));
    row->addWidget(configBox_);

    startButton_ = control(QStringLiteral("\u25B8"), tr("Run"));
    stepButton_ = control(QStringLiteral("\u2193\ufe0e"), tr("Step: one instruction, entering any call"));
    overButton_ = control(QStringLiteral("\u21B7\ufe0e"), tr("Step over: run any call to completion"));
    outButton_ = control(QStringLiteral("\u2191\ufe0e"), tr("Step out: until this frame returns"));
    goButton_ = control(QStringLiteral("\u25B9\u25B9"), tr("Continue: until a breakpoint, or the end"));
    stopButton_ = control(QStringLiteral("\u25A0\ufe0e"), tr("Stop"));
    for (QToolButton *button : {startButton_, stepButton_, overButton_, outButton_, goButton_, stopButton_}) {
        button->setObjectName(QStringLiteral("transportButton"));
        row->addWidget(button);
    }
    // The steps mean nothing until something is stopped, so they stay out of
    // the way until there is.
    for (QToolButton *button : {stepButton_, overButton_, outButton_, goButton_, stopButton_})
        button->hide();

    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("muted"));
    row->addSpacing(8);
    row->addWidget(status_);
    layout->addWidget(bar);

    connect(configBox_, &QComboBox::activated, this, [this](int index) {
        if (index == configBox_->count() - 1) {
            editConfigurations();
            return;
        }
        if (index >= 0 && index < static_cast<int>(configurations_.size()))
            RunConfigurations::setChosen(path_, configurations_[index].name);
    });

    connect(startButton_, &QToolButton::clicked, this, [this] {
        if (!session_)
            return;
        const RunConfiguration one = current();
        // Live is a setting with no back end behind it yet. Saying so is the
        // only honest thing to do: quietly emulating instead would answer a
        // different question than the one that was asked.
        if (DebugSettings::value(path_, one.name, QStringLiteral("engine")) ==
            QStringLiteral("live")) {
            status_->setText(tr("live runs need a native back end, which is not built yet"));
            Q_EMIT logMessage(tr("debug: this run is set to live, and Astral has no native "
                                 "back end yet. Set the engine to emulate in Debug Settings, "
                                 "or choose Emulate under the Debug button."));
            return;
        }
        session_->setArguments(one.arguments);
        session_->setInput(one.input);
        QMetaObject::invokeMethod(session_.get(), "start", Qt::QueuedConnection);
        if (!one.stopAtStart)
            QMetaObject::invokeMethod(session_.get(), "go", Qt::QueuedConnection);
    });
    auto drive = [this](const char *slot) {
        if (session_)
            QMetaObject::invokeMethod(session_.get(), slot, Qt::QueuedConnection);
    };
    connect(stepButton_, &QToolButton::clicked, this, [drive] { drive("step"); });
    connect(overButton_, &QToolButton::clicked, this, [drive] { drive("stepOver"); });
    connect(outButton_, &QToolButton::clicked, this, [drive] { drive("stepOut"); });
    connect(goButton_, &QToolButton::clicked, this, [drive] { drive("go"); });
    // Cancel is the one thing that may be called while it is running.
    connect(stopButton_, &QToolButton::clicked, this, [this] {
        if (session_)
            session_->cancel();
    });
}


// ------------------------------------------------------------------- memory
//
// The dump: sixteen bytes to a line against their address, following whatever
// it is pointed at. Editing a line writes those bytes into the program, which
// is the whole reason a debugger shows memory rather than describing it.

void DebuggerPane::buildMemory()
{
    memory_ = new QWidget;
    auto *layout = new QVBoxLayout(memory_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *row = new QWidget;
    row->setObjectName(QStringLiteral("decompilerHeader"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    auto *bar = new QHBoxLayout(row);
    bar->setContentsMargins(8, 4, 8, 4);
    bar->setSpacing(6);
    memoryWhere_ = new QLineEdit;
    memoryWhere_->setPlaceholderText(tr("Address, a register name, or sp"));
    memoryWhere_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    bar->addWidget(memoryWhere_, 1);
    auto *watch = new QToolButton;
    watch->setObjectName(QStringLiteral("transportButton"));
    watch->setText(tr("Watch"));
    watch->setToolTip(tr("Stop when these bytes are written"));
    bar->addWidget(watch);
    layout->addWidget(row);

    memoryBytes_ = new QPlainTextEdit;
    memoryBytes_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    memoryBytes_->setLineWrapMode(QPlainTextEdit::NoWrap);
    memoryBytes_->setPlaceholderText(tr("Bytes the program can see, once it is running"));
    layout->addWidget(memoryBytes_, 1);

    // A register name follows that register, so pointing the dump at sp keeps
    // it on the stack as the stack moves.
    connect(memoryWhere_, &QLineEdit::returnPressed, this, [this] { refreshMemory(); });
    connect(watch, &QToolButton::clicked, this, [this] {
        if (memoryAt_ != 0)
            toggleWatchpoint(memoryAt_, 16);
    });
    // Editing the dump writes it back. Only the bytes that changed are sent,
    // so a stray keystroke somewhere else in the pane costs nothing.
    connect(memoryBytes_, &QPlainTextEdit::textChanged, this, [this] {
        if (applying_ || session_ == nullptr || !debugging_)
            return;
        const QStringList lines = memoryBytes_->toPlainText().split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const int colon = line.indexOf(QLatin1Char(':'));
            if (colon <= 0)
                continue;
            bool ok = false;
            const quint64 at = QStringView(line).left(colon).trimmed().toULongLong(&ok, 16);
            if (!ok)
                continue;
            const QStringList parts =
                line.mid(colon + 1).simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            QByteArray bytes;
            for (const QString &part : parts) {
                if (part.size() != 2)
                    break;
                bool byteOk = false;
                const uint value = part.toUInt(&byteOk, 16);
                if (!byteOk)
                    break;
                bytes.append(static_cast<char>(value));
            }
            if (!bytes.isEmpty())
                QMetaObject::invokeMethod(session_.get(), "writeMemory", Qt::QueuedConnection,
                                          Q_ARG(quint64, at), Q_ARG(QByteArray, bytes));
        }
    });
}

void DebuggerPane::showMemory(quint64 address)
{
    memoryAt_ = address;
    if (memoryWhere_ != nullptr)
        memoryWhere_->setText(QStringLiteral("0x%1").arg(address, 0, 16));
    refreshMemory();
}

void DebuggerPane::refreshMemory()
{
    if (session_ == nullptr || memoryWhere_ == nullptr)
        return;
    const QString where = memoryWhere_->text().trimmed();
    if (where.isEmpty())
        return;
    // A name is looked up among the registers, so `sp` follows the stack.
    quint64 address = 0;
    bool ok = false;
    QString hex = where;
    if (hex.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        hex = hex.mid(2);
    address = hex.toULongLong(&ok, 16);
    if (!ok) {
        for (int i = 0; i < registers_->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = registers_->topLevelItem(i);
            if (item->text(0).compare(where, Qt::CaseInsensitive) != 0)
                continue;
            address = QStringView(item->text(1)).mid(2).toULongLong(&ok, 16);
            break;
        }
    }
    if (!ok)
        return;
    memoryAt_ = address;
    QMetaObject::invokeMethod(session_.get(), "readMemory", Qt::QueuedConnection,
                              Q_ARG(quint64, address), Q_ARG(int, 256));
}

// -------------------------------------------------------------- breakpoints
//
// Everywhere the run will stop, in one list: the addresses it breaks at and
// the memory it watches. Double-clicking one goes there; delete takes it away.

void DebuggerPane::buildBreakpoints()
{
    breakpoints_ = table({tr("Kind"), tr("Where"), tr("What")});
    breakpoints_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(breakpoints_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        bool ok = false;
        const quint64 address = QStringView(item->text(1)).mid(2).toULongLong(&ok, 16);
        if (ok)
            Q_EMIT locationChanged(address);
    });
    connect(breakpoints_, &QWidget::customContextMenuRequested, this, [this](const QPoint &at) {
        QTreeWidgetItem *item = breakpoints_->itemAt(at);
        if (item == nullptr)
            return;
        bool ok = false;
        const quint64 address = QStringView(item->text(1)).mid(2).toULongLong(&ok, 16);
        if (!ok)
            return;
        QMenu menu;
        const bool isWatch = item->text(0) == tr("write");
        connect(menu.addAction(tr("Remove")), &QAction::triggered, this, [this, address, isWatch] {
            if (isWatch)
                toggleWatchpoint(address, 0);
            else
                toggleBreakpoint(address);
        });
        menu.exec(breakpoints_->viewport()->mapToGlobal(at));
    });
}

void DebuggerPane::refreshBreakpoints()
{
    if (breakpoints_ == nullptr || session_ == nullptr)
        return;
    breakpoints_->clear();
    for (quint64 address : session_->breakpoints()) {
        new QTreeWidgetItem(breakpoints_, {tr("execute"), QStringLiteral("0x%1").arg(address, 0, 16),
                                           tr("stops before this instruction")});
    }
    for (const DebugWatch &watch : session_->watchpoints()) {
        new QTreeWidgetItem(breakpoints_,
                            {tr("write"), QStringLiteral("0x%1").arg(watch.address, 0, 16),
                             tr("%n byte(s)", nullptr, static_cast<int>(watch.size))});
    }
}

bool DebuggerPane::hasWatchpoint(quint64 address) const
{
    return session_ && session_->hasWatchpoint(address);
}

void DebuggerPane::toggleWatchpoint(quint64 address, quint64 size)
{
    if (!session_)
        return;
    if (session_->hasWatchpoint(address) || size == 0)
        QMetaObject::invokeMethod(session_.get(), "removeWatchpoint", Qt::QueuedConnection,
                                  Q_ARG(quint64, address));
    else
        QMetaObject::invokeMethod(session_.get(), "addWatchpoint", Qt::QueuedConnection,
                                  Q_ARG(quint64, address), Q_ARG(quint64, size));
}

void DebuggerPane::runToAddress(quint64 address)
{
    if (session_)
        QMetaObject::invokeMethod(session_.get(), "runTo", Qt::QueuedConnection,
                                  Q_ARG(quint64, address));
}

// A register typed over is written into the program. What is typed is read as
// hex whether or not it says 0x, because that is what the column shows.
void DebuggerPane::writeRegister(QTreeWidgetItem *item, int column)
{
    if (applying_ || column != 1 || session_ == nullptr)
        return;
    QString text = item->text(1).trimmed();
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        text = text.mid(2);
    bool ok = false;
    const quint64 value = text.toULongLong(&ok, 16);
    if (!ok) {
        Q_EMIT logMessage(tr("debug: %1 is not a number").arg(item->text(1)));
        refreshMemory();
        return;
    }
    QMetaObject::invokeMethod(session_.get(), "setRegister", Qt::QueuedConnection,
                              Q_ARG(QString, item->text(0)), Q_ARG(quint64, value));
}


// The settings, put where each one belongs: some are the session's, some are
// the views'. Read here rather than remembered, so a change made in the dialog
// shows without restarting anything.
void DebuggerPane::applySettings(const QString &program, const QString &configuration)
{
    configuration_ = configuration;
    if (session_ == nullptr)
        return;
    QMetaObject::invokeMethod(
        session_.get(), "setTrace", Qt::QueuedConnection,
        Q_ARG(bool, DebugSettings::boolValue(program, configuration, QStringLiteral("trace"))));
    const int bytes = DebugSettings::intValue(program, configuration, QStringLiteral("dumpBytes"));
    if (bytes > 0 && memoryAt_ != 0)
        QMetaObject::invokeMethod(session_.get(), "readMemory", Qt::QueuedConnection,
                                  Q_ARG(quint64, memoryAt_), Q_ARG(int, bytes));
    // What the dump points at when a run stops.
    const QString follows =
        DebugSettings::value(program, configuration, QStringLiteral("dumpFollows"));
    if (follows != QStringLiteral("nothing") && memoryWhere_ != nullptr &&
        memoryWhere_->text().isEmpty())
        memoryWhere_->setText(follows);
}

// A snapshot is named for when it was taken, because what a person wants back
// is "before I did that" and the number of times they have done that is the
// only name that distinguishes them.
void DebuggerPane::snapshotHere()
{
    if (session_ == nullptr)
        return;
    const QString name = tr("step %1").arg(session_->state().steps);
    QMetaObject::invokeMethod(session_.get(), "takeSnapshot", Qt::QueuedConnection,
                              Q_ARG(QString, name));
    lastSnapshot_ = name;
    ++snapshots_;
}

void DebuggerPane::windBack()
{
    if (session_ == nullptr || lastSnapshot_.isEmpty()) {
        Q_EMIT logMessage(tr("debug: nothing has been snapshotted yet"));
        return;
    }
    QMetaObject::invokeMethod(session_.get(), "restoreSnapshot", Qt::QueuedConnection,
                              Q_ARG(QString, lastSnapshot_));
}

void DebuggerPane::setProgram(const QString &path)
{
    if (path == path_ && session_)
        return;
    path_ = path;
    session_ = std::make_unique<DebugSession>(path);
    connect(session_.get(), &DebugSession::stopped, this, &DebuggerPane::applyState);
    connect(session_.get(), &DebugSession::busyChanged, this, &DebuggerPane::setBusy);
    connect(session_.get(), &DebugSession::breakpointsChanged, this, &DebuggerPane::breakpointsChanged);
    connect(session_.get(), &DebugSession::message, this, [this](const QString &line) {
        output_->appendPlainText(line);
        Q_EMIT logMessage(tr("debug: %1").arg(line));
    });
    connect(session_.get(), &DebugSession::failed, this, [this](const QString &error) {
        status_->setText(tr("failed"));
        status_->setToolTip(error);
        Q_EMIT logMessage(tr("debug: %1").arg(error));
    });
    connect(session_.get(), &DebugSession::registersChanged, this,
            [this](const std::vector<DebugRegister> &registers) {
                // Refilling the table changes every item, and a change is how a
                // typed-over value is noticed, so the two are told apart here.
                applying_ = true;
                registers_->clear();
                for (const DebugRegister &one : registers) {
                    auto *item = new QTreeWidgetItem(
                        registers_, {one.name, QStringLiteral("0x%1").arg(one.value, 0, 16)});
                    item->setFlags(item->flags() | Qt::ItemIsEditable);
                }
                applying_ = false;
                // The dump follows a register when it was pointed at one.
                refreshMemory();
            });
    connect(session_.get(), &DebugSession::memoryRead, this,
            [this](quint64 address, const QByteArray &bytes) {
                applying_ = true;
                QString text;
                for (qsizetype offset = 0; offset < bytes.size(); offset += 16) {
                    QString hex, ascii;
                    for (int i = 0; i < 16 && offset + i < bytes.size(); ++i) {
                        const unsigned char c = static_cast<unsigned char>(bytes[offset + i]);
                        hex += QStringLiteral("%1 ").arg(c, 2, 16, QLatin1Char('0'));
                        ascii += (c >= 0x20 && c < 0x7f) ? QLatin1Char(char(c)) : QLatin1Char('.');
                    }
                    text += QStringLiteral("%1: %2 %3\n")
                                .arg(address + static_cast<quint64>(offset), 12, 16, QLatin1Char('0'))
                                .arg(hex.leftJustified(48), ascii);
                }
                memoryBytes_->setPlainText(text);
                applying_ = false;
            });
    connect(session_.get(), &DebugSession::watchpointsChanged, this,
            [this] { refreshBreakpoints(); Q_EMIT breakpointsChanged(); });
    connect(session_.get(), &DebugSession::breakpointsChanged, this,
            [this] { refreshBreakpoints(); });
    connect(session_.get(), &DebugSession::stackChanged, this,
            [this](const std::vector<DebugFrame> &frames) {
                stack_->clear();
                for (const DebugFrame &frame : frames)
                    new QTreeWidgetItem(stack_, {QStringLiteral("0x%1").arg(frame.address, 0, 16),
                                                 frame.function});
            });
    loadConfigurations();
    status_->clear();
    startButton_->setEnabled(true);
}

void DebuggerPane::setEntry(quint64 address, const QString &name)
{
    entry_ = address;
    Q_UNUSED(name);
    if (session_)
        session_->setEntry(0);
}

bool DebuggerPane::hasBreakpoint(quint64 address) const
{
    return session_ && session_->hasBreakpoint(address);
}

void DebuggerPane::toggleBreakpoint(quint64 address)
{
    if (session_)
        QMetaObject::invokeMethod(session_.get(), "toggleBreakpoint", Qt::QueuedConnection,
                                  Q_ARG(quint64, address));
}

quint64 DebuggerPane::currentAddress() const
{
    return session_ ? session_->state().address : 0;
}

void DebuggerPane::runForTesting(quint64 breakpoint, const QStringList &arguments)
{
    if (!session_)
        return;
    session_->setArguments(arguments);
    if (breakpoint != 0)
        toggleBreakpoint(breakpoint);
    QMetaObject::invokeMethod(session_.get(), "start", Qt::QueuedConnection);
    QMetaObject::invokeMethod(session_.get(), "go", Qt::QueuedConnection);
}

void DebuggerPane::loadConfigurations()
{
    configurations_ = RunConfigurations::forProgram(path_);
    const QString chosen = RunConfigurations::chosen(path_);
    const QSignalBlocker blocker(configBox_);
    configBox_->clear();
    int select = 0;
    for (size_t i = 0; i < configurations_.size(); ++i) {
        configBox_->addItem(configurations_[i].name);
        if (configurations_[i].name == chosen)
            select = static_cast<int>(i);
    }
    configBox_->addItem(tr("Edit Configurations..."));
    configBox_->setCurrentIndex(select);
}

RunConfiguration DebuggerPane::current() const
{
    const int index = configBox_->currentIndex();
    if (index >= 0 && index < static_cast<int>(configurations_.size()))
        return configurations_[index];
    return RunConfigurations::byDefault();
}

void DebuggerPane::editConfigurations()
{
    if (path_.isEmpty())
        return;
    RunConfigDialog dialog(path_, configurations_, RunConfigurations::chosen(path_), this);
    if (dialog.exec() != QDialog::Accepted) {
        loadConfigurations();
        return;
    }
    RunConfigurations::save(path_, dialog.configurations());
    RunConfigurations::setChosen(path_, dialog.chosen());
    loadConfigurations();
}

void DebuggerPane::applyState(const DebugState &state)
{
    if (state.live != debugging_) {
        debugging_ = state.live;
        Q_EMIT debuggingChanged(debugging_);
    }
    status_->setText(state.reason.isEmpty() ? tr("stopped") : state.reason);
    status_->setToolTip(tr("%1 steps").arg(state.steps));
    if (state.address != 0)
        Q_EMIT locationChanged(state.address);
    // Once something is stopped there are steps to take; until then there
    // are not, and the buttons are not there to be wondered about.
    const bool live = state.live;
    for (QToolButton *button : {stepButton_, overButton_, outButton_, goButton_}) {
        button->setVisible(live);
        button->setEnabled(live);
    }
    stopButton_->setVisible(live);
    configBox_->setEnabled(!live);
}

void DebuggerPane::setBusy(bool busy)
{
    for (QToolButton *button : {startButton_, stepButton_, overButton_, outButton_, goButton_})
        button->setEnabled(!busy && (button == startButton_ || (session_ && session_->state().live)));
    stopButton_->setEnabled(busy);
}

} // namespace astral::gui
