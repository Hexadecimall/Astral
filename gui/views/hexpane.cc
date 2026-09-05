#include "views/hexpane.hh"
#include "model/programdocument.hh"
#include "views/hexview.hh"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

namespace astral::gui {

HexPane::HexPane(HexView *view, QWidget *parent)
    : QWidget(parent), view_(view), editButton_(new QPushButton(tr("Edit"))),
      applyButton_(new QPushButton(tr("Apply"))), revertButton_(new QPushButton(tr("Revert"))),
      status_(new QLabel)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new QWidget;
    auto *row = new QHBoxLayout(header);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(6);
    editButton_->setCheckable(true);
    editButton_->setToolTip(tr("Type hex digits over the bytes to change them"));
    applyButton_->setToolTip(tr("Queue the changed bytes as a patch (Ctrl+Return)"));
    for (QPushButton *button : {editButton_, applyButton_, revertButton_})
        button->setFocusPolicy(Qt::NoFocus);
    status_->setObjectName(QStringLiteral("muted"));
    row->addWidget(editButton_);
    row->addWidget(applyButton_);
    row->addWidget(revertButton_);
    row->addWidget(status_, 1);
    layout->addWidget(header);
    layout->addWidget(view_, 1);

    connect(editButton_, &QPushButton::toggled, this, [this](bool on) {
        view_->setEditing(on);
        if (!on)
            view_->revert();
        status_->setText(on ? tr("editing: type hex digits, apply with Ctrl+Return") : QString());
        updateButtons();
    });
    connect(applyButton_, &QPushButton::clicked, this, &HexPane::apply);
    connect(revertButton_, &QPushButton::clicked, this, [this] {
        view_->revert();
        status_->setText(tr("reverted to the bytes in the program"));
    });
    connect(view_, &HexView::editsChanged, this, [this](int count) {
        if (count > 0)
            status_->setText(tr("%n byte(s) changed", nullptr, count));
        updateButtons();
    });
    auto *shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, &HexPane::apply);

    updateButtons();
}

void HexPane::setDocument(ProgramDocument *document)
{
    document_ = document;
    updateButtons();
}

void HexPane::updateButtons()
{
    const bool dirty = view_->dirtyCount() > 0;
    editButton_->setEnabled(document_ != nullptr);
    applyButton_->setVisible(view_->editing());
    revertButton_->setVisible(view_->editing());
    applyButton_->setEnabled(dirty && document_ != nullptr);
    revertButton_->setEnabled(dirty);
}

void HexPane::apply()
{
    if (!document_ || view_->dirtyCount() == 0)
        return;
    const auto runs = view_->dirtyRuns();
    int written = 0;
    QStringList where;
    for (const auto &run : runs) {
        QString error;
        if (!document_->patchBytes(run.first, run.second,
                                   tr("%n byte(s) edited in the hex view", nullptr,
                                      static_cast<int>(run.second.size())),
                                   error)) {
            Q_EMIT logMessage(tr("hex apply refused at 0x%1: %2").arg(run.first, 0, 16).arg(error));
            status_->setText(error);
            return;
        }
        written += static_cast<int>(run.second.size());
        where << QStringLiteral("0x%1+%2").arg(run.first, 0, 16).arg(run.second.size());
    }
    Q_EMIT logMessage(tr("hex: queued %1 byte(s) in %2 run(s): %3")
                          .arg(written)
                          .arg(runs.size())
                          .arg(where.join(QStringLiteral(", "))));
    status_->setText(tr("queued %1 byte(s)").arg(written));
    view_->clearEdits();
    updateButtons();
    Q_EMIT patchApplied();
}

} // namespace astral::gui
