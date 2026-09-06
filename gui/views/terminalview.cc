#include "views/terminalview.hh"

#include "model/pty.hh"
#include "model/settings.hh"
#include "theme/theme.hh"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>

#include <algorithm>

namespace astral::gui {

namespace {

// How much of what has scrolled away is kept. Enough to look back over a long
// build; not so much that a runaway program eats the machine.
constexpr int kScrollback = 5000;

// The colour a number in an escape sequence stands for. The first sixteen come
// from the theme so a terminal looks like the rest of the window; the rest are
// the cube and the greys every terminal agrees about.
QColor indexedColour(int index)
{
    // A colour the program gave in full rather than by number.
    if (index >= 0x1000000)
        return QColor(QRgb(index & 0xffffff) | 0xff000000u);
    const Theme &theme = Theme::current();
    static const char *const names[16] = {
        "term.black",   "term.red",     "term.green",   "term.yellow",
        "term.blue",    "term.magenta", "term.cyan",    "term.white",
        "term.brightBlack", "term.brightRed", "term.brightGreen", "term.brightYellow",
        "term.brightBlue", "term.brightMagenta", "term.brightCyan", "term.brightWhite",
    };
    if (index >= 0 && index < 16) {
        const QColor c = theme.colour(QString::fromLatin1(names[index]));
        if (c.isValid())
            return c;
    }
    if (index >= 16 && index < 232) {
        const int n = index - 16;
        static const int steps[6] = {0, 95, 135, 175, 215, 255};
        return QColor(steps[(n / 36) % 6], steps[(n / 6) % 6], steps[n % 6]);
    }
    if (index >= 232 && index < 256) {
        const int grey = 8 + (index - 232) * 10;
        return QColor(grey, grey, grey);
    }
    return QColor();
}

QVector<int> numbers(const QString &body)
{
    QVector<int> out;
    if (body.isEmpty())
        return out;
    for (const QString &piece : body.split(QLatin1Char(';'))) {
        bool ok = false;
        const int value = piece.toInt(&ok);
        out.push_back(ok ? value : 0);
    }
    return out;
}

int parameter(const QVector<int> &values, int at, int fallback)
{
    if (at >= values.size() || values[at] == 0)
        return fallback;
    return values[at];
}

} // namespace

TerminalView::TerminalView(QWidget *parent) : QAbstractScrollArea(parent)
{
    // A prompt that draws bars and gauges is written in glyphs only some
    // fonts carry, and picking one the machine does not have leaves a row of
    // empty boxes. The setting decides; failing that, the fonts that do carry
    // them are looked for by name, and the system's own is the last resort.
    const Settings &settings = Settings::instance();
    QFont mono;
    const QString wanted = settings.stringValue(QStringLiteral("terminal.font"));
    const QStringList families = QFontDatabase::families();
    QString chosen;
    if (!wanted.isEmpty() && families.contains(wanted, Qt::CaseInsensitive)) {
        chosen = wanted;
    } else {
        for (const QString &name : {QStringLiteral("JetBrainsMono Nerd Font Mono"),
                                    QStringLiteral("JetBrainsMono Nerd Font"),
                                    QStringLiteral("MesloLGS NF"),
                                    QStringLiteral("Hack Nerd Font Mono"),
                                    QStringLiteral("FiraCode Nerd Font Mono"),
                                    QStringLiteral("SauceCodePro Nerd Font Mono")}) {
            if (families.contains(name, Qt::CaseInsensitive)) {
                chosen = name;
                break;
            }
        }
    }
    if (chosen.isEmpty()) {
        mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setPointSizeF(mono.pointSizeF() + 0.5);
    } else {
        mono = QFont(chosen);
        mono.setPointSizeF(settings.intValue(QStringLiteral("terminal.fontSize"), 12));
        mono.setFixedPitch(true);
    }
    mono.setStyleHint(QFont::Monospace);
    setFont(mono);
    const QFontMetricsF metrics(mono);
    cellWidth_ = std::max(1, qRound(metrics.horizontalAdvance(QLatin1Char('M'))));
    cellHeight_ = std::max(1, qRound(metrics.height()));
    baseline_ = qRound(metrics.ascent());

    viewport()->setCursor(Qt::IBeamCursor);
    setFocusPolicy(Qt::StrongFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    screen_.resize(rows_);
    for (Row &row : screen_)
        row = blankRow();
    regionBottom_ = rows_ - 1;

    pty_ = new Pty(this);
    connect(pty_, &Pty::output, this, [this](const QByteArray &bytes) {
        take(bytes);
        // The far end has spoken, so its line editor is listening.
        if (!pendingCommand_.isEmpty()) {
            const QString command = pendingCommand_;
            pendingCommand_.clear();
            send(command);
        }
        // What arrives puts the view back at the bottom, which is where
        // anything being typed is - unless the reader has scrolled back, in
        // which case dragging them away from what they are reading is the
        // last thing to do.
        if (following_)
            verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        viewport()->update();
    });
    connect(pty_, &Pty::finished, this, [this] {
        // A shell that ends should not leave a dead pane behind. What it said
        // stays on the screen, and a keypress starts another.
        notice(tr("[the shell ended - press Enter for another]"));
        viewport()->update();
        Q_EMIT sessionEnded();
    });
}

// ------------------------------------------------------------------ session

void TerminalView::startSession(const QString &directory, const QString &extraPath,
                                const QString &startup)
{
    directory_ = directory;
    extraPath_ = extraPath;
    startup_ = startup;
    QString error;
    pty_->resize(columns_, rows_);
    if (!pty_->start(directory, extraPath, error)) {
        notice(error);
        viewport()->update();
        return;
    }
    if (!startup.isEmpty())
        sendWhenReady(startup);
}

void TerminalView::restart()
{
    if (running())
        return;
    // The cursor goes to a fresh line so the new shell's first prompt does not
    // land on top of the notice.
    cursorColumn_ = 0;
    newline();
    startSession(directory_, extraPath_, startup_);
    viewport()->update();
}

// Astral's own words, told apart from the program's by being dim.
void TerminalView::notice(const QString &text)
{
    const Cell had = pen_;
    if (cursorColumn_ != 0) {
        cursorColumn_ = 0;
        newline();
    }
    pen_ = Cell();
    pen_.faint = true;
    for (QChar c : text)
        put(c);
    cursorColumn_ = 0;
    newline();
    pen_ = had;
}

bool TerminalView::running() const
{
    return pty_ != nullptr && pty_->running();
}

void TerminalView::send(const QString &text)
{
    if (pty_ != nullptr)
        pty_->write(text.toLocal8Bit() + "\n");
}

void TerminalView::sendWhenReady(const QString &text)
{
    pendingCommand_ = text;
}

void TerminalView::clearScreen()
{
    history_.clear();
    for (Row &row : screen_)
        row = blankRow();
    cursorRow_ = 0;
    cursorColumn_ = 0;
    updateScrollBar();
    viewport()->update();
}

// ------------------------------------------------------------------ screen

TerminalView::Cell TerminalView::blankCell() const
{
    Cell cell;
    cell.background = pen_.background;
    return cell;
}

TerminalView::Row TerminalView::blankRow() const
{
    Row row;
    row.resize(columns_);
    for (Cell &cell : row)
        cell = Cell();
    return row;
}

void TerminalView::reshape()
{
    const int width = viewport()->width();
    const int height = viewport()->height();
    const int columns = std::max(2, width / cellWidth_);
    const int rows = std::max(1, height / cellHeight_);
    if (columns == columns_ && rows == rows_)
        return;

    columns_ = columns;
    const int had = rows_;
    rows_ = rows;

    // Growing takes lines back off the history so what was said stays where
    // it was; shrinking pushes the top of the screen into it.
    if (rows_ > had) {
        for (int i = had; i < rows_; ++i) {
            if (!history_.isEmpty() && cursorRow_ >= 0) {
                screen_.prepend(history_.takeLast());
                ++cursorRow_;
            } else {
                screen_.append(blankRow());
            }
        }
    } else if (rows_ < had) {
        for (int i = rows_; i < had; ++i) {
            if (cursorRow_ > 0) {
                history_.append(screen_.takeFirst());
                --cursorRow_;
            } else {
                screen_.removeLast();
            }
        }
    }
    screen_.resize(rows_);
    for (Row &row : screen_) {
        const int had_columns = row.size();
        row.resize(columns_);
        for (int i = had_columns; i < columns_; ++i)
            row[i] = Cell();
    }
    regionTop_ = 0;
    regionBottom_ = rows_ - 1;
    clampCursor();
    while (history_.size() > kScrollback)
        history_.removeFirst();
    updateScrollBar();
    if (pty_ != nullptr)
        pty_->resize(columns_, rows_);
}

void TerminalView::clampCursor()
{
    cursorRow_ = std::clamp(cursorRow_, 0, rows_ - 1);
    cursorColumn_ = std::clamp(cursorColumn_, 0, columns_ - 1);
}

void TerminalView::scrollUp(int lines)
{
    for (int i = 0; i < lines; ++i) {
        // Only what leaves the top of the whole screen is worth keeping; a
        // program scrolling inside a region of it is repainting, not saying
        // something new.
        if (regionTop_ == 0 && !alternate_)
            history_.append(screen_[regionTop_]);
        screen_.removeAt(regionTop_);
        screen_.insert(regionBottom_, blankRow());
    }
    while (history_.size() > kScrollback)
        history_.removeFirst();
    updateScrollBar();
}

void TerminalView::scrollDown(int lines)
{
    for (int i = 0; i < lines; ++i) {
        screen_.removeAt(regionBottom_);
        screen_.insert(regionTop_, blankRow());
    }
}

void TerminalView::newline()
{
    if (cursorRow_ >= regionBottom_)
        scrollUp(1);
    else
        ++cursorRow_;
}

void TerminalView::eraseInLine(int mode)
{
    Row &row = screen_[cursorRow_];
    const int from = mode == 0 ? cursorColumn_ : 0;
    const int to = mode == 1 ? cursorColumn_ + 1 : columns_;
    for (int i = std::max(0, from); i < std::min(columns_, to); ++i)
        row[i] = blankCell();
}

void TerminalView::eraseInDisplay(int mode)
{
    if (mode == 2 || mode == 3) {
        for (Row &row : screen_)
            for (Cell &cell : row)
                cell = blankCell();
        if (mode == 3)
            history_.clear();
        updateScrollBar();
        return;
    }
    if (mode == 0) {
        eraseInLine(0);
        for (int r = cursorRow_ + 1; r < rows_; ++r)
            for (Cell &cell : screen_[r])
                cell = blankCell();
        return;
    }
    eraseInLine(1);
    for (int r = 0; r < cursorRow_; ++r)
        for (Cell &cell : screen_[r])
            cell = blankCell();
}

void TerminalView::insertLines(int count)
{
    for (int i = 0; i < count && cursorRow_ <= regionBottom_; ++i) {
        screen_.removeAt(regionBottom_);
        screen_.insert(cursorRow_, blankRow());
    }
}

void TerminalView::deleteLines(int count)
{
    for (int i = 0; i < count && cursorRow_ <= regionBottom_; ++i) {
        screen_.removeAt(cursorRow_);
        screen_.insert(regionBottom_, blankRow());
    }
}

void TerminalView::deleteCharacters(int count)
{
    Row &row = screen_[cursorRow_];
    for (int i = 0; i < count; ++i) {
        row.removeAt(cursorColumn_);
        row.append(blankCell());
    }
}

void TerminalView::insertCharacters(int count)
{
    Row &row = screen_[cursorRow_];
    for (int i = 0; i < count; ++i) {
        row.insert(cursorColumn_, blankCell());
        row.removeLast();
    }
}

void TerminalView::eraseCharacters(int count)
{
    Row &row = screen_[cursorRow_];
    for (int i = 0; i < count && cursorColumn_ + i < columns_; ++i)
        row[cursorColumn_ + i] = blankCell();
}

void TerminalView::useAlternateScreen(bool alternate)
{
    if (alternate == alternate_)
        return;
    alternate_ = alternate;
    if (alternate) {
        savedScreen_ = screen_;
        for (Row &row : screen_)
            row = blankRow();
        cursorRow_ = 0;
        cursorColumn_ = 0;
    } else {
        if (savedScreen_.size() == screen_.size())
            screen_ = savedScreen_;
        savedScreen_.clear();
    }
    updateScrollBar();
}

// ------------------------------------------------------------------ parser

void TerminalView::take(const QByteArray &bytes)
{
    // A character may be split across two reads, so what cannot be decoded yet
    // is kept until the rest of it turns up.
    QByteArray all = partial_ + bytes;
    partial_.clear();

    int at = 0;
    while (at < all.size()) {
        const unsigned char b = static_cast<unsigned char>(all[at]);
        if (state_ != State::Ground || b < 0x80) {
            ++at;
            const char c = static_cast<char>(b);
            switch (state_) {
            case State::Ground:
                if (b < 0x20 || b == 0x7f)
                    control(c);
                else
                    put(QLatin1Char(c));
                break;
            case State::Escape:
                if (c == '[') {
                    state_ = State::Csi;
                    pending_.clear();
                } else if (c == ']') {
                    state_ = State::Osc;
                    pending_.clear();
                } else {
                    escape(c);
                    state_ = State::Ground;
                }
                break;
            case State::Csi:
                // Parameters and modifiers come first, then one final byte.
                if (c >= 0x40 && c <= 0x7e) {
                    csi(pending_, c);
                    state_ = State::Ground;
                } else {
                    pending_.push_back(QLatin1Char(c));
                    if (pending_.size() > 64)
                        state_ = State::Ground;
                }
                break;
            case State::Osc:
                if (c == 0x07) {
                    osc(pending_);
                    state_ = State::Ground;
                } else if (c == 0x1b) {
                    state_ = State::OscEscape;
                } else {
                    pending_.push_back(QLatin1Char(c));
                    if (pending_.size() > 1024)
                        state_ = State::Ground;
                }
                break;
            case State::OscEscape:
                osc(pending_);
                state_ = State::Ground;
                break;
            }
            continue;
        }

        // A character of more than one byte. How many follow is in the first.
        int length = 1;
        if ((b & 0xe0) == 0xc0)
            length = 2;
        else if ((b & 0xf0) == 0xe0)
            length = 3;
        else if ((b & 0xf8) == 0xf0)
            length = 4;
        if (at + length > all.size()) {
            partial_ = all.mid(at);
            return;
        }
        const QString decoded = QString::fromUtf8(all.mid(at, length));
        for (QChar ch : decoded)
            put(ch);
        at += length;
    }
}

void TerminalView::control(char c)
{
    switch (c) {
    case 0x1b:
        state_ = State::Escape;
        break;
    case '\n':
    case 0x0b:
    case 0x0c:
        wrapPending_ = false;
        newline();
        break;
    case '\r':
        wrapPending_ = false;
        cursorColumn_ = 0;
        break;
    case '\b':
        wrapPending_ = false;
        if (cursorColumn_ > 0)
            --cursorColumn_;
        break;
    case '\t': {
        wrapPending_ = false;
        const int next = ((cursorColumn_ / 8) + 1) * 8;
        cursorColumn_ = std::min(next, columns_ - 1);
        break;
    }
    default:
        // A bell, and anything else, has nothing to show for it.
        break;
    }
}

void TerminalView::put(QChar ch)
{
    if (wrapPending_) {
        cursorColumn_ = 0;
        newline();
        wrapPending_ = false;
    }
    if (cursorRow_ >= screen_.size())
        return;
    Row &row = screen_[cursorRow_];
    if (cursorColumn_ >= row.size())
        return;
    Cell cell = pen_;
    cell.ch = ch;
    row[cursorColumn_] = cell;
    if (cursorColumn_ + 1 >= columns_)
        wrapPending_ = true;
    else
        ++cursorColumn_;
}

void TerminalView::escape(char c)
{
    switch (c) {
    case 'M':
        // Up one line, scrolling the region if there is nowhere to go.
        if (cursorRow_ <= regionTop_)
            scrollDown(1);
        else
            --cursorRow_;
        break;
    case '7':
        savedRow_ = cursorRow_;
        savedColumn_ = cursorColumn_;
        break;
    case '8':
        cursorRow_ = savedRow_;
        cursorColumn_ = savedColumn_;
        clampCursor();
        break;
    case 'c':
        pen_ = Cell();
        clearScreen();
        break;
    default:
        break;
    }
}

void TerminalView::osc(const QString &body)
{
    // The window title and the like. Nothing here has a title bar of its own
    // to put it in.
    Q_UNUSED(body);
}

void TerminalView::csi(const QString &body, char final)
{
    // A `?` in front marks the private settings a program turns on and off.
    if (body.startsWith(QLatin1Char('?'))) {
        const QVector<int> values = numbers(body.mid(1));
        const bool on = final == 'h';
        if (final != 'h' && final != 'l')
            return;
        for (int value : values) {
            switch (value) {
            case 1: applicationCursorKeys_ = on; break;
            case 25: cursorVisible_ = on; break;
            case 1049:
            case 1047:
            case 47: useAlternateScreen(on); break;
            case 2004: bracketedPaste_ = on; break;
            default: break;
            }
        }
        return;
    }

    const QVector<int> values = numbers(body);
    switch (final) {
    case 'A': cursorRow_ -= parameter(values, 0, 1); break;
    case 'B': cursorRow_ += parameter(values, 0, 1); break;
    case 'C': cursorColumn_ += parameter(values, 0, 1); break;
    case 'D': cursorColumn_ -= parameter(values, 0, 1); break;
    case 'E': cursorRow_ += parameter(values, 0, 1); cursorColumn_ = 0; break;
    case 'F': cursorRow_ -= parameter(values, 0, 1); cursorColumn_ = 0; break;
    case 'G': cursorColumn_ = parameter(values, 0, 1) - 1; break;
    case 'd': cursorRow_ = parameter(values, 0, 1) - 1; break;
    case 'H':
    case 'f':
        cursorRow_ = parameter(values, 0, 1) - 1;
        cursorColumn_ = parameter(values, 1, 1) - 1;
        break;
    case 'J': eraseInDisplay(values.isEmpty() ? 0 : values[0]); break;
    case 'K': eraseInLine(values.isEmpty() ? 0 : values[0]); break;
    case 'L': insertLines(parameter(values, 0, 1)); break;
    case 'M': deleteLines(parameter(values, 0, 1)); break;
    case 'P': deleteCharacters(parameter(values, 0, 1)); break;
    case '@': insertCharacters(parameter(values, 0, 1)); break;
    case 'X': eraseCharacters(parameter(values, 0, 1)); break;
    case 'S': scrollUp(parameter(values, 0, 1)); break;
    case 'T': scrollDown(parameter(values, 0, 1)); break;
    case 'm': selectGraphic(values); break;
    case 'r':
        regionTop_ = parameter(values, 0, 1) - 1;
        regionBottom_ = values.size() > 1 && values[1] > 0 ? values[1] - 1 : rows_ - 1;
        regionTop_ = std::clamp(regionTop_, 0, rows_ - 1);
        regionBottom_ = std::clamp(regionBottom_, regionTop_, rows_ - 1);
        cursorRow_ = regionTop_;
        cursorColumn_ = 0;
        break;
    case 's':
        savedRow_ = cursorRow_;
        savedColumn_ = cursorColumn_;
        break;
    case 'u':
        cursorRow_ = savedRow_;
        cursorColumn_ = savedColumn_;
        break;
    default:
        break;
    }
    wrapPending_ = false;
    clampCursor();
}

void TerminalView::selectGraphic(const QVector<int> &parameters)
{
    if (parameters.isEmpty()) {
        pen_ = Cell();
        return;
    }
    for (int i = 0; i < parameters.size(); ++i) {
        const int value = parameters[i];
        switch (value) {
        case 0: pen_ = Cell(); break;
        case 1: pen_.bold = true; break;
        case 2: pen_.faint = true; break;
        case 4: pen_.underline = true; break;
        case 7: pen_.inverse = true; break;
        case 22: pen_.bold = false; pen_.faint = false; break;
        case 24: pen_.underline = false; break;
        case 27: pen_.inverse = false; break;
        case 39: pen_.foreground = -1; break;
        case 49: pen_.background = -1; break;
        case 38:
        case 48: {
            // A colour given as an index into the table, or in full.
            const bool foreground = value == 38;
            const int kind = i + 1 < parameters.size() ? parameters[i + 1] : 0;
            if (kind == 5 && i + 2 < parameters.size()) {
                (foreground ? pen_.foreground : pen_.background) = parameters[i + 2];
                i += 2;
            } else if (kind == 2 && i + 4 < parameters.size()) {
                // A colour given outright. Anything that draws its own bars
                // and gauges asks for these, and rounding them to the nearest
                // of sixteen is why such a thing looked colourless.
                (foreground ? pen_.foreground : pen_.background) =
                    directColour(parameters[i + 2], parameters[i + 3], parameters[i + 4]);
                i += 4;
            }
            break;
        }
        default:
            if (value >= 30 && value <= 37)
                pen_.foreground = value - 30;
            else if (value >= 40 && value <= 47)
                pen_.background = value - 40;
            else if (value >= 90 && value <= 97)
                pen_.foreground = value - 90 + 8;
            else if (value >= 100 && value <= 107)
                pen_.background = value - 100 + 8;
            break;
        }
    }
}

// ------------------------------------------------------------------ painting

QColor TerminalView::colourOf(int index, bool foreground, bool bold) const
{
    if (index < 0) {
        const Theme &theme = Theme::current();
        const QColor c = theme.colour(foreground ? QStringLiteral("editorText")
                                                 : QStringLiteral("editorBackground"));
        return c.isValid() ? c : (foreground ? QColor(220, 220, 220) : QColor(20, 20, 20));
    }
    // A bold one of the first eight is the brighter of the pair, which is what
    // every terminal has always done. A colour given outright is already
    // exactly what was asked for.
    if (bold && index < 8)
        index += 8;
    const QColor c = indexedColour(index);
    return c.isValid() ? c : QColor(200, 200, 200);
}

int TerminalView::topLine() const
{
    return verticalScrollBar()->value();
}

void TerminalView::scrollContentsBy(int dx, int dy)
{
    Q_UNUSED(dx);
    Q_UNUSED(dy);
    // The whole viewport is drawn from the screen and the history, so moving
    // the bar means painting again rather than shifting pixels about.
    following_ = verticalScrollBar()->value() >= verticalScrollBar()->maximum();
    viewport()->update();
}

void TerminalView::updateScrollBar()
{
    const int total = history_.size() + rows_;
    verticalScrollBar()->setRange(0, std::max(0, total - rows_));
    verticalScrollBar()->setPageStep(rows_);
    verticalScrollBar()->setSingleStep(1);
}

void TerminalView::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    painter.setFont(font());
    const QColor background = colourOf(-1, false, false);
    painter.fillRect(event->rect(), background);

    const int first = topLine();
    // The selection, as a pair of places in the whole history, in order.
    int fromLine = anchorLine_, fromColumn = anchorColumn_;
    int toLine = caretLine_, toColumn = caretColumn_;
    if (fromLine > toLine || (fromLine == toLine && fromColumn > toColumn)) {
        std::swap(fromLine, toLine);
        std::swap(fromColumn, toColumn);
    }
    const bool hasSelection = anchorLine_ >= 0 && caretLine_ >= 0 &&
                              (fromLine != toLine || fromColumn != toColumn);
    const QColor selectionColour = Theme::current().colour(QStringLiteral("selection"));

    for (int y = 0; y < rows_; ++y) {
        const int line = first + y;
        const Row *row = nullptr;
        if (line < history_.size())
            row = &history_[line];
        else if (line - history_.size() < screen_.size())
            row = &screen_[line - history_.size()];
        if (row == nullptr)
            continue;

        const int top = y * cellHeight_;
        for (int x = 0; x < row->size() && x < columns_; ++x) {
            const Cell &cell = (*row)[x];
            bool inverse = cell.inverse;
            if (hasSelection) {
                const bool after = line > fromLine || (line == fromLine && x >= fromColumn);
                const bool before = line < toLine || (line == toLine && x < toColumn);
                if (after && before)
                    inverse = !inverse;
            }
            QColor fg = colourOf(cell.foreground, true, cell.bold);
            QColor bg = colourOf(cell.background, false, false);
            if (cell.faint)
                fg = QColor(fg.red() / 2 + bg.red() / 2, fg.green() / 2 + bg.green() / 2,
                            fg.blue() / 2 + bg.blue() / 2);
            if (inverse) {
                if (hasSelection && selectionColour.isValid() && !cell.inverse) {
                    bg = selectionColour;
                } else {
                    std::swap(fg, bg);
                }
            }
            const QRect box(x * cellWidth_, top, cellWidth_, cellHeight_);
            if (bg != background)
                painter.fillRect(box, bg);
            if (cell.ch != QLatin1Char(' ') && !cell.ch.isNull()) {
                QFont f = font();
                f.setBold(cell.bold);
                f.setUnderline(cell.underline);
                painter.setFont(f);
                painter.setPen(fg);
                painter.drawText(QPoint(box.left(), top + baseline_), QString(cell.ch));
            }
        }
    }

    // The cursor, drawn where the screen is rather than where the view is
    // scrolled to, and only when this has the keyboard.
    if (cursorVisible_) {
        const int line = history_.size() + cursorRow_;
        const int y = line - first;
        if (y >= 0 && y < rows_) {
            const QRect box(cursorColumn_ * cellWidth_, y * cellHeight_, cellWidth_, cellHeight_);
            const QColor caret = colourOf(-1, true, false);
            if (focused_) {
                painter.fillRect(box, caret);
                const Row &row = screen_[cursorRow_];
                if (cursorColumn_ < row.size() && row[cursorColumn_].ch != QLatin1Char(' ')) {
                    painter.setPen(background);
                    painter.setFont(font());
                    painter.drawText(QPoint(box.left(), y * cellHeight_ + baseline_),
                                     QString(row[cursorColumn_].ch));
                }
            } else {
                painter.setPen(caret);
                painter.drawRect(box.adjusted(0, 0, -1, -1));
            }
        }
    }
}

void TerminalView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    reshape();
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

// ------------------------------------------------------------------ input

// A window full of shortcuts and a terminal cannot both have the keyboard.
// Qt offers every shortcut to the widget in focus first, and taking them here
// is what stops Ctrl-C ending an analysis instead of the program running in
// this pane, and Cmd-B jumping to a definition instead of reaching the shell.
//
// Two are left alone on purpose: quitting and closing a window are how a
// person gets out of anything, and a terminal that swallowed them could trap
// someone in it.
bool TerminalView::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        const bool command = key->modifiers().testFlag(Qt::ControlModifier);
        const bool escape_hatch = command && (key->key() == Qt::Key_Q || key->key() == Qt::Key_W);
        if (running() && !escape_hatch) {
            event->accept();
            return true;
        }
    }
    return QAbstractScrollArea::event(event);
}

bool TerminalView::focusNextPrevChild(bool next)
{
    // Tab belongs to whatever is running, not to the window's focus order.
    Q_UNUSED(next);
    return false;
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    if (pty_ == nullptr || !pty_->running()) {
        // Nothing is running: a keypress asks for another shell rather than
        // going nowhere.
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
            || !event->text().isEmpty()) {
            restart();
            return;
        }
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }

    const Qt::KeyboardModifiers modifiers = event->modifiers();
#if defined(Q_OS_MACOS)
    // Qt hands the Command key over as Control and the Control key as Meta.
    // A terminal cares about the one the machine calls Control, so the names
    // are put back the right way round here rather than everywhere below.
    const bool control = modifiers.testFlag(Qt::MetaModifier);
    const bool command = modifiers.testFlag(Qt::ControlModifier);
#else
    const bool control = modifiers.testFlag(Qt::ControlModifier);
    const bool command = modifiers.testFlag(Qt::MetaModifier);
#endif
    const bool alt = modifiers.testFlag(Qt::AltModifier);
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);

    // Copying and pasting are the window's, not the program's: no terminal
    // sequence means either, and every other application uses these keys for
    // them. Everything else with a modifier goes to the far end.
    const bool copy_paste_key = command || (control && shift);
    if (copy_paste_key && event->key() == Qt::Key_C && !selectedText().isEmpty()) {
        copySelection();
        return;
    }
    if (copy_paste_key && event->key() == Qt::Key_V) {
        QByteArray text = QApplication::clipboard()->text().toLocal8Bit();
        // A program that asked to be told where a paste begins and ends is
        // told, so it can take the whole of it as typing rather than as
        // commands.
        if (bracketedPaste_)
            text = QByteArray("\x1b[200~") + text + "\x1b[201~";
        pty_->write(text);
        return;
    }

    // Anything with Alt is that key with an escape in front, which is what a
    // shell reads as Meta.
    const QByteArray meta = alt ? QByteArray("\x1b") : QByteArray();

    const char *arrows = applicationCursorKeys_ ? "\x1bO" : "\x1b[";
    switch (event->key()) {
    case Qt::Key_Up: pty_->write(meta + arrows + "A"); return;
    case Qt::Key_Down: pty_->write(meta + arrows + "B"); return;
    case Qt::Key_Right: pty_->write(meta + arrows + "C"); return;
    case Qt::Key_Left: pty_->write(meta + arrows + "D"); return;
    case Qt::Key_Home: pty_->write("\x1b[H"); return;
    case Qt::Key_End: pty_->write("\x1b[F"); return;
    case Qt::Key_Insert: pty_->write("\x1b[2~"); return;
    case Qt::Key_Delete: pty_->write("\x1b[3~"); return;
    case Qt::Key_PageUp:
        // Shift is the window scrolling; on its own it is the program's.
        if (shift) {
            verticalScrollBar()->setValue(verticalScrollBar()->value() - rows_);
            viewport()->update();
        } else {
            pty_->write("\x1b[5~");
        }
        return;
    case Qt::Key_PageDown:
        if (shift) {
            verticalScrollBar()->setValue(verticalScrollBar()->value() + rows_);
            viewport()->update();
        } else {
            pty_->write("\x1b[6~");
        }
        return;
    case Qt::Key_Backspace: pty_->write(meta + "\x7f"); return;
    case Qt::Key_Return:
    case Qt::Key_Enter: pty_->write(meta + "\r"); return;
    case Qt::Key_Tab: pty_->write("\t"); return;
    case Qt::Key_Backtab: pty_->write("\x1b[Z"); return;
    case Qt::Key_Escape: pty_->write("\x1b"); return;
    default:
        break;
    }
    if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F12) {
        static const char *const codes[12] = {"\x1bOP", "\x1bOQ", "\x1bOR", "\x1bOS",
                                              "\x1b[15~", "\x1b[17~", "\x1b[18~", "\x1b[19~",
                                              "\x1b[20~", "\x1b[21~", "\x1b[23~", "\x1b[24~"};
        pty_->write(codes[event->key() - Qt::Key_F1]);
        return;
    }

    // Control and a letter is the character that many places into the
    // alphabet: Ctrl-A is one, Ctrl-C is three, and so on up to Ctrl-_.
    if (control) {
        int code = -1;
        const int key = event->key();
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            code = key - Qt::Key_A + 1;
        else if (key == Qt::Key_At || key == Qt::Key_2)
            code = 0;
        else if (key == Qt::Key_BracketLeft)
            code = 27;
        else if (key == Qt::Key_Backslash)
            code = 28;
        else if (key == Qt::Key_BracketRight)
            code = 29;
        else if (key == Qt::Key_AsciiCircum || key == Qt::Key_6)
            code = 30;
        else if (key == Qt::Key_Underscore || key == Qt::Key_Minus || key == Qt::Key_Slash)
            code = 31;
        else if (key == Qt::Key_Space)
            code = 0;
        if (code >= 0) {
            pty_->write(meta + QByteArray(1, static_cast<char>(code)));
            return;
        }
    }

    if (!event->text().isEmpty()) {
        pty_->write(meta + event->text().toLocal8Bit());
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void TerminalView::positionAt(const QPoint &point, int &line, int &column) const
{
    line = topLine() + std::clamp(point.y() / cellHeight_, 0, rows_ - 1);
    column = std::clamp(point.x() / cellWidth_, 0, columns_);
}

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    selecting_ = true;
    positionAt(event->pos(), anchorLine_, anchorColumn_);
    caretLine_ = anchorLine_;
    caretColumn_ = anchorColumn_;
    viewport()->update();
}

void TerminalView::mouseMoveEvent(QMouseEvent *event)
{
    if (!selecting_) {
        QAbstractScrollArea::mouseMoveEvent(event);
        return;
    }
    positionAt(event->pos(), caretLine_, caretColumn_);
    viewport()->update();
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event)
{
    if (selecting_ && event->button() == Qt::LeftButton)
        selecting_ = false;
    QAbstractScrollArea::mouseReleaseEvent(event);
}

QString TerminalView::selectedText() const
{
    if (anchorLine_ < 0 || caretLine_ < 0)
        return QString();
    int fromLine = anchorLine_, fromColumn = anchorColumn_;
    int toLine = caretLine_, toColumn = caretColumn_;
    if (fromLine > toLine || (fromLine == toLine && fromColumn > toColumn)) {
        std::swap(fromLine, toLine);
        std::swap(fromColumn, toColumn);
    }
    QString out;
    for (int line = fromLine; line <= toLine; ++line) {
        const Row *row = nullptr;
        if (line < history_.size())
            row = &history_[line];
        else if (line - history_.size() < screen_.size())
            row = &screen_[line - history_.size()];
        if (row == nullptr)
            continue;
        const int first = line == fromLine ? fromColumn : 0;
        const int last = line == toLine ? toColumn : row->size();
        QString text;
        for (int x = first; x < last && x < row->size(); ++x)
            text.push_back((*row)[x].ch);
        // Trailing blanks are the screen being a rectangle, not part of what
        // was written.
        while (text.endsWith(QLatin1Char(' ')))
            text.chop(1);
        out += text;
        if (line != toLine)
            out += QLatin1Char('\n');
    }
    return out;
}

void TerminalView::copySelection()
{
    const QString text = selectedText();
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

void TerminalView::focusInEvent(QFocusEvent *event)
{
    focused_ = true;
    QAbstractScrollArea::focusInEvent(event);
    viewport()->update();
}

void TerminalView::focusOutEvent(QFocusEvent *event)
{
    focused_ = false;
    QAbstractScrollArea::focusOutEvent(event);
    viewport()->update();
}

} // namespace astral::gui
