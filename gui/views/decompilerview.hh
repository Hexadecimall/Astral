// The centre pane: one function's recovered C with a header naming it.
#ifndef ASTRAL_GUI_DECOMPILERVIEW_HH
#define ASTRAL_GUI_DECOMPILERVIEW_HH

#include "model/programdocument.hh"

#include <QWidget>

class QLabel;
class QToolButton;

namespace astral::gui {

class CodeView;

class DecompilerView : public QWidget {
    Q_OBJECT
public:
    explicit DecompilerView(QWidget *parent = nullptr);

    void showPending(const QString &name, quint64 address);
    void showFunction(const Decompiled &function);
    void showError(const QString &error);
    void showEmpty(const QString &message);
    // Whether the pane shows the engine's listing rather than compilable C.
    void setPseudo(bool pseudo);
    QString text() const;
    void setText(const QString &text);
    bool modified() const;
    // Reports the result of a compile check in the header.
    void showCompileResult(bool ok, int errors);
    void showPatched();
    void showRefused(const QString &reason);

Q_SIGNALS:
    void compileRequested(const QString &source);
    void modifiedChanged(bool modified);

private:
    void render();

    Decompiled current_;
    bool pseudo_ = false;
    QToolButton *compile_ = nullptr;
    QToolButton *revert_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *header_;
    QLabel *detail_;
    CodeView *code_;
};

} // namespace astral::gui

#endif
