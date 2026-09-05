// A window that draws its own title bar. The frame is dropped on every
// platform and the title bar widget supplies the controls, so the
// application looks the same everywhere.
#ifndef ASTRAL_GUI_PLATFORM_WINDOW_HH
#define ASTRAL_GUI_PLATFORM_WINDOW_HH

#include <QObject>

class QWidget;

namespace astral::gui::platform {

// Prepares a top-level widget for a custom title bar. Call before show().
void adoptCustomTitleBar(QWidget *window);

// Rounds the window's corners by `radius` pixels, or squares them with zero.
// Called again whenever the window's size or state changes.
void applyWindowShape(QWidget *window, int radius);

// Corner radius the window uses when it is not maximised.
constexpr int kCornerRadius = 10;

// Resizes a frameless window from its edges: watches mouse events across the
// whole window, shows the right cursor near an edge, and hands the drag to
// the platform's own resize.
class EdgeResizer : public QObject {
    Q_OBJECT
public:
    explicit EdgeResizer(QWidget *window);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Qt::Edges edgesAt(const QPoint &globalPos) const;

    QWidget *window_;
};

} // namespace astral::gui::platform

#endif
