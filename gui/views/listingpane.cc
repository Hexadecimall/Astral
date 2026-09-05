#include "views/listingpane.hh"
#include "model/assembler.hh"
#include "model/programdocument.hh"
#include "views/listingview.hh"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

namespace astral::gui {

ListingPane::ListingPane(ListingView *view, QWidget *parent)
    : QWidget(parent), view_(view), editButton_(new QPushButton(tr("Edit"))),
      assembleButton_(new QPushButton(tr("Assemble"))), revertButton_(new QPushButton(tr("Revert"))),
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
    editButton_->setToolTip(tr("Edit the disassembly and assemble it back into the program"));
    assembleButton_->setToolTip(tr("Assemble the edited listing in place (Ctrl+Return)"));
    for (QPushButton *button : {editButton_, assembleButton_, revertButton_})
        button->setFocusPolicy(Qt::NoFocus);
    status_->setObjectName(QStringLiteral("muted"));
    row->addWidget(editButton_);
    row->addWidget(assembleButton_);
    row->addWidget(revertButton_);
    row->addWidget(status_, 1);
    layout->addWidget(header);
    layout->addWidget(view_, 1);

    connect(editButton_, &QPushButton::toggled, this, [this](bool on) { setEditing(on); });
    connect(assembleButton_, &QPushButton::clicked, this, &ListingPane::assemble);
    connect(revertButton_, &QPushButton::clicked, this, [this] {
        view_->setPlainText(pristine_);
        status_->setText(tr("reverted to the disassembly"));
    });
    auto *shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, &ListingPane::assemble);

    updateButtons();
}

void ListingPane::setListing(const QString &text)
{
    pristine_ = text;
    editing_ = false;
    const QSignalBlocker blocker(editButton_);
    editButton_->setChecked(false);
    view_->setEditable(false);
    view_->setPlainText(text);
    status_->clear();
    updateButtons();
}

void ListingPane::setProgram(ProgramDocument *document, quint64 address)
{
    document_ = document;
    address_ = address;
    updateButtons();
}

void ListingPane::updateButtons()
{
    const bool possible = document_ != nullptr && !pristine_.isEmpty();
    editButton_->setEnabled(possible);
    assembleButton_->setEnabled(possible && editing_ && !busy_);
    revertButton_->setEnabled(editing_ && !busy_);
    assembleButton_->setVisible(editing_);
    revertButton_->setVisible(editing_);
}

void ListingPane::setEditing(bool editing)
{
    editing_ = editing;
    view_->setEditable(editing);
    if (!editing)
        view_->setPlainText(pristine_);
    status_->setText(editing ? tr("editing: assemble with Ctrl+Return") : QString());
    updateButtons();
}

quint64 ListingPane::span() const
{
    if (!document_)
        return 0;
    // The listing's own addresses bound it: from the first line to the end of
    // the last instruction. That is what the assembled block may occupy.
    quint64 last = 0;
    bool found = false;
    const QStringList lines = pristine_.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        QString head = line.left(colon).trimmed();
        if (head.startsWith(QStringLiteral("0x")))
            head = head.mid(2);
        bool ok = false;
        const quint64 at = head.toULongLong(&ok, 16);
        if (!ok)
            continue;
        last = at;
        found = true;
    }
    if (!found || last < address_)
        return 0;
    const int length = document_->instructionLength(last);
    if (length <= 0)
        return 0;
    return last + static_cast<quint64>(length) - address_;
}

void ListingPane::assemble()
{
    if (!editing_ || busy_ || !document_)
        return;
    const quint64 available = span();
    if (available == 0) {
        Q_EMIT logMessage(tr("assemble refused: the listing's extent could not be measured"));
        status_->setText(tr("no measurable extent"));
        return;
    }
    busy_ = true;
    status_->setText(tr("assembling..."));
    updateButtons();
    auto *assembler = new Assembler(document_, this);
    assembler->assemble(view_->toPlainText(), address_, available,
                        [this, assembler](const AssembleOutcome &outcome) {
                            assembler->deleteLater();
                            busy_ = false;
                            if (outcome.ok) {
                                Q_EMIT logMessage(tr("assemble 0x%1: %2").arg(address_, 0, 16).arg(outcome.report));
                                status_->setText(tr("queued %1 bytes").arg(outcome.bytes.size()));
                                // The patch refreshes every view; leave edit
                                // mode so the re-read disassembly is shown.
                                editButton_->setChecked(false);
                                setEditing(false);
                                Q_EMIT patchApplied();
                                return;
                            }
                            if (!outcome.diagnostics.isEmpty())
                                Q_EMIT logMessage(tr("assemble refused: %1\n%2")
                                                      .arg(outcome.report, outcome.diagnostics));
                            else
                                Q_EMIT logMessage(tr("assemble refused: %1").arg(outcome.report));
                            status_->setText(outcome.report);
                            updateButtons();
                        });
}

} // namespace astral::gui
