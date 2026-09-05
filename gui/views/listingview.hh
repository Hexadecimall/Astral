// Disassembly with the engine's patches on the right-click menu.
#ifndef ASTRAL_GUI_LISTINGVIEW_HH
#define ASTRAL_GUI_LISTINGVIEW_HH

#include "views/codeview.hh"

namespace astral::gui {

class ListingView : public CodeView {
    Q_OBJECT
public:
    explicit ListingView(QWidget *parent = nullptr);

Q_SIGNALS:
    void nopRequested(quint64 address, int count);
    void invertRequested(quint64 address);
    void returnRequested(quint64 address);
    void navigateRequested(quint64 address);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
};

} // namespace astral::gui

#endif
