// The look of the application, read from a plain text file of `key = #rrggbb`
// lines. Keeping colours in data rather than code means a light theme, or a
// user's own, is a file and not a rebuild.
#ifndef ASTRAL_GUI_THEME_HH
#define ASTRAL_GUI_THEME_HH

#include <QColor>
#include <QHash>
#include <QPalette>
#include <QString>
#include <QStringList>

namespace astral::gui {

class Theme {
public:
    // Every key the file may contain, with the built-in dark values. Unknown
    // keys are errors so a misspelling is caught rather than ignored.
    static Theme defaults();
    static const QStringList &keys();

    // Parses theme text. Missing keys keep their default. On a malformed line
    // `error` names the line and the theme returned is the defaults.
    static Theme parse(const QString &text, QString *error);
    static Theme load(const QString &path, QString *error);

    // The theme the application is running with, set once at startup.
    static const Theme &current();
    static void setCurrent(const Theme &theme);

    QColor colour(const QString &key) const;
    QPalette palette() const;
    // The Qt style sheet that applies the palette to every widget class used.
    QString styleSheet() const;

private:
    QHash<QString, QColor> colours_;
};

} // namespace astral::gui

#endif
