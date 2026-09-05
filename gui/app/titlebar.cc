#include "app/titlebar.hh"
#include "platform/window.hh"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QToolButton>
#include <QWindow>

namespace astral::gui {

namespace {
constexpr int kHeight = 32;
}

TitleBar::TitleBar(QWidget *window) : QWidget(window), window_(window)
{
    setObjectName(QStringLiteral("titleBar"));
    setFixedHeight(kHeight);
    // Plain QWidgets ignore stylesheet backgrounds without this attribute.
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 0, 0);
    layout->setSpacing(4);

    auto *logo = new QLabel;
    logo->setPixmap(QIcon(QStringLiteral(":/icons/logo-small.svg")).pixmap(20, 20));
    logo->setFixedSize(26, kHeight);
    logo->setAlignment(Qt::AlignCenter);
    layout->addWidget(logo);

    menuBar_ = new QMenuBar(this);
    // Inline in the title bar on every platform, so the bar never moves to
    // the top of the screen.
    menuBar_->setNativeMenuBar(false);
    menuBar_->setObjectName(QStringLiteral("titleMenuBar"));
    menuBar_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    layout->addWidget(menuBar_);

    // The rest of the bar is a drag handle. The window title is kept for the
    // operating system; the bar does not repeat it.
    layout->addStretch(1);
    title_ = new QLabel;
    title_->hide();

    auto *minimise = new WindowButton(WindowButton::Minimise, this);
    connect(minimise, &QToolButton::clicked, this, [this] { window_->showMinimized(); });
    maximise_ = new WindowButton(WindowButton::Maximise, this);
    connect(maximise_, &QToolButton::clicked, this, &TitleBar::toggleMaximised);
    auto *close = new WindowButton(WindowButton::Close, this);
    connect(close, &QToolButton::clicked, this, [this] { window_->close(); });
    layout->addWidget(minimise);
    layout->addWidget(maximise_);
    layout->addWidget(close);
    window_->installEventFilter(this);
}

bool TitleBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == window_ && event->type() == QEvent::WindowStateChange)
        maximise_->setRestore(window_->isMaximized());
    return QWidget::eventFilter(watched, event);
}

WindowButton::WindowButton(Kind kind, QWidget *parent) : QToolButton(parent), kind_(kind)
{
    static const char *names[] = {"minimiseButton", "maximiseButton", "closeButton"};
    setObjectName(QString::fromLatin1(names[kind]));
    setFixedSize(46, kHeight);
    setFocusPolicy(Qt::NoFocus);
    setToolTip(kind == Close ? tr("Close") : kind == Minimise ? tr("Minimise") : tr("Maximise"));
}

void WindowButton::paintEvent(QPaintEvent *event)
{
    // The style sheet paints the hover background; the glyph goes on top.
    QToolButton::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, kind_ == Close);
    QPen pen(palette().color(QPalette::ButtonText));
    pen.setWidthF(1.0);
    painter.setPen(pen);
    const QPointF centre(width() / 2.0, height() / 2.0);
    const qreal half = 5.0;
    switch (kind_) {
    case Minimise:
        painter.drawLine(QPointF(centre.x() - half, centre.y() + 0.5),
                         QPointF(centre.x() + half, centre.y() + 0.5));
        break;
    case Maximise:
        if (restore_) {
            QRectF back(centre.x() - half + 2, centre.y() - half, 2 * half - 2, 2 * half - 2);
            QRectF front(centre.x() - half, centre.y() - half + 2, 2 * half - 2, 2 * half - 2);
            painter.drawLine(back.topLeft(), back.topRight());
            painter.drawLine(back.topRight(), back.bottomRight());
            painter.drawLine(back.topLeft(), QPointF(back.left(), front.top()));
            painter.drawLine(back.bottomRight(), QPointF(front.right(), back.bottom()));
            painter.drawRect(front);
        } else {
            painter.drawRect(QRectF(centre.x() - half, centre.y() - half, 2 * half, 2 * half));
        }
        break;
    case Close:
        painter.drawLine(QPointF(centre.x() - half, centre.y() - half),
                         QPointF(centre.x() + half, centre.y() + half));
        painter.drawLine(QPointF(centre.x() - half, centre.y() + half),
                         QPointF(centre.x() + half, centre.y() - half));
        break;
    }
}

void TitleBar::setTitle(const QString &title)
{
    title_->setText(title);
}

void TitleBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && window_->windowHandle())
        window_->windowHandle()->startSystemMove();
    QWidget::mousePressEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        toggleMaximised();
}

void TitleBar::toggleMaximised()
{
    if (window_->isMaximized())
        window_->showNormal();
    else
        window_->showMaximized();
}

} // namespace astral::gui
