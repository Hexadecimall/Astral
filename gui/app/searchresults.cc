#include "app/searchresults.hh"

#include <QApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace astral::gui {

namespace {
constexpr int kMaxShown = 200;
constexpr int kRowsVisible = 12;
}

SearchResults::SearchResults(QLineEdit *box, QWidget *parent)
    : QFrame(parent), box_(box), list_(new QListWidget(this)), summary_(new QLabel(this))
{
    setObjectName(QStringLiteral("searchResults"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFrameShape(QFrame::NoFrame);
    hide();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    list_->setObjectName(QStringLiteral("searchResultsList"));
    list_->setFrameShape(QFrame::NoFrame);
    list_->setUniformItemSizes(true);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(list_);
    summary_->setObjectName(QStringLiteral("searchResultsSummary"));
    summary_->setContentsMargins(10, 4, 10, 5);
    layout->addWidget(summary_);

    connect(list_, &QListWidget::itemClicked, this, [this] { chooseCurrent(); });
    // Typing must stay in the box, so the box's keys are read from here.
    box_->installEventFilter(this);
    qApp->installEventFilter(this);
}

void SearchResults::showMatches(const std::vector<SearchResults::Match> &matches, const QString &needle)
{
    list_->clear();
    if (matches.empty()) {
        hideMatches();
        return;
    }
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const int shown = static_cast<int>(std::min<size_t>(matches.size(), kMaxShown));
    for (int i = 0; i < shown; ++i) {
        const Match &match = matches[i];
        auto *item = new QListWidgetItem(QStringLiteral("%1\t%2\t0x%3")
                                             .arg(match.kind, match.name)
                                             .arg(match.address, 0, 16));
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(match.address));
        item->setFont(mono);
        list_->addItem(item);
    }
    list_->setCurrentRow(0);
    summary_->setText(matches.size() > static_cast<size_t>(shown)
                          ? tr("%1 of %2 matches for \"%3\"  ·  ↑↓ to pick, Enter to go")
                                .arg(shown).arg(matches.size()).arg(needle)
                          : tr("%n match(es) for \"%1\"  ·  ↑↓ to pick, Enter to go", nullptr, shown)
                                .arg(needle));

    const int rowHeight = list_->sizeHintForRow(0) > 0 ? list_->sizeHintForRow(0) : 20;
    list_->setFixedHeight(rowHeight * std::min(shown, kRowsVisible) + 4);
    reposition();
    show();
    raise();
}

void SearchResults::hideMatches()
{
    list_->clear();
    hide();
}

bool SearchResults::isShowing() const
{
    return isVisible() && list_->count() > 0;
}

void SearchResults::reposition()
{
    QWidget *host = parentWidget();
    if (host == nullptr)
        return;
    const QPoint below = box_->mapTo(host, QPoint(0, box_->height() + 2));
    setGeometry(below.x(), below.y(), box_->width(), sizeHint().height());
}

void SearchResults::step(int delta)
{
    const int count = list_->count();
    if (count == 0)
        return;
    int row = list_->currentRow() + delta;
    if (row < 0)
        row = count - 1;
    else if (row >= count)
        row = 0;
    list_->setCurrentRow(row);
}

void SearchResults::chooseCurrent()
{
    QListWidgetItem *item = list_->currentItem();
    if (item == nullptr)
        return;
    hideMatches();
    Q_EMIT chosen(item->data(Qt::UserRole).toULongLong());
}

bool SearchResults::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == box_ && event->type() == QEvent::KeyPress && isShowing()) {
        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Down:
            step(1);
            return true;
        case Qt::Key_Up:
            step(-1);
            return true;
        case Qt::Key_PageDown:
            step(kRowsVisible);
            return true;
        case Qt::Key_PageUp:
            step(-kRowsVisible);
            return true;
        case Qt::Key_Escape:
            hideMatches();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // Enter takes the highlighted match unless the box holds an exact
            // destination, which the window handles before this is reached.
            chooseCurrent();
            return true;
        default:
            break;
        }
    }
    // Anything clicked outside puts the list away.
    if (event->type() == QEvent::MouseButtonPress && isVisible()) {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget != nullptr && widget->window() == window() && !isAncestorOf(widget) && widget != box_)
            hideMatches();
    }
    if (watched == box_ && event->type() == QEvent::FocusOut && !isAncestorOf(QApplication::focusWidget()))
        hideMatches();
    return QFrame::eventFilter(watched, event);
}

} // namespace astral::gui
