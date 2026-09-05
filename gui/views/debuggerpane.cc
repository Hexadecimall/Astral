#include "views/debuggerpane.hh"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTabWidget>
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

    auto *tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    registers_ = table({tr("Register"), tr("Value")});
    stack_ = table({tr("Address"), tr("Function")});
    output_ = new QPlainTextEdit;
    output_->setReadOnly(true);
    output_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output_->setPlaceholderText(tr("What the program writes, and the calls it makes"));
    tabs->addTab(registers_, tr("Registers"));
    tabs->addTab(stack_, tr("Stack"));
    tabs->addTab(output_, tr("Output"));
    layout->addWidget(tabs, 1);
}

DebuggerPane::~DebuggerPane() = default;

void DebuggerPane::buildControls(QVBoxLayout *layout)
{
    auto *bar = new QWidget;
    bar->setObjectName(QStringLiteral("decompilerHeader"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(8, 5, 8, 5);
    row->setSpacing(6);

    startButton_ = control(tr("Start"), tr("Back to the first instruction, nothing executed"));
    stepButton_ = control(tr("Step"), tr("One instruction, entering any call it makes"));
    overButton_ = control(tr("Over"), tr("One instruction, running any call to completion"));
    outButton_ = control(tr("Out"), tr("Until the frame it is in returns"));
    goButton_ = control(tr("Continue"), tr("Until a breakpoint, or the end"));
    stopButton_ = control(tr("Stop"), tr("Ask a run in progress to stop"));
    for (QToolButton *button : {startButton_, stepButton_, overButton_, outButton_, goButton_, stopButton_})
        row->addWidget(button);

    row->addSpacing(10);
    argumentsBox_ = new QLineEdit;
    argumentsBox_->setPlaceholderText(tr("arguments, separated by spaces"));
    argumentsBox_->setMinimumWidth(180);
    row->addWidget(argumentsBox_, 1);
    inputBox_ = new QLineEdit;
    inputBox_->setPlaceholderText(tr("input"));
    inputBox_->setMaximumWidth(140);
    row->addWidget(inputBox_);

    status_ = new QLabel(tr("no program"));
    status_->setObjectName(QStringLiteral("muted"));
    row->addSpacing(10);
    row->addWidget(status_);
    layout->addWidget(bar);

    connect(startButton_, &QToolButton::clicked, this, [this] {
        if (!session_)
            return;
        session_->setArguments(argumentsBox_->text().split(QLatin1Char(' '), Qt::SkipEmptyParts));
        session_->setInput(inputBox_->text());
        QMetaObject::invokeMethod(session_.get(), "start", Qt::QueuedConnection);
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
    status_->setText(tr("ready"));
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
    argumentsBox_->setText(arguments.join(QLatin1Char(' ')));
    session_->setArguments(arguments);
    if (breakpoint != 0)
        toggleBreakpoint(breakpoint);
    QMetaObject::invokeMethod(session_.get(), "start", Qt::QueuedConnection);
    QMetaObject::invokeMethod(session_.get(), "go", Qt::QueuedConnection);
}

void DebuggerPane::applyState(const DebugState &state)
{
    status_->setText(state.reason.isEmpty() ? tr("stopped") : state.reason);
    status_->setToolTip(tr("%1 steps").arg(state.steps));
    if (state.address != 0)
        Q_EMIT locationChanged(state.address);
    const bool live = state.live;
    for (QToolButton *button : {stepButton_, overButton_, outButton_, goButton_})
        button->setEnabled(live);
}

void DebuggerPane::setBusy(bool busy)
{
    for (QToolButton *button : {startButton_, stepButton_, overButton_, outButton_, goButton_})
        button->setEnabled(!busy && (button == startButton_ || (session_ && session_->state().live)));
    stopButton_->setEnabled(busy);
}

} // namespace astral::gui
