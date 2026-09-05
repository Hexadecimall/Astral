// The window's own title bar: logo, inline menus, centred title and, where
// the platform does not supply them, the window buttons.
#ifndef ASTRAL_GUI_TITLEBAR_HH
#define ASTRAL_GUI_TITLEBAR_HH

#include <QToolButton>
#include <QWidget>

class QLabel;
class QMenuBar;
class QToolButton;

namespace astral::gui {

// Minimise, maximise and close, drawn with lines so no font is involved.
class WindowButton : public QToolButton {
    Q_OBJECT
public:
    enum Kind { Minimise, Maximise, Close };
    WindowButton(Kind kind, QWidget *parent);
    void setRestore(bool restore) { restore_ = restore; update(); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Kind kind_;
    bool restore_ = false;
};

class TitleBar : public QWidget {
    Q_OBJECT
public:
    explicit TitleBar(QWidget *window);

    QMenuBar *menuBar() const { return menuBar_; }
    void setTitle(const QString &title);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void toggleMaximised();

    QWidget *window_;
    QMenuBar *menuBar_ = nullptr;
    QLabel *title_ = nullptr;
    WindowButton *maximise_ = nullptr;
};

} // namespace astral::gui

#endif
