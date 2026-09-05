#include "app/welcomepage.hh"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

namespace astral::gui {

namespace {
constexpr int kMaxRecent = 8;
constexpr int kColumnWidth = 820;
const auto kRecentKey = QStringLiteral("recent/projects");

QLabel *label(const QString &text, const char *name)
{
    auto *l = new QLabel(text);
    l->setObjectName(QString::fromLatin1(name));
    return l;
}
} // namespace

Card::Card(const QString &glyph, const QString &title, const QString &detail, QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("card"));
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(16, 14, 16, 14);
    row->setSpacing(14);
    auto *icon = label(glyph, "cardGlyph");
    icon->setFixedWidth(28);
    icon->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    row->addWidget(icon);
    auto *text = new QVBoxLayout;
    text->setSpacing(3);
    text->addWidget(label(title, "cardTitle"));
    auto *sub = label(detail, "cardDetail");
    sub->setWordWrap(true);
    text->addWidget(sub);
    row->addLayout(text, 1);
}

void Card::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->pos()))
        Q_EMIT clicked();
    QFrame::mouseReleaseEvent(event);
}

WelcomePage::WelcomePage(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("welcomePage"));
    setAttribute(Qt::WA_StyledBackground, true);

    // Everything sits in one column of fixed width, centred, inside a scroll
    // area so a short window still reaches the footer.
    auto *scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("welcomePage"));
    scroll->setWidget(page);
    auto *pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(24, 0, 24, 0);
    auto *column = new QWidget;
    column->setMaximumWidth(kColumnWidth);
    pageLayout->addStretch(1);
    pageLayout->addWidget(column, 100);
    pageLayout->addStretch(1);

    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 56, 0, 32);
    layout->setSpacing(0);

    // Hero.
    auto *hero = new QHBoxLayout;
    hero->setSpacing(22);
    auto *logo = new QLabel;
    logo->setPixmap(QIcon(QStringLiteral(":/icons/logo.svg")).pixmap(88, 88));
    hero->addWidget(logo, 0, Qt::AlignTop);
    auto *heroText = new QVBoxLayout;
    heroText->setSpacing(4);
    heroText->addWidget(label(QStringLiteral("Astral"), "welcomeTitle"));
    heroText->addWidget(label(tr("A decompiler that emits C which compiles."), "welcomeTagline"));
    heroText->addWidget(label(tr("Version %1").arg(QCoreApplication::applicationVersion()), "muted"));
    heroText->addStretch(1);
    hero->addLayout(heroText, 1);
    layout->addLayout(hero);
    layout->addSpacing(40);

    // Start.
    layout->addWidget(label(tr("Start"), "welcomeHeading"));
    layout->addSpacing(10);
    auto *cards = new QGridLayout;
    cards->setHorizontalSpacing(12);
    cards->setVerticalSpacing(12);
    auto *newProject = new Card(QStringLiteral("＋"), tr("New Project"),
                                tr("A folder that holds programs, their analysis and shared types."));
    auto *openBinary = new Card(QStringLiteral("⌘"), tr("Open Binary"),
                                tr("Mach-O, ELF or PE. Creates a project beside it if there is none."));
    auto *openProject = new Card(QStringLiteral("▤"), tr("Open Project"),
                                 tr("Continue where you left off in an existing .astralproj."));
    connect(newProject, &Card::clicked, this, &WelcomePage::newProjectRequested);
    connect(openBinary, &Card::clicked, this, &WelcomePage::openRequested);
    connect(openProject, &Card::clicked, this, &WelcomePage::openRequested);
    cards->addWidget(newProject, 0, 0);
    cards->addWidget(openBinary, 0, 1);
    cards->addWidget(openProject, 0, 2);
    layout->addLayout(cards);
    layout->addSpacing(36);

    // Recent.
    recentHeading_ = label(tr("Recent"), "welcomeHeading");
    layout->addWidget(recentHeading_);
    layout->addSpacing(10);
    recentBox_ = new QFrame;
    recentBox_->setObjectName(QStringLiteral("recentBox"));
    recentBox_->setAttribute(Qt::WA_StyledBackground, true);
    recentLayout_ = new QVBoxLayout(recentBox_);
    recentLayout_->setContentsMargins(0, 0, 0, 0);
    recentLayout_->setSpacing(0);
    layout->addWidget(recentBox_);
    layout->addSpacing(36);

    // What changed.
    layout->addWidget(label(tr("What's new in %1").arg(QCoreApplication::applicationVersion()),
                            "welcomeHeading"));
    layout->addSpacing(10);
    auto *news = new QFrame;
    news->setObjectName(QStringLiteral("newsBox"));
    news->setAttribute(Qt::WA_StyledBackground, true);
    auto *newsLayout = new QVBoxLayout(news);
    newsLayout->setContentsMargins(18, 14, 18, 14);
    newsLayout->setSpacing(6);
    for (const QString &line : {tr("Desktop application, first look"),
                                tr("Colour for decompiled C, assembly and p-code on a terminal"),
                                tr("Locals declared at first assignment; copy-only locals folded away"),
                                tr("main's argc and argv recovered; used void results coerced")}) {
        auto *item = label(QStringLiteral("•  ") + line, "newsItem");
        item->setWordWrap(true);
        newsLayout->addWidget(item);
    }
    layout->addWidget(news);
    layout->addStretch(1);

    refresh();
}

void WelcomePage::refresh()
{
    while (QLayoutItem *item = recentLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const QStringList paths = recentPaths();
    if (paths.isEmpty()) {
        auto *empty = label(tr("Nothing yet. Projects and binaries you open appear here."), "muted");
        empty->setContentsMargins(18, 14, 18, 14);
        recentLayout_->addWidget(empty);
        return;
    }
    for (const QString &path : paths) {
        QFileInfo info(path);
        auto *row = new Card(info.suffix() == QStringLiteral("astralproj") ? QStringLiteral("▤")
                                                                             : QStringLiteral("⌘"),
                             info.fileName(), info.absolutePath());
        row->setObjectName(QStringLiteral("recentRow"));
        row->setToolTip(path);
        connect(row, &Card::clicked, this, [this, path] { Q_EMIT recentRequested(path); });
        recentLayout_->addWidget(row);
    }
}

void WelcomePage::rememberRecent(const QString &path)
{
    QSettings settings;
    QStringList paths = settings.value(kRecentKey).toStringList();
    paths.removeAll(path);
    paths.prepend(path);
    while (paths.size() > kMaxRecent)
        paths.removeLast();
    settings.setValue(kRecentKey, paths);
}

QStringList WelcomePage::recentPaths()
{
    return QSettings().value(kRecentKey).toStringList();
}

} // namespace astral::gui
