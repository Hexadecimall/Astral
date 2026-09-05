#include "views/debuggerpane.hh"

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
}

DebuggerPane::~DebuggerPane() = default;

QWidget *DebuggerPane::registersView() const { return registers_; }
QWidget *DebuggerPane::stackView() const { return stack_; }
QWidget *DebuggerPane::outputView() const { return output_; }
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

    startButton_ = control(QStringLiteral("\u25B6"), tr("Run"));
    stepButton_ = control(QStringLiteral("\u2193"), tr("Step: one instruction, entering any call"));
    overButton_ = control(QStringLiteral("\u21B7"), tr("Step over: run any call to completion"));
    outButton_ = control(QStringLiteral("\u2191"), tr("Step out: until this frame returns"));
    goButton_ = control(QStringLiteral("\u25B7\u25B7"), tr("Continue: until a breakpoint, or the end"));
    stopButton_ = control(QStringLiteral("\u25A0"), tr("Stop"));
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
                registers_->clear();
                for (const DebugRegister &one : registers)
                    new QTreeWidgetItem(registers_, {one.name, QStringLiteral("0x%1").arg(one.value, 0, 16)});
            });
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
