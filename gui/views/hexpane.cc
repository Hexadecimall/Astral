#include "views/hexpane.hh"
#include "model/programdocument.hh"
#include "views/hexview.hh"
#include "theme/theme.hh"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

namespace astral::gui {

HexPane::HexPane(HexView *view, QWidget *parent)
    : QWidget(parent), view_(view),
      applyButton_(new QPushButton(tr("Patch"))), revertButton_(new QPushButton(tr("Revert"))),
      status_(new QLabel)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new QWidget;
    auto *row = new QHBoxLayout(header);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(6);
    // The dock draws nothing behind a plain widget, so the row paints itself
    // in the panel colour rather than sitting on the window's black.
    header->setAutoFillBackground(true);
    QPalette headerPalette = header->palette();
    headerPalette.setColor(QPalette::Window, Theme::current().colour(QStringLiteral("panel")));
    header->setPalette(headerPalette);
    applyButton_->setToolTip(tr("Queue the changed bytes as a patch (Ctrl+Return)"));
    for (QPushButton *button : {applyButton_, revertButton_})
        button->setFocusPolicy(Qt::NoFocus);
    status_->setObjectName(QStringLiteral("muted"));
    row->addWidget(applyButton_);
    row->addWidget(revertButton_);
    row->addWidget(status_, 1);
    layout->addWidget(header);
    layout->addWidget(view_, 1);

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
    // The bytes are always open to being typed over; nothing has to be
    // announced first.
    view_->setEditing(true);
    updateButtons();
}

void HexPane::updateButtons()
{
    // Only a changed byte makes Patch mean anything, so the buttons say
    // whether anything has been typed over.
    const bool dirty = view_->dirtyCount() > 0;
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
