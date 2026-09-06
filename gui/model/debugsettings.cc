#include "model/debugsettings.hh"
#include "model/settings.hh"

#include <QCoreApplication>

namespace astral::gui {

namespace {

// Where one setting of one configuration of one program is kept. The program
// is part of the key because a run belongs to a binary, and the configuration
// is because two ways of running the same binary are allowed to disagree.
QString keyFor(const QString &program, const QString &configuration, const QString &name)
{
    return QStringLiteral("debug.%1.%2.%3")
        .arg(QString(program).replace(QLatin1Char('.'), QLatin1Char('_')),
             QString(configuration).replace(QLatin1Char('.'), QLatin1Char('_')), name);
}

DebugOption boolean(const char *name, const char *group, const char *label,
                    const char *explanation, bool byDefault, bool emulated = true,
                    bool live = true)
{
    DebugOption option;
    option.name = QString::fromLatin1(name);
    option.group = QCoreApplication::translate("DebugSettings", group);
    option.label = QCoreApplication::translate("DebugSettings", label);
    option.explanation = QCoreApplication::translate("DebugSettings", explanation);
    option.defaultValue = byDefault ? QStringLiteral("on") : QStringLiteral("off");
    option.kind = DebugOption::Kind::Boolean;
    option.emulated = emulated;
    option.live = live;
    return option;
}

DebugOption number(const char *name, const char *group, const char *label,
                   const char *explanation, int byDefault, int low, int high,
                   bool emulated = true, bool live = true)
{
    DebugOption option;
    option.name = QString::fromLatin1(name);
    option.group = QCoreApplication::translate("DebugSettings", group);
    option.label = QCoreApplication::translate("DebugSettings", label);
    option.explanation = QCoreApplication::translate("DebugSettings", explanation);
    option.defaultValue = QString::number(byDefault);
    option.kind = DebugOption::Kind::Integer;
    option.minimum = low;
    option.maximum = high;
    option.emulated = emulated;
    option.live = live;
    return option;
}

DebugOption choice(const char *name, const char *group, const char *label,
                   const char *explanation, const char *byDefault,
                   const QStringList &choices, bool emulated = true, bool live = true)
{
    DebugOption option;
    option.name = QString::fromLatin1(name);
    option.group = QCoreApplication::translate("DebugSettings", group);
    option.label = QCoreApplication::translate("DebugSettings", label);
    option.explanation = QCoreApplication::translate("DebugSettings", explanation);
    option.defaultValue = QString::fromLatin1(byDefault);
    option.kind = DebugOption::Kind::Choice;
    option.choices = choices;
    option.emulated = emulated;
    option.live = live;
    return option;
}

DebugOption text(const char *name, const char *group, const char *label,
                 const char *explanation, const char *byDefault = "",
                 bool emulated = true, bool live = true)
{
    DebugOption option;
    option.name = QString::fromLatin1(name);
    option.group = QCoreApplication::translate("DebugSettings", group);
    option.label = QCoreApplication::translate("DebugSettings", label);
    option.explanation = QCoreApplication::translate("DebugSettings", explanation);
    option.defaultValue = QString::fromLatin1(byDefault);
    option.kind = DebugOption::Kind::Text;
    option.emulated = emulated;
    option.live = live;
    return option;
}

} // namespace

const QVector<DebugOption> &DebugSettings::options()
{
    static const QVector<DebugOption> table = {
        // ------------------------------------------------------------ engine
        choice("engine", "Engine", "Run with",
               "Emulating runs the p-code Astral already lifted, against a stand-in "
               "libc. It works on any binary Astral can read, whatever machine this "
               "is, and it cannot call anything the stand-in does not have. Live runs "
               "the real program on this machine, which needs a native back end.",
               "emulate", {QStringLiteral("emulate"), QStringLiteral("live")}),
        number("stepLimit", "Engine", "Instructions before giving up",
               "A run that will not end is stopped after this many instructions. "
               "Zero lets it go until something else stops it.",
               0, 0, 1000000000),
        number("stackBytes", "Engine", "Stack size",
               "How much stack the program is given. A program that recurses deeply "
               "needs more; nothing else notices.",
               1048576, 4096, 268435456, true, false),
        number("heapBytes", "Engine", "Heap size",
               "How much the stand-in allocator can hand out before it refuses.",
               16777216, 65536, 1073741824, true, false),
        boolean("zeroMemory", "Engine", "Start memory as zero",
                "Anything the image does not define reads as zero rather than as "
                "whatever was there. Off is closer to a real machine and makes an "
                "uninitialised read visible.",
                true, true, false),

        // ------------------------------------------------------- the program
        text("arguments", "Program", "Arguments",
             "Handed to the program as argv, after argv[0], which is the program "
             "itself. Separated by spaces."),
        text("input", "Program", "Standard input",
             "What the program reads when it asks. Written as it would be typed."),
        text("entry", "Program", "Begin at",
             "A function name or an address. Empty starts at the program's own "
             "entry point."),
        text("workingDirectory", "Program", "Working directory",
             "Where the program starts. Only a live run has one.", "", false, true),
        text("environment", "Program", "Environment",
             "NAME=value, one per line, added to what the program is started with.",
             "", false, true),

        // --------------------------------------------------------- stopping
        boolean("stopAtStart", "Stopping", "Stop at the first instruction",
                "Otherwise it runs until a breakpoint or the end. On is what a "
                "debugger is for; off is how a program is tried once.",
                true),
        boolean("stopOnFault", "Stopping", "Stop when it faults",
                "A read of memory that is not there, or an instruction that cannot "
                "be executed, stops the run where it happened rather than ending it.",
                true),
        boolean("stopOnUnknownCall", "Stopping", "Stop at a call nothing answers",
                "The stand-in libc knows about twenty functions. This stops at the "
                "first call to something outside that, which is where an emulated "
                "run of a real program usually ends.",
                true, true, false),
        boolean("stopOnOutput", "Stopping", "Stop when it writes",
                "Every write to output stops the run. Useful for finding which line "
                "printed something, tiring otherwise.",
                false),
        number("watchBytes", "Stopping", "Bytes a new watchpoint covers",
               "What Watch uses when it is not told a size.", 16, 1, 4096),

        // -------------------------------------------------------- recording
        boolean("trace", "Recording", "Record every instruction",
                "A line for each one executed. Off unless it is wanted: a real "
                "program is millions of them.",
                false),
        number("traceLimit", "Recording", "Trace lines kept",
               "The most recent this many, so a long run does not fill memory. "
               "Zero keeps all of them.",
               100000, 0, 100000000),
        boolean("recordCalls", "Recording", "Note every library call",
                "Each call the stand-in answers is written to the output pane with "
                "what it was passed.",
                true, true, false),
        boolean("recordOutput", "Recording", "Keep what it writes",
                "The program's own output, gathered as it runs.", true),

        // ------------------------------------------------------------ views
        number("dumpBytes", "Views", "Bytes the dump shows",
               "How much memory to read for the dump pane at once.", 256, 16, 65536),
        choice("dumpFollows", "Views", "The dump follows",
               "What the memory pane is pointed at when a run stops. A register "
               "name keeps it on that register as the program changes it.",
               "nothing",
               {QStringLiteral("nothing"), QStringLiteral("sp"), QStringLiteral("pc"),
                QStringLiteral("the last watchpoint")}),
        choice("registerRadix", "Views", "Registers shown in",
               "How a register's value is written, and how one typed over is read.",
               "hexadecimal",
               {QStringLiteral("hexadecimal"), QStringLiteral("decimal"),
                QStringLiteral("both")}),
        boolean("showChangedRegisters", "Views", "Mark registers that changed",
                "A register the last step changed is marked, so what an instruction "
                "did is visible without remembering what was there before.",
                true),
        number("stackFrames", "Views", "Frames to walk",
               "How far back the call stack is followed. A frame pointer reused for "
               "something else ends the walk whatever this says.",
               64, 1, 4096),
        boolean("followLocation", "Views", "Follow the program",
                "The listing and the source move to wherever it stopped. Off keeps "
                "the view where it was put.",
                true),
    };
    return table;
}

const DebugOption *DebugSettings::find(const QString &name)
{
    for (const DebugOption &option : options())
        if (option.name == name)
            return &option;
    return nullptr;
}

QStringList DebugSettings::groups()
{
    QStringList out;
    for (const DebugOption &option : options())
        if (!out.contains(option.group))
            out << option.group;
    return out;
}

QString DebugSettings::value(const QString &program, const QString &configuration,
                             const QString &name)
{
    const DebugOption *option = find(name);
    if (option == nullptr)
        return QString();
    const QString kept =
        Settings::instance().stringValue(keyFor(program, configuration, name));
    return kept.isEmpty() ? option->defaultValue : kept;
}

void DebugSettings::setValue(const QString &program, const QString &configuration,
                             const QString &name, const QString &value)
{
    const DebugOption *option = find(name);
    if (option == nullptr)
        return;
    // A value that is the default is not recorded, so the settings file says
    // what someone chose rather than what everything happens to be.
    if (value == option->defaultValue)
        Settings::instance().remove(keyFor(program, configuration, name));
    else
        Settings::instance().setString(keyFor(program, configuration, name), value);
}

bool DebugSettings::boolValue(const QString &program, const QString &configuration,
                              const QString &name)
{
    return value(program, configuration, name) == QStringLiteral("on");
}

int DebugSettings::intValue(const QString &program, const QString &configuration,
                            const QString &name)
{
    return value(program, configuration, name).toInt();
}

void DebugSettings::reset(const QString &program, const QString &configuration)
{
    for (const DebugOption &option : options())
        Settings::instance().remove(keyFor(program, configuration, option.name));
}

} // namespace astral::gui
