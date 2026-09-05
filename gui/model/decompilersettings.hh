// The decompiler's settings: what exists, what each is set to, and how a
// value gets from the settings file into an open program.
//
// The list of settings is the library's, read once out of its option table, so
// this file describes none of them itself. A setting added to the engine shows
// up in the dialog and in the settings file without a change here.
//
// A value is kept for every program by default, because someone who wants
// eighty columns wants eighty columns in the next binary too. A program can
// override any of them, and its own value wins while it is open.
#ifndef ASTRAL_GUI_DECOMPILERSETTINGS_HH
#define ASTRAL_GUI_DECOMPILERSETTINGS_HH

#include <QString>
#include <QStringList>
#include <QVector>

namespace astral::gui {

class ProgramDocument;

// One row of the library's option table.
struct OptionInfo {
    enum class Kind { Boolean, Integer, Choice };
    enum class Scope { Engine, Emission, Interface };

    QString name;
    QString group;
    QString label;
    QString explanation;
    QString defaultValue;
    // The decompiler option this reaches, empty when nothing in the engine
    // corresponds to it. Shown so someone reading the settings file can find
    // the option in the decompiler's own documentation.
    QString engineName;
    Kind kind = Kind::Boolean;
    Scope scope = Scope::Engine;
    int minimum = 0;
    int maximum = 0;
    // Whether the function has to be analysed again for a change to show.
    bool needsReanalysis = false;
    QStringList choices;
};

class DecompilerSettings {
public:
    // Every setting, in the order the dialog shows them.
    static const QVector<OptionInfo> &options();
    // The setting of that name, or null.
    static const OptionInfo *find(const QString &name);
    // The group headings, in the order they first appear.
    static QStringList groups();

    // How a program is named in the settings file: its file name, and enough
    // of a digest of the whole path to tell two of the same name apart.
    static QString programKey(const QString &path);

    // What the setting stands at: the program's own value where it has one,
    // otherwise the one set for every program, otherwise the default.
    static QString value(const QString &name, const QString &programKey = QString());
    // The value set for every program, or the default when none is set.
    static QString globalValue(const QString &name);
    // Whether this program overrides the setting.
    static bool overridden(const QString &name, const QString &programKey);

    // Records a value, after asking the library whether the setting takes it.
    // A value it refuses is not written and `error` says what was expected.
    static bool setValue(const QString &name, const QString &value, const QString &programKey,
                         QString *error);
    // The same, with the open program given the value first. Only what the
    // decompiler accepted is written down, so the file never claims a setting
    // that did not take. Some values, a prototype model among them, can only
    // be judged against a program.
    static bool applyAndSet(ProgramDocument *document, const QString &name, const QString &value,
                            const QString &programKey, QString *error);
    // Forgets a value, so the one below it applies again.
    static void clearValue(const QString &name, const QString &programKey);
    // Forgets every value in a group, or in all of them.
    static void resetGroup(const QString &group, const QString &programKey);
    static void resetAll(const QString &programKey);

    // Hands the program everything set for it. Returns false when the engine
    // refused something, with a line per refusal in `problems`; the rest are
    // still applied, because one bad line in a hand-edited file should not
    // leave the program unconfigured.
    static bool applyTo(ProgramDocument &document, QStringList &problems);

    // The settings file key a value is written under, so the explanation the
    // dialog shows can name it.
    static QString keyFor(const QString &name, const QString &programKey);

private:
    // Remembers which program a key stands for, so the file can be read.
    static void recordProgramPath(const QString &programKey, const QString &path);
};

} // namespace astral::gui

#endif
