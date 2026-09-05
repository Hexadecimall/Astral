// The listing dock: the disassembly, plus the controls that let it be edited
// and assembled back into the program.
#ifndef ASTRAL_GUI_LISTINGPANE_HH
#define ASTRAL_GUI_LISTINGPANE_HH

#include <QWidget>

class QLabel;
class QPushButton;

namespace astral::gui {

class ListingView;
class ProgramDocument;

class ListingPane : public QWidget {
    Q_OBJECT
public:
    // Takes the view it wraps, so whoever owns the view keeps its signals.
    explicit ListingPane(ListingView *view, QWidget *parent = nullptr);

    ListingView *view() const { return view_; }
    // Drives the assemble action from a scripted run.
    void assembleForTesting() { editing_ = true; assemble(); }
    // Replaces the disassembly and leaves edit mode; the old text describes
    // code the program may no longer hold.
    void setListing(const QString &text);
    // The program the listing came from and the address it starts at.
    void setProgram(ProgramDocument *document, quint64 address);

Q_SIGNALS:
    void logMessage(const QString &line);
    // Bytes reached the engine's patch queue.
    void patchApplied();

private:
    void setEditing(bool editing);
    void assemble();
    // Astral's own assembler, one changed line at a time.
    void assembleWithEngine();
    // The bytes the untouched listing covers, measured from its own addresses.
    quint64 span() const;
    void updateButtons();

    ListingView *view_;
    QPushButton *editButton_;
    QPushButton *assembleButton_;
    QPushButton *revertButton_;
    QLabel *status_;
    ProgramDocument *document_ = nullptr;
    quint64 address_ = 0;
    QString pristine_;
    bool editing_ = false;
    bool busy_ = false;
};

} // namespace astral::gui

#endif
