// How a program should be run: what to hand it, what it reads, where to
// begin, and how long to let it go.
//
// A binary is rarely interesting when run with no arguments, and typing them
// again every time is how a tool wastes someone's afternoon. So a run is a
// named thing that is kept, edited and chosen, rather than two boxes on a
// toolbar.
#ifndef ASTRAL_GUI_RUNCONFIG_HH
#define ASTRAL_GUI_RUNCONFIG_HH

#include <QString>
#include <QStringList>

#include <vector>

namespace astral::gui {

struct RunConfiguration {
    QString name;
    // Handed to the program as argv, after argv[0], which is the program.
    QStringList arguments;
    // What it reads from its input.
    QString input;
    // Where to begin: a function name, an address, or empty for the program's
    // own entry point.
    QString entry;
    // How many instructions to allow before stopping a run that will not end.
    quint64 stepLimit = 0;
    // Stop at the first instruction rather than running to a breakpoint.
    bool stopAtStart = true;

    bool operator==(const RunConfiguration &other) const;
};

// The configurations for one program, kept in the settings file so they
// survive and can be edited by hand.
class RunConfigurations {
public:
    // Reads the configurations recorded for `program`. Always answers with at
    // least one, since a program with none can still be run with none.
    static std::vector<RunConfiguration> forProgram(const QString &program);
    static void save(const QString &program, const std::vector<RunConfiguration> &configurations);

    // Which one was last used, by name.
    static QString chosen(const QString &program);
    static void setChosen(const QString &program, const QString &name);

    // The one a program starts with when nothing has been recorded.
    static RunConfiguration byDefault();
};

} // namespace astral::gui

#endif
