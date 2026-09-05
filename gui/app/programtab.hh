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

class ProgramTab : public QWidget {
    Q_OBJECT
public:
    explicit ProgramTab(std::unique_ptr<ProgramDocument> document, QWidget *parent = nullptr);
    ~ProgramTab() override;

    ProgramDocument *document() const { return document_.get(); }
    FunctionListModel *functionModel() const { return functions_; }
    quint64 currentAddress() const { return current_; }
    QString listing() const { return listing_; }

    enum View { Code, PseudoC, Graph, Hex };
    void setView(View view);
    View view() const;

    void showFunction(quint64 address);
    // A function address opens in Code; anything else opens in Hex.
    void showAddress(quint64 address);
    void refreshHex();
    // Accepts a hex address, with or without 0x, or a function name.
    bool navigateTo(const QString &target);

    void compileCurrent();
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
    // A menu wants the actions that apply to `word` at `pos`.
    void contextActionsWanted(QMenu *menu, const QString &word);
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
    HexView *hex_;
    HexPane *hexPane_;
    QStackedWidget *views_;
    quint64 current_ = 0;
    quint64 hexAddress_ = 0;
    bool refreshPending_ = false;
    QString listing_;
};

} // namespace astral::gui

#endif
