// The hex view with the controls that turn editing on and queue what changed.
#ifndef ASTRAL_GUI_HEXPANE_HH
#define ASTRAL_GUI_HEXPANE_HH

#include <QWidget>

class QLabel;
class QPushButton;

namespace astral::gui {

class HexView;
class ProgramDocument;

class HexPane : public QWidget {
    Q_OBJECT
public:
    // Takes the view it wraps, so whoever owns the view keeps it.
    explicit HexPane(HexView *view, QWidget *parent = nullptr);

    HexView *view() const { return view_; }
    void setDocument(ProgramDocument *document);

Q_SIGNALS:
    void logMessage(const QString &line);
    void patchApplied();

private:
    void apply();
    void updateButtons();

    HexView *view_;
    QPushButton *editButton_;
    QPushButton *applyButton_;
    QPushButton *revertButton_;
    QLabel *status_;
    ProgramDocument *document_ = nullptr;
};

} // namespace astral::gui

#endif
