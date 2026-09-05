// Bytes around the current function: address, sixteen hex bytes, and the
// printable characters, with the function's own bytes marked. Editable in
// place: typing a hex digit over a nibble changes that byte and nothing else,
// so the layout the view depends on always survives.
#ifndef ASTRAL_GUI_HEXVIEW_HH
#define ASTRAL_GUI_HEXVIEW_HH

#include "views/codeview.hh"

#include <QMap>

#include <utility>

namespace astral::gui {

class HexView : public CodeView {
    Q_OBJECT
public:
    explicit HexView(QWidget *parent = nullptr);

    // Shows `bytes`, which start at `base`, and highlights [mark, mark+markSize).
    void showBytes(quint64 base, const QByteArray &bytes, quint64 mark, quint64 markSize);

    // In editing mode a hex digit typed over a nibble rewrites that byte.
    void setEditing(bool editing);
    bool editing() const { return editing_; }

    // Bytes changed but not yet queued, coalesced into contiguous runs.
    std::vector<std::pair<quint64, QByteArray>> dirtyRuns() const;
    int dirtyCount() const { return static_cast<int>(dirty_.size()); }
    // Puts the shown bytes back to what was read.
    void revert();
    // Forgets the edits without changing what is shown; used once they are
    // queued and the view is about to be re-read.
    void clearEdits();

    // The column a byte's first nibble sits in, for a line of sixteen.
    static int hexColumn(int byte);
    static int asciiColumn(int byte);

Q_SIGNALS:
    void editsChanged(int count);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void render();
    void applySelections();
    // The byte index in the whole buffer under the cursor, and which nibble of
    // it, or -1 when the cursor is not on a hex digit.
    qsizetype byteUnderCursor(int *nibble) const;
    void setByte(qsizetype index, unsigned char value);

    quint64 base_ = 0;
    QByteArray bytes_;
    QByteArray original_;
    quint64 mark_ = 0;
    quint64 markSize_ = 0;
    // index into bytes_ -> the value read from the program
    QMap<qsizetype, unsigned char> dirty_;
    bool editing_ = false;
};

} // namespace astral::gui

#endif
