// Read-only monospace text with a line-number gutter and a highlighted current
// line. The decompiler and listing views build on it.
#ifndef ASTRAL_GUI_CODEVIEW_HH
#define ASTRAL_GUI_CODEVIEW_HH

#include <QPlainTextEdit>

class QMenu;
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include <optional>
#include <vector>

namespace astral::gui {

class CodeView : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeView(QWidget *parent = nullptr);

    void setEditable(bool editable);
    // The identifier or number the cursor sits in, empty when it sits in
    // whitespace or punctuation.
    QString wordUnderCursor() const;
    QString wordAt(const QPoint &pos) const;
    // For listings: the address at the start of the line under `pos`, if any.
    std::optional<quint64> addressAtLine(int blockNumber) const;
    std::optional<quint64> addressAtCursor() const;

    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);
    // Draws a dot beside each of these addresses, and an arrow at `current`.
    void setGutterMarks(const std::vector<quint64> &marks, quint64 current);

Q_SIGNALS:
    // The gutter was clicked on this line, which is where a breakpoint is set.
    void gutterClicked(int blockNumber);
    // Raised before the context menu is shown, so whoever owns the view can
    // add what it can do with `word` at the top of the menu.
    void contextMenuAboutToShow(QMenu *menu, const QString &word);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateGutterWidth();
    void updateGutter(const QRect &rect, int dy);
    void highlightCurrentLine();

    // Addresses to mark in the gutter, and which one is executing.
    std::vector<quint64> marks_;
    quint64 current_ = 0;
    QWidget *gutter_;
};

// Colours C the way the decompiler pane needs before tokens exist: keywords,
// types, numbers, strings, comments, calls.
class CHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit CHighlighter(QTextDocument *document);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    std::vector<Rule> rules_;
    QRegularExpression commentStart_;
    QRegularExpression commentEnd_;
    QTextCharFormat commentFormat_;
};

// Colours a disassembly listing: address, mnemonic, registers, numbers.
class AsmHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit AsmHighlighter(QTextDocument *document);

protected:
    void highlightBlock(const QString &text) override;

private:
    QTextCharFormat address_, mnemonic_, register_, number_, symbol_;
};

} // namespace astral::gui

#endif
