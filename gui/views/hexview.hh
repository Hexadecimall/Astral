// Bytes around the current function: address, sixteen hex bytes, and the
// printable characters, with the function's own bytes marked.
#ifndef ASTRAL_GUI_HEXVIEW_HH
#define ASTRAL_GUI_HEXVIEW_HH

#include "views/codeview.hh"

namespace astral::gui {

class HexView : public CodeView {
    Q_OBJECT
public:
    explicit HexView(QWidget *parent = nullptr);

    // Shows `bytes`, which start at `base`, and highlights [mark, mark+markSize).
    void showBytes(quint64 base, const QByteArray &bytes, quint64 mark, quint64 markSize);

private:
    quint64 base_ = 0;
};

} // namespace astral::gui

#endif
