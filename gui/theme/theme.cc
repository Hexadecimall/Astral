#include "theme/theme.hh"

#include <QFile>
#include <QTextStream>

#include <array>
#include <utility>

namespace astral::gui {
namespace {

// The order here is the order a theme file is expected to read in.
constexpr std::array<std::pair<const char *, const char *>, 31> kDefaults = {{
    // Surfaces, darkest to lightest.
    {"background", "#1b1c1f"},
    {"panel", "#222428"},
    {"panelRaised", "#2a2d32"},
    {"border", "#35383e"},
    {"editorBackground", "#1e1f23"},
    {"editorLine", "#26282d"},
    {"selection", "#2d4a7a"},
    {"selectionInactive", "#2a3140"},
    {"hover", "#2f333a"},
    // Text.
    {"text", "#d6d9df"},
    {"textMuted", "#8b909a"},
    {"textDisabled", "#5d626b"},
    {"accent", "#4d8dff"},
    {"accentText", "#ffffff"},
    {"warning", "#e3b341"},
    {"error", "#f0645f"},
    {"success", "#6fcf7a"},
    // Code tokens, shared by the decompiler and the listing.
    {"token.keyword", "#c792ea"},
    {"token.type", "#82aaff"},
    {"token.function", "#ffcb6b"},
    {"token.variable", "#d6d9df"},
    {"token.parameter", "#f5b8d0"},
    {"token.global", "#89ddff"},
    {"token.number", "#f78c6c"},
    {"token.string", "#c3e88d"},
    {"token.comment", "#6b7280"},
    {"token.label", "#e3b341"},
    {"token.address", "#5d626b"},
    {"token.mnemonic", "#82aaff"},
    {"token.register", "#f5b8d0"},
    // A byte changed in the hex view and not yet queued.
    {"token.dirty", "#ffb86c"},
}};

} // namespace

namespace {
Theme &currentTheme()
{
    static Theme theme = Theme::defaults();
    return theme;
}
} // namespace

const Theme &Theme::current() { return currentTheme(); }
void Theme::setCurrent(const Theme &theme) { currentTheme() = theme; }

Theme Theme::defaults()
{
    Theme theme;
    for (const auto &[key, value] : kDefaults)
        theme.colours_.insert(QString::fromLatin1(key), QColor(QString::fromLatin1(value)));
    return theme;
}

const QStringList &Theme::keys()
{
    static const QStringList list = [] {
        QStringList out;
        for (const auto &[key, value] : kDefaults)
            out << QString::fromLatin1(key);
        return out;
    }();
    return list;
}

Theme Theme::parse(const QString &text, QString *error)
{
    Theme theme = defaults();
    if (error)
        error->clear();
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        int hash = line.indexOf(QLatin1Char('#'));
        // A colour starts with '#', so only a '#' that is not preceded by '='
        // on the same line begins a comment.
        int eq = line.indexOf(QLatin1Char('='));
        if (hash >= 0 && (eq < 0 || hash < eq))
            line = line.left(hash);
        else if (hash >= 0) {
            int second = line.indexOf(QLatin1Char('#'), hash + 1);
            if (second >= 0)
                line = line.left(second);
        }
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        eq = line.indexOf(QLatin1Char('='));
        if (eq < 0) {
            if (error)
                *error = QStringLiteral("line %1: expected `key = #rrggbb`").arg(i + 1);
            return defaults();
        }
        QString key = line.left(eq).trimmed();
        QString value = line.mid(eq + 1).trimmed();
        if (!theme.colours_.contains(key)) {
            if (error)
                *error = QStringLiteral("line %1: unknown key `%2`").arg(i + 1).arg(key);
            return defaults();
        }
        QColor colour(value);
        if (!colour.isValid()) {
            if (error)
                *error = QStringLiteral("line %1: `%2` is not a colour").arg(i + 1).arg(value);
            return defaults();
        }
        theme.colours_[key] = colour;
    }
    return theme;
}

Theme Theme::load(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("cannot read %1").arg(path);
        return defaults();
    }
    return parse(QString::fromUtf8(file.readAll()), error);
}

QColor Theme::colour(const QString &key) const
{
    return colours_.value(key);
}

QPalette Theme::palette() const
{
    QPalette p;
    p.setColor(QPalette::Window, colour(QStringLiteral("background")));
    p.setColor(QPalette::WindowText, colour(QStringLiteral("text")));
    p.setColor(QPalette::Base, colour(QStringLiteral("editorBackground")));
    p.setColor(QPalette::AlternateBase, colour(QStringLiteral("panel")));
    p.setColor(QPalette::Text, colour(QStringLiteral("text")));
    p.setColor(QPalette::Button, colour(QStringLiteral("panelRaised")));
    p.setColor(QPalette::ButtonText, colour(QStringLiteral("text")));
    p.setColor(QPalette::Highlight, colour(QStringLiteral("selection")));
    p.setColor(QPalette::HighlightedText, colour(QStringLiteral("text")));
    p.setColor(QPalette::ToolTipBase, colour(QStringLiteral("panelRaised")));
    p.setColor(QPalette::ToolTipText, colour(QStringLiteral("text")));
    p.setColor(QPalette::PlaceholderText, colour(QStringLiteral("textMuted")));
    p.setColor(QPalette::Link, colour(QStringLiteral("accent")));
    p.setColor(QPalette::Disabled, QPalette::Text, colour(QStringLiteral("textDisabled")));
    p.setColor(QPalette::Disabled, QPalette::WindowText, colour(QStringLiteral("textDisabled")));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, colour(QStringLiteral("textDisabled")));
    return p;
}

QString Theme::styleSheet() const
{
    // Flat surfaces, one-pixel borders, no gradients: the tool-window look.
    QString s = QStringLiteral(R"(
QMainWindow { background: transparent; }
QDialog { background: %background%; }
QWidget { color: %text%; font-size: 13px; }
QWidget#titleBar { background: %panel%; border-bottom: 1px solid %border%; }
QLabel#titleLabel { color: %textMuted%; font-weight: 600; }
QMenuBar { background: %panel%; border-bottom: 1px solid %border%; padding: 2px; }
QMenuBar#titleMenuBar { background: %panel%; border: none; padding: 0; }
QMenuBar#titleMenuBar::item { padding: 5px 9px; }
QToolButton#minimiseButton, QToolButton#maximiseButton, QToolButton#closeButton { border-radius: 0; font-size: 13px; color: %textMuted%; padding: 0; }
QToolButton#minimiseButton:hover, QToolButton#maximiseButton:hover { background: %hover%; color: %text%; }
QToolButton#closeButton:hover { background: %error%; color: %accentText%; }
QMenuBar::item { padding: 4px 8px; background: transparent; }
QMenuBar::item:selected { background: %hover%; border-radius: 4px; }
QMenu { background: %panelRaised%; border: 1px solid %border%; padding: 4px; }
QMenu::item { padding: 5px 24px 5px 12px; border-radius: 4px; }
QMenu::item:selected { background: %selection%; }
QMenu::item:disabled { color: %textDisabled%; }
QMenu::separator { height: 1px; background: %border%; margin: 4px 8px; }
QToolBar { background: %panel%; border-bottom: 1px solid %border%; spacing: 4px; padding: 2px 6px; }
QToolBar::separator { width: 1px; background: %border%; margin: 4px 4px; }
QToolButton { background: transparent; border: none; border-radius: 4px; padding: 4px; color: %text%; }
QToolButton:hover { background: %hover%; }
QToolButton:pressed, QToolButton:checked { background: %selection%; }
QToolButton:disabled { color: %textDisabled%; }
QStatusBar { background: %panel%; border-top: 1px solid %border%; color: %textMuted%; }
QStatusBar::item { border: none; }
QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; color: %textMuted%; }
QDockWidget::title { background: %panel%; border-bottom: 1px solid %border%; padding: 5px 8px; text-align: left; font-weight: 600; }
QDockWidget::close-button, QDockWidget::float-button { background: transparent; border: none; padding: 0px; }
QDockWidget::close-button:hover, QDockWidget::float-button:hover { background: %hover%; }
QMainWindow::separator { background: %border%; width: 1px; height: 1px; }
QMainWindow::separator:hover { background: %accent%; }
QSplitter::handle { background: %border%; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QTabWidget::pane { border: none; border-top: 1px solid %border%; background: %editorBackground%; }
QTabBar { background: %panel%; }
QWidget#tabHeader { background: %panel%; border-bottom: 1px solid %border%; }
QFrame#tabDivider { background: %border%; margin: 8px 6px; }
QTabBar#programTabs::tab, QTabBar#viewTabs::tab { height: 20px; padding: 6px 12px; font-size: 13px; }
QTabBar#programTabs::tab { padding-right: 8px; }
QTabBar::tab { background: %panel%; color: %textMuted%; padding: 6px 14px; border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: %text%; border-bottom: 2px solid %accent%; }
QTabBar::tab:hover:!selected { background: %hover%; }
QTabBar::close-button { image: url(:/icons/close.svg); subcontrol-position: right; width: 14px; height: 14px; margin-right: 2px; }
QTabBar::close-button:hover { image: url(:/icons/close-hover.svg); }
QTreeView, QListView, QTableView, QPlainTextEdit, QTextEdit {
    background: %editorBackground%; border: none; selection-background-color: %selection%;
    selection-color: %text%; outline: none; alternate-background-color: %editorBackground%; }
QTreeView::item, QListView::item { padding: 2px 4px; }
QTreeView::item:hover, QListView::item:hover, QTableView::item:hover { background: %hover%; }
QTreeView::item:selected, QListView::item:selected, QTableView::item:selected { background: %selection%; }
QTreeView::item:selected:!active, QListView::item:selected:!active, QTableView::item:selected:!active { background: %selectionInactive%; }
QTreeView::branch { background: %editorBackground%; }
QHeaderView::section { background: %panel%; color: %textMuted%; border: none; border-right: 1px solid %border%; border-bottom: 1px solid %border%; padding: 4px 6px; }
QLineEdit, QComboBox, QSpinBox { background: %editorBackground%; border: 1px solid %border%; border-radius: 4px; padding: 3px 6px; selection-background-color: %selection%; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 1px solid %accent%; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: %panelRaised%; border: 1px solid %border%; selection-background-color: %selection%; }
QPushButton { background: %panelRaised%; border: 1px solid %border%; border-radius: 4px; padding: 4px 12px; }
QPushButton:hover { background: %hover%; }
QPushButton:pressed { background: %selection%; }
QPushButton:default { background: %accent%; color: %accentText%; border-color: %accent%; }
QPushButton:disabled { color: %textDisabled%; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: %border%; min-height: 24px; border-radius: 5px; margin: 2px; }
QScrollBar::handle:vertical:hover { background: %textMuted%; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: %border%; min-width: 24px; border-radius: 5px; margin: 2px; }
QScrollBar::handle:horizontal:hover { background: %textMuted%; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; border: none; height: 0; width: 0; }
QToolTip { background: %panelRaised%; color: %text%; border: 1px solid %border%; padding: 4px; }
QLabel#muted { color: %textMuted%; }
QLabel#warning { color: %warning%; }
QLabel#error { color: %error%; }
QLabel#success { color: %success%; }
QFrame#editorLine { background: %editorLine%; }
QWidget#decompilerHeader { background: %panel%; border-bottom: 1px solid %border%; }
QLabel#decompilerName { font-size: 13px; color: %text%; }
QFrame#searchResults { background: %panelRaised%; border: 1px solid %accent%; border-radius: 6px; }
QListWidget#searchResultsList { background: %panelRaised%; border: none; font-size: 12px; }
QListWidget#searchResultsList::item { padding: 3px 10px; }
QLabel#searchResultsSummary { background: %panel%; color: %textMuted%; font-size: 11px; border-top: 1px solid %border%; }
QLabel#busyPill { background: %panelRaised%; color: %text%; border: 1px solid %accent%; border-radius: 12px; padding: 5px 14px; font-size: 12px; }
QToolButton#headerButton { border: 1px solid %border%; border-radius: 4px; padding: 2px 10px; color: %text%; font-size: 11px; }
QToolButton#headerButton:hover { background: %hover%; border-color: %accent%; }
QToolButton#headerButton:disabled { color: %textDisabled%; }
QWidget#welcomePage { background: %editorBackground%; }
QScrollArea { background: transparent; }
QLabel#welcomeTitle { font-size: 30px; font-weight: 700; }
QLabel#welcomeTagline { font-size: 15px; color: %textMuted%; }
QLabel#welcomeHeading { font-size: 12px; font-weight: 600; color: %textMuted%; letter-spacing: 1px; }
QFrame#card { background: %panel%; border: 1px solid %border%; border-radius: 8px; }
QFrame#card:hover { background: %panelRaised%; border: 1px solid %accent%; }
QFrame#card:focus { border: 1px solid %accent%; }
QLabel#cardGlyph { font-size: 18px; color: %accent%; }
QLabel#cardTitle { font-size: 14px; font-weight: 600; }
QLabel#cardDetail { font-size: 12px; color: %textMuted%; }
QFrame#recentBox { background: %panel%; border: 1px solid %border%; border-radius: 8px; }
QFrame#recentRow { background: transparent; border: none; border-bottom: 1px solid %border%; border-radius: 0; }
QFrame#recentRow:hover { background: %hover%; }
QFrame#newsBox { background: %panel%; border: 1px solid %border%; border-radius: 8px; }
QLabel#newsItem { font-size: 13px; color: %text%; }
)");
    for (auto it = colours_.cbegin(); it != colours_.cend(); ++it)
        s.replace(QStringLiteral("%%1%").arg(it.key()), it.value().name());
    return s;
}

} // namespace astral::gui
