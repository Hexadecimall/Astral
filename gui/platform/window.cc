#include "platform/window.hh"

#include <QApplication>
#include <QMouseEvent>
#include <QPainterPath>
#include <QWidget>
#include <QWindow>

namespace astral::gui::platform {

namespace {
constexpr int kGrip = 6;
}

void adoptCustomTitleBar(QWidget *window)
{
    window->setWindowFlags(window->windowFlags() | Qt::FramelessWindowHint);
    window->setAttribute(Qt::WA_TranslucentBackground, true);
    new EdgeResizer(window);
}

#if !defined(Q_OS_MACOS)
void applyWindowShape(QWidget *window, int radius)
{
    // A mask is the portable way; its edge is not antialiased, which is the
    // price for having no compositor contract on these platforms.
    if (radius <= 0) {
        window->clearMask();
        return;
    }
    QPainterPath path;
    path.addRoundedRect(QRectF(window->rect()), radius, radius);
    window->setMask(QRegion(path.toFillPolygon().toPolygon()));
}
#endif

EdgeResizer::EdgeResizer(QWidget *window) : QObject(window), window_(window)
{
    qApp->installEventFilter(this);
}

Qt::Edges EdgeResizer::edgesAt(const QPoint &globalPos) const
{
    if (window_->isMaximized() || window_->isFullScreen())
        return {};
    const QRect frame = window_->frameGeometry();
    if (!frame.adjusted(-kGrip, -kGrip, kGrip, kGrip).contains(globalPos))
        return {};
    Qt::Edges edges;
    if (globalPos.x() <= frame.left() + kGrip)
        edges |= Qt::LeftEdge;
    if (globalPos.x() >= frame.right() - kGrip)
        edges |= Qt::RightEdge;
    if (globalPos.y() <= frame.top() + kGrip)
        edges |= Qt::TopEdge;
    if (globalPos.y() >= frame.bottom() - kGrip)
        edges |= Qt::BottomEdge;
    return edges;
}

bool EdgeResizer::eventFilter(QObject *watched, QEvent *event)
{
    // Only events for widgets inside this window matter.
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || widget->window() != window_)
        return false;

    if (event->type() == QEvent::MouseMove) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        const Qt::Edges edges = edgesAt(mouse->globalPosition().toPoint());
        if (!edges) {
            if (window_->property("edgeCursor").toBool()) {
                window_->unsetCursor();
                window_->setProperty("edgeCursor", false);
            }
            return false;
        }
        Qt::CursorShape shape = Qt::ArrowCursor;
        const bool horizontal = edges & (Qt::LeftEdge | Qt::RightEdge);
        const bool vertical = edges & (Qt::TopEdge | Qt::BottomEdge);
        if (horizontal && vertical) {
            const bool nwse = (edges == (Qt::LeftEdge | Qt::TopEdge))
                              || (edges == (Qt::RightEdge | Qt::BottomEdge));
            shape = nwse ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
        } else if (horizontal) {
            shape = Qt::SizeHorCursor;
        } else {
            shape = Qt::SizeVerCursor;
        }
        window_->setCursor(shape);
        window_->setProperty("edgeCursor", true);
        return false;
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton)
            return false;
        const Qt::Edges edges = edgesAt(mouse->globalPosition().toPoint());
        if (!edges || !window_->windowHandle())
            return false;
        window_->windowHandle()->startSystemResize(edges);
        return true;
    }
    return false;
}

} // namespace astral::gui::platform
