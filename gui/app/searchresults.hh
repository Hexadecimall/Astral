// The list of matches that drops under the search box. It sits over the
// window rather than in a pane, so the results are where the typing is and
// nothing has to be docked to see them.
#ifndef ASTRAL_GUI_SEARCHRESULTS_HH
#define ASTRAL_GUI_SEARCHRESULTS_HH

#include <QFrame>

#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;

namespace astral::gui {

class SearchResults : public QFrame {
    Q_OBJECT
public:
    struct Match {
        QString kind;
        QString name;
        quint64 address = 0;
    };

    // `box` is the field the list hangs under; the list follows it and reads
    // its arrow keys.
    SearchResults(QLineEdit *box, QWidget *parent);

    void showMatches(const std::vector<Match> &matches, const QString &needle);
    void hideMatches();
    bool isShowing() const;

Q_SIGNALS:
    void chosen(quint64 address);

protected:
    // The box keeps focus while the arrows move the selection here.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void reposition();
    void chooseCurrent();
    void step(int delta);

    QLineEdit *box_;
    QListWidget *list_;
    QLabel *summary_;
};

} // namespace astral::gui

#endif
