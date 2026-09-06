// Everything about how a program is run and watched, in one table.
//
// The decompiler's settings are a list the dialog renders rather than a dialog
// with settings written into it, and this is the same: a setting added here
// shows up in the window, in the settings file and in a run without another
// line of code anywhere. What differs is that a debugger's settings belong to
// a named way of running a program rather than to the program, so they are
// kept per configuration - "Default" and "with a key" can disagree about the
// step limit, which is the point of having two of them.
#ifndef ASTRAL_GUI_DEBUGSETTINGS_HH
#define ASTRAL_GUI_DEBUGSETTINGS_HH

#include <QString>
#include <QStringList>
#include <QVector>

namespace astral::gui {

// One row of the table. Deliberately the same shape as the decompiler's
// OptionInfo, so the two dialogs read the same and anyone who has used one has
// used the other.
struct DebugOption {
    enum class Kind { Boolean, Integer, Choice, Text };

    QString name;
    QString group;
    QString label;
    QString explanation;
    QString defaultValue;
    Kind kind = Kind::Boolean;
    int minimum = 0;
    int maximum = 0;
    QStringList choices;
    // False for anything the emulated machine cannot answer, so the dialog can
    // say why rather than offering a control that does nothing.
    bool emulated = true;
    bool live = true;
};

class DebugSettings {
public:
    // Every setting, in the order the dialog shows them.
    static const QVector<DebugOption> &options();
    static const DebugOption *find(const QString &name);
    // The group headings, in the order they first appear.
    static QStringList groups();

    // The value in force for one named way of running one program, and the
    // default when nothing has been recorded.
    static QString value(const QString &program, const QString &configuration,
                         const QString &name);
    static void setValue(const QString &program, const QString &configuration,
                         const QString &name, const QString &value);
    static bool boolValue(const QString &program, const QString &configuration,
                          const QString &name);
    static int intValue(const QString &program, const QString &configuration,
                        const QString &name);
    // Puts one configuration back to what it would be if it had never been
    // touched.
    static void reset(const QString &program, const QString &configuration);
};

} // namespace astral::gui

#endif
