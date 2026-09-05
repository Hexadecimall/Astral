// The screen shown before anything is open: the ways to start, recent
// projects, and what changed in this version.
#ifndef ASTRAL_GUI_WELCOMEPAGE_HH
#define ASTRAL_GUI_WELCOMEPAGE_HH

#include <QFrame>
#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace astral::gui {

// A bordered, hoverable block that acts as one large button.
class Card : public QFrame {
    Q_OBJECT
public:
    Card(const QString &glyph, const QString &title, const QString &detail, QWidget *parent = nullptr);

Q_SIGNALS:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
};

class WelcomePage : public QWidget {
    Q_OBJECT
public:
    explicit WelcomePage(QWidget *parent = nullptr);

    // Reloads the recent list from settings; call after a project opens.
    void refresh();

    static void rememberRecent(const QString &path);
    static QStringList recentPaths();

Q_SIGNALS:
    void newProjectRequested();
    void openRequested();
    void recentRequested(const QString &path);

private:
    QWidget *recentBox_ = nullptr;
    QVBoxLayout *recentLayout_ = nullptr;
    QLabel *recentHeading_ = nullptr;
};

} // namespace astral::gui

#endif
