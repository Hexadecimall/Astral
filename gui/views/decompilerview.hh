// The centre pane: one function's recovered C with a header naming it.
#ifndef ASTRAL_GUI_DECOMPILERVIEW_HH
#define ASTRAL_GUI_DECOMPILERVIEW_HH

#include "model/programdocument.hh"

#include <QWidget>

class QLabel;
class QTimer;
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
    CodeView *codeView() const { return code_; }
    QString text() const;
    void setText(const QString &text);
    bool modified() const;
    // Reports the result of a compile check in the header.
    void showCompileResult(bool ok, int errors);
    void showPatching();
    // The patch is in the engine's queue but no file has changed yet.
    void showPatchQueued();
    // The patched binary reached the disk.
    void showPatchWritten();
    void showRefused(const QString &reason);
    void setStatus(const QString &text, const QString &kind, const QString &tip = QString());

Q_SIGNALS:
    void compileRequested(const QString &source);
    void modifiedChanged(bool modified);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void render();
    void setBusy(bool busy, const QString &verb = QString());
    QString busyVerb_;

    Decompiled current_;
    bool pseudo_ = false;
    QToolButton *compile_ = nullptr;
    QToolButton *revert_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *busy_ = nullptr;
    QTimer *busyTimer_ = nullptr;
    int busyTick_ = 0;
    QLabel *header_;
    QLabel *detail_;
    CodeView *code_;
};

} // namespace astral::gui

#endif
