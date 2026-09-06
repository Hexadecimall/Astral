// A terminal: a screen of cells, what a program writing to it means, and the
// keys that go back the other way.
//
// This is a real terminal rather than a log with colours in it. A shell draws
// its own line editor, moves the cursor about, repaints in place and clears
// what it no longer wants, and none of that reads as anything unless the
// sequences are actually carried out. So there is a screen here, and what
// arrives is applied to it.
//
// It is not every terminal ever made. What it answers to is what a shell and
// the programs a person runs from one actually use.
#ifndef ASTRAL_GUI_TERMINALVIEW_HH
#define ASTRAL_GUI_TERMINALVIEW_HH

#include <QAbstractScrollArea>
#include <QByteArray>
#include <QColor>
#include <QString>
#include <QVector>

namespace astral::gui {

class Pty;

class TerminalView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit TerminalView(QWidget *parent = nullptr);

    // Starts a shell. `directory` is where it begins, `extraPath` goes in
    // front of PATH so Astral's own command is the one found first, and
    // `startup` is run once the shell is listening. All three are kept, so a
    // shell that ends can be replaced by the same one.
    void startSession(const QString &directory, const QString &extraPath,
                      const QString &startup = QString());
    // Starts another shell exactly like the last, keeping what is on screen.
    void restart();
    bool running() const;
    // Types text as though it had been typed, ending with a newline.
    void send(const QString &text);
    // Types `text` once the shell has actually shown something. A line typed
    // before its line editor is up is thrown away, so anything Astral wants
    // to run at the start has to wait for a sign of life.
    void sendWhenReady(const QString &text);
    void clearScreen();

Q_SIGNALS:
    void sessionEnded();

protected:
    // Every key belongs to whatever is running here, so the window's own
    // shortcuts are told to stand aside while this has the keyboard.
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private:
    // One place on the screen: what is written there and how it looks.
    struct Cell {
        QChar ch = QLatin1Char(' ');
        // What colour this is: -1 for the theme's own, 0 to 255 for a place in
        // the table every terminal shares, and anything larger for a colour
        // given outright, with the red, green and blue in the low three bytes.
        // One number rather than three fields, because there is one of these
        // for every place on the screen and every line kept behind it.
        int foreground = -1;
        int background = -1;
        bool bold = false;
        bool underline = false;
        bool inverse = false;
        bool faint = false;
    };
    using Row = QVector<Cell>;

    // --- what arrives ---
    void take(const QByteArray &bytes);
    void put(QChar ch);
    void control(char c);
    // Applies a complete escape sequence. `body` is what came between the
    // introducer and the final character.
    void csi(const QString &body, char final);
    void osc(const QString &body);
    void escape(char c);
    void selectGraphic(const QVector<int> &parameters);

    // --- the screen ---
    void reshape();
    void scrollUp(int lines);
    void scrollDown(int lines);
    void eraseInLine(int mode);
    void eraseInDisplay(int mode);
    void insertLines(int count);
    void deleteLines(int count);
    void deleteCharacters(int count);
    void insertCharacters(int count);
    void eraseCharacters(int count);
    void newline();
    Row blankRow() const;
    Cell blankCell() const;
    void useAlternateScreen(bool alternate);
    // Keeps the cursor inside the screen, and inside the scrolling region.
    void clampCursor();

    // --- painting and geometry ---
    QColor colourOf(int index, bool foreground, bool bold) const;
    int cellWidth() const { return cellWidth_; }
    int cellHeight() const { return cellHeight_; }
    void updateScrollBar();
    // Repaints when the scrollbar moves, which nothing else does for a view
    // that draws its own contents.
    void scrollContentsBy(int dx, int dy) override;
    // A colour written out in full, as one number.
    static int directColour(int r, int g, int b)
    {
        return 0x1000000 | ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
    }
    // The row of the whole history the top of the viewport shows.
    int topLine() const;
    // What the selection covers, as text.
    QString selectedText() const;
    void copySelection();
    // Writes a line of Astral's own into the screen, marked as not being the
    // program's output.
    void notice(const QString &text);
    // Where a point falls, in history rows and columns.
    void positionAt(const QPoint &point, int &line, int &column) const;

    Pty *pty_ = nullptr;

    // The screen, and what has scrolled off the top of it.
    QVector<Row> screen_;
    QVector<Row> history_;
    // The screen a full-window program is given, kept apart so leaving it
    // puts back exactly what was there before.
    QVector<Row> savedScreen_;
    bool alternate_ = false;
    int columns_ = 80;
    int rows_ = 24;
    int cursorRow_ = 0;
    int cursorColumn_ = 0;
    int savedRow_ = 0;
    int savedColumn_ = 0;
    // The rows a scroll is confined to, which is the whole screen unless a
    // program has said otherwise.
    int regionTop_ = 0;
    int regionBottom_ = 23;
    bool cursorVisible_ = true;
    bool wrapPending_ = false;
    bool applicationCursorKeys_ = false;
    bool bracketedPaste_ = false;

    Cell pen_;

    // --- the parser ---
    enum class State { Ground, Escape, Csi, Osc, OscEscape };
    State state_ = State::Ground;
    QString pending_;
    // A character that arrived split across two reads.
    QByteArray partial_;
    // Held until the far end says something, then typed.
    QString pendingCommand_;
    // What the last session was, so another can be started the same way.
    QString directory_;
    QString extraPath_;
    QString startup_;

    int cellWidth_ = 8;
    int cellHeight_ = 16;
    int baseline_ = 12;
    bool focused_ = false;
    // Whether the view is following what arrives. It stops when the reader
    // scrolls back, or output would drag them away from what they are reading.
    bool following_ = true;
    // Where a drag started and where it is now, in history rows and columns.
    bool selecting_ = false;
    int anchorLine_ = -1;
    int anchorColumn_ = 0;
    int caretLine_ = -1;
    int caretColumn_ = 0;
};

} // namespace astral::gui

#endif
