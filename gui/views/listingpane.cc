#include "views/listingpane.hh"
#include "model/programdocument.hh"

#include <QRegularExpression>

#include <map>
#include "model/programdocument.hh"
#include "views/listingview.hh"
#include "theme/theme.hh"

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
    // The dock draws nothing behind a plain widget, so the row paints itself
    // in the panel colour rather than sitting on the window's black.
    header->setAutoFillBackground(true);
    QPalette headerPalette = header->palette();
    headerPalette.setColor(QPalette::Window, Theme::current().colour(QStringLiteral("panel")));
    header->setPalette(headerPalette);
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
    // The button label carries the state; a checked flat button reads as off.
    editButton_->setText(editing ? tr("Editing") : tr("Edit"));
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

namespace {

// One line of a listing, split into the address it sits at and the
// instruction written there. Comments and blank lines have no address.
struct ListingLine {
    quint64 address = 0;
    QString instruction;
    bool hasAddress = false;
};

ListingLine readLine(const QString &line)
{
    static const QRegularExpression shape(QStringLiteral(R"(^\s*(0x[0-9a-fA-F]+)\s*:\s*(.*)$)"));
    const auto match = shape.match(line);
    ListingLine out;
    if (!match.hasMatch())
        return out;
    bool ok = false;
    out.address = QStringView(match.captured(1)).mid(2).toULongLong(&ok, 16);
    if (!ok)
        return out;
    // The trailing note the listing adds is for the reader, not the assembler.
    QString text = match.captured(2);
    const int comment = text.indexOf(QStringLiteral(";"));
    if (comment >= 0)
        text = text.left(comment);
    out.instruction = text.simplified();
    out.hasAddress = true;
    return out;
}

} // namespace

void ListingPane::assemble()
{
    if (!editing_ || busy_ || document_ == nullptr)
        return;
    assembleWithEngine();
}

void ListingPane::assembleWithEngine()
{
    // Every line the user changed is one instruction at a known address, which
    // is exactly what the engine's assembler takes. Lines left alone are not
    // touched, so a listing is edited in place rather than rebuilt.
    std::map<quint64, QString> before;
    for (const QString &line : pristine_.split(QLatin1Char('\n'))) {
        const ListingLine parsed = readLine(line);
        if (parsed.hasAddress)
            before[parsed.address] = parsed.instruction;
    }

    std::vector<std::pair<quint64, QString>> changes;
    QStringList unknown;
    for (const QString &line : view_->toPlainText().split(QLatin1Char('\n'))) {
        const ListingLine parsed = readLine(line);
        if (!parsed.hasAddress) {
            if (!line.trimmed().isEmpty())
                unknown << line.trimmed();
            continue;
        }
        const auto found = before.find(parsed.address);
        if (found == before.end()) {
            unknown << line.trimmed();
            continue;
        }
        if (found->second != parsed.instruction)
            changes.push_back({parsed.address, parsed.instruction});
    }

    if (!unknown.isEmpty()) {
        const QString problem = tr("every line has to keep its address, and no line may be added: "
                                   "%1").arg(unknown.first());
        Q_EMIT logMessage(tr("assemble refused: %1").arg(problem));
        status_->setText(tr("line without an address"));
        return;
    }
    if (changes.empty()) {
        status_->setText(tr("nothing changed"));
        return;
    }

    QStringList written;
    for (const auto &[address, text] : changes) {
        QString error;
        if (!document_->patchAssembly(address, text, error)) {
            Q_EMIT logMessage(tr("assemble refused at 0x%1 (%2): %3").arg(address, 0, 16).arg(text, error));
            status_->setText(tr("refused at 0x%1").arg(address, 0, 16));
            return;
        }
        written << QStringLiteral("0x%1 %2").arg(address, 0, 16).arg(text);
    }
    Q_EMIT logMessage(tr("assembled %n instruction(s) with the engine: %1", nullptr,
                         static_cast<int>(written.size()))
                          .arg(written.join(QStringLiteral(", "))));
    status_->setText(tr("queued %n instruction(s)", nullptr, static_cast<int>(written.size())));
    editButton_->setChecked(false);
    setEditing(false);
    Q_EMIT patchApplied();
}

} // namespace astral::gui
