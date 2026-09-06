// Read-only monospace text with a line-number gutter and a highlighted current
// line. The decompiler and listing views build on it.
#ifndef ASTRAL_GUI_CODEVIEW_HH
#define ASTRAL_GUI_CODEVIEW_HH

#include <QPlainTextEdit>
#include <QStringList>

class QCompleter;
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
    // What is being edited, which decides the words offered and how a line is
    // indented. None is a document with no language, where the editing help is
    // limited to matching brackets and keeping the indent.
    enum class Language { None, C, Nova, Assembly };

    explicit CodeView(QWidget *parent = nullptr);

    void setEditable(bool editable);
    // Which language this view holds. Sets the words offered on completion.
    void setLanguage(Language language);
    Language language() const { return language_; }
    // Names worth offering that the language does not know: the program's own
    // symbols, and whatever the caller thinks belongs here. Kept apart from
    // the language's own words so changing one does not disturb the other.
    void setKnownNames(const QStringList &names);
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
    // Puts the line holding this address in the middle of the view.
    void scrollToAddress(quint64 address);

Q_SIGNALS:
    // The gutter was clicked on this line, which is where a breakpoint is set.
    void gutterClicked(int blockNumber);
    // Raised before the context menu is shown, so whoever owns the view can
    // add what it can do with `word` at the top of the menu.
    void contextMenuAboutToShow(QMenu *menu, const QString &word);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;

private:
    void updateGutterWidth();
    void updateGutter(const QRect &rect, int dy);
    void highlightCurrentLine();

    // The editing help, each answering whether it handled the key.
    bool closeBracket(QKeyEvent *event);
    bool stepOverClose(QKeyEvent *event);
    bool openLine(QKeyEvent *event);
    bool outdentClose(QKeyEvent *event);
    bool eraseEmptyPair(QKeyEvent *event);
    bool indentBySpaces(QKeyEvent *event);
    void showCompletions();
    void insertCompletion(const QString &completion);
    QString prefixUnderCursor() const;
    QStringList completionWords() const;

    // Addresses to mark in the gutter, and which one is executing.
    std::vector<quint64> marks_;
    quint64 current_ = 0;
    QWidget *gutter_;
    Language language_ = Language::None;
    QStringList knownNames_;
    QCompleter *completer_ = nullptr;
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

// Colours Nova: the language the decompiler writes for a person to read.
// Nova shares C's brackets and almost none of its spellings, so it gets rules
// of its own rather than a few extra words bolted onto the C ones.
class NovaHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit NovaHighlighter(QTextDocument *document);

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
