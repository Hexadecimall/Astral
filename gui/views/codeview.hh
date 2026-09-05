// Read-only monospace text with a line-number gutter and a highlighted current
// line. The decompiler and listing views build on it.
#ifndef ASTRAL_GUI_CODEVIEW_HH
#define ASTRAL_GUI_CODEVIEW_HH

#include <QPlainTextEdit>
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
    // For listings: the address at the start of the line under `pos`, if any.
    std::optional<quint64> addressAtLine(int blockNumber) const;
    std::optional<quint64> addressAtCursor() const;

    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateGutterWidth();
    void updateGutter(const QRect &rect, int dy);
    void highlightCurrentLine();

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
