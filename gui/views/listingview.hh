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

    // The dock that holds this shows Nova when the centre pane has taken the
    // disassembly. Colouring source with the assembly rules would be
    // confidently wrong, so the rules follow what is in it.
    void setShowingSource(bool source);

Q_SIGNALS:
    void navigateRequested(quint64 address);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    QSyntaxHighlighter *highlighter_ = nullptr;
    bool showingSource_ = false;

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
};

} // namespace astral::gui

#endif
