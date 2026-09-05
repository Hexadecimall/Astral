// Disassembly. The menu it raises is the window's, built from the line and
// the word the cursor sits in.
#ifndef ASTRAL_GUI_LISTINGVIEW_HH
#define ASTRAL_GUI_LISTINGVIEW_HH

#include "views/codeview.hh"

namespace astral::gui {

class ListingView : public CodeView {
    Q_OBJECT
public:
    explicit ListingView(QWidget *parent = nullptr);

Q_SIGNALS:
    void navigateRequested(quint64 address);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
};

} // namespace astral::gui

#endif
