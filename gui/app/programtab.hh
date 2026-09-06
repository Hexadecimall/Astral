// One open program inside the workspace: its document, the models the side
// panes bind to, and the decompiler view in the centre.
#ifndef ASTRAL_GUI_PROGRAMTAB_HH
#define ASTRAL_GUI_PROGRAMTAB_HH

#include "model/programdocument.hh"

#include <QWidget>

class QMenu;
class QStackedWidget;

#include <memory>

namespace astral::gui {

class DecompilerView;
class HexView;
class HexPane;
class FunctionListModel;
class ListingPane;
class ListingView;

class ProgramTab : public QWidget {
    Q_OBJECT
public:
    explicit ProgramTab(std::unique_ptr<ProgramDocument> document, QWidget *parent = nullptr);
    ~ProgramTab() override;

    ProgramDocument *document() const { return document_.get(); }
    FunctionListModel *functionModel() const { return functions_; }
    quint64 currentAddress() const { return current_; }
    QString listing() const { return listing_; }

    // What the centre pane is showing. The first five are the same program
    // said five ways and share one tab; Graph is a picture of it and keeps a
    // tab of its own.
    enum View { Nova, Code, Assembly, PseudoC, Hex, Graph };
    void setView(View view);
    View view() const;
    // Whether a view is one of the readings that share the source tab.
    static bool isSource(View view) { return view != Graph; }
    // What to call a view in a menu or on a tab.
    static QString viewName(View view);

    void showFunction(quint64 address);
    // A function address opens in Code; anything else opens in Hex.
    // Goes to the address. False when the image does not map it, which is
    // the caller's to report; nothing in the view changes.
    bool showAddress(quint64 address);
    void refreshHex();
    // Accepts a hex address, with or without 0x, or a function name.
    bool navigateTo(const QString &target);

    void compileCurrent(DecompilerView *view = nullptr);
    void replaceCodeText(const QString &text);
    // The word the cursor sits in, in whichever view is showing.
    QString currentWord() const;
    // Re-reads the current function from the engine, discarding what the view
    // holds. Used after a patch, when the old text describes code that is gone.
    void refreshCurrent();
    void reportPatchWritten();
    void reportPatchFailed(const QString &reason);
Q_SIGNALS:
    void viewChanged(int index);
    // A menu wants the actions that apply to `word`. `line` is the whole line
    // the cursor sat in, which is one of the things the menu can copy.
    void contextActionsWanted(QMenu *menu, const QString &word, const QString &line);
    void logMessage(const QString &line);
    // A patch landed in the engine queue and is ready to write out.
    void patchApplied();
    // The current function changed; the window updates panes bound to it.
    void locationChanged(quint64 address, const QString &name);
    void listingChanged(const QString &listing);

private:
    std::unique_ptr<ProgramDocument> document_;
    FunctionListModel *functions_;
    DecompilerView *decompiler_;
    DecompilerView *pseudo_;
    // The disassembly, shown in the centre when the source tab is on
    // Assembly. The dock on the right keeps its own. The view is declared
    // first because the pane is built around it.
    ListingView *centreListingView_;
    ListingPane *centreListing_;
    HexView *hex_;
    HexPane *hexPane_;
    QStackedWidget *views_;
    quint64 current_ = 0;
    quint64 hexAddress_ = 0;
    bool refreshPending_ = false;
    QString listing_;
    // Which reading is showing. Nova and Pseudo-C are the same widget told to
    // say it differently, so the stack's index cannot answer this.
    View view_ = Nova;
};

} // namespace astral::gui

#endif
