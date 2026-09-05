#include "model/runconfig.hh"

#include "model/settings.hh"

#include <QCryptographicHash>
#include <QFileInfo>

namespace astral::gui {

namespace {

// Configurations are recorded against a short digest of the program's path,
// so one settings file holds them for every program without the paths
// themselves becoming keys.
QString keyFor(const QString &program)
{
    const QByteArray digest =
        QCryptographicHash::hash(program.toUtf8(), QCryptographicHash::Sha256).toHex().left(12);
    return QStringLiteral("run.") + QString::fromLatin1(digest);
}

} // namespace

bool RunConfiguration::operator==(const RunConfiguration &other) const
{
    return name == other.name && arguments == other.arguments && input == other.input
           && entry == other.entry && stepLimit == other.stepLimit
           && stopAtStart == other.stopAtStart;
}

RunConfiguration RunConfigurations::byDefault()
{
    RunConfiguration one;
    one.name = QStringLiteral("Default");
    return one;
}

std::vector<RunConfiguration> RunConfigurations::forProgram(const QString &program)
{
    Settings &settings = Settings::instance();
    const QString key = keyFor(program);
    std::vector<RunConfiguration> out;
    for (int i = 1;; ++i) {
        const QString prefix = QStringLiteral("%1.%2").arg(key).arg(i);
        const QString name = settings.stringValue(prefix + QStringLiteral(".name"));
        if (name.isEmpty())
            break;
        RunConfiguration one;
        one.name = name;
        const QString arguments = settings.stringValue(prefix + QStringLiteral(".arguments"));
        if (!arguments.isEmpty())
            one.arguments = arguments.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        one.input = settings.stringValue(prefix + QStringLiteral(".input"));
        one.entry = settings.stringValue(prefix + QStringLiteral(".entry"));
        one.stepLimit = static_cast<quint64>(settings.intValue(prefix + QStringLiteral(".stepLimit"), 0));
        one.stopAtStart = settings.boolValue(prefix + QStringLiteral(".stopAtStart"), true);
        out.push_back(one);
    }
    if (out.empty())
        out.push_back(byDefault());
    return out;
}

void RunConfigurations::save(const QString &program, const std::vector<RunConfiguration> &configurations)
{
    Settings &settings = Settings::instance();
    const QString key = keyFor(program);
    // Anything recorded before goes, or a shorter list would leave a tail.
    for (int i = 1; i < 64; ++i) {
        const QString prefix = QStringLiteral("%1.%2").arg(key).arg(i);
        for (const char *field : {".name", ".arguments", ".input", ".entry", ".stepLimit", ".stopAtStart"})
            settings.remove(prefix + QString::fromLatin1(field));
    }
    for (size_t i = 0; i < configurations.size(); ++i) {
        const RunConfiguration &one = configurations[i];
        const QString prefix = QStringLiteral("%1.%2").arg(key).arg(i + 1);
        settings.setString(prefix + QStringLiteral(".name"), one.name);
        settings.setString(prefix + QStringLiteral(".arguments"), one.arguments.join(QLatin1Char(' ')));
        settings.setString(prefix + QStringLiteral(".input"), one.input);
        settings.setString(prefix + QStringLiteral(".entry"), one.entry);
        settings.setInt(prefix + QStringLiteral(".stepLimit"), static_cast<int>(one.stepLimit));
        settings.setBool(prefix + QStringLiteral(".stopAtStart"), one.stopAtStart);
    }
}

QString RunConfigurations::chosen(const QString &program)
{
    return Settings::instance().stringValue(keyFor(program) + QStringLiteral(".chosen"));
}

void RunConfigurations::setChosen(const QString &program, const QString &name)
{
    Settings::instance().setString(keyFor(program) + QStringLiteral(".chosen"), name);
}

} // namespace astral::gui
