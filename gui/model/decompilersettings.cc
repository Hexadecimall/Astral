#include "model/decompilersettings.hh"

#include "model/programdocument.hh"
#include "model/settings.hh"

#include <astral/astral.h>

#include <QCryptographicHash>
#include <QFileInfo>

namespace astral::gui {

namespace {

QString fromC(const char *text)
{
    return text == nullptr ? QString() : QString::fromUtf8(text);
}

QVector<OptionInfo> readTable()
{
    QVector<OptionInfo> table;
    const int count = astral_option_count();
    table.reserve(count);
    for (int index = 0; index < count; ++index) {
        OptionInfo info;
        info.name = fromC(astral_option_name(index));
        info.group = fromC(astral_option_group(index));
        info.label = fromC(astral_option_label(index));
        info.explanation = fromC(astral_option_explanation(index));
        info.defaultValue = fromC(astral_option_default(index));
        info.engineName = fromC(astral_option_engine_name(index));
        switch (astral_option_kind(index)) {
        case ASTRAL_OPTION_BOOLEAN:
            info.kind = OptionInfo::Kind::Boolean;
            break;
        case ASTRAL_OPTION_INTEGER:
            info.kind = OptionInfo::Kind::Integer;
            break;
        default:
            info.kind = OptionInfo::Kind::Choice;
            break;
        }
        switch (astral_option_scope(index)) {
        case ASTRAL_OPTION_ENGINE:
            info.scope = OptionInfo::Scope::Engine;
            break;
        case ASTRAL_OPTION_EMISSION:
            info.scope = OptionInfo::Scope::Emission;
            break;
        default:
            info.scope = OptionInfo::Scope::Interface;
            break;
        }
        info.minimum = astral_option_minimum(index);
        info.maximum = astral_option_maximum(index);
        info.needsReanalysis = astral_option_needs_reanalysis(index) != 0;
        const int choices = astral_option_choice_count(index);
        for (int choice = 0; choice < choices; ++choice)
            info.choices << fromC(astral_option_choice(index, choice));
        table.push_back(info);
    }
    return table;
}

QString globalKey(const QString &name)
{
    return QStringLiteral("decompiler.") + name;
}

QString programPrefix(const QString &programKey)
{
    return QStringLiteral("decompiler.program.") + programKey + QLatin1Char('.');
}

} // namespace

const QVector<OptionInfo> &DecompilerSettings::options()
{
    static const QVector<OptionInfo> table = readTable();
    return table;
}

const OptionInfo *DecompilerSettings::find(const QString &name)
{
    for (const OptionInfo &info : options())
        if (info.name == name)
            return &info;
    return nullptr;
}

QStringList DecompilerSettings::groups()
{
    QStringList named;
    for (const OptionInfo &info : options())
        if (!named.contains(info.group))
            named << info.group;
    return named;
}

QString DecompilerSettings::programKey(const QString &path)
{
    // The file name is what makes the key readable; the digest is what keeps
    // two programs of the same name apart.
    QString name = QFileInfo(path).fileName();
    for (QChar &letter : name)
        if (!letter.isLetterOrNumber())
            letter = QLatin1Char('-');
    if (name.isEmpty())
        name = QStringLiteral("program");
    const QByteArray digest =
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(6);
    return name + QLatin1Char('-') + QString::fromLatin1(digest);
}

QString DecompilerSettings::keyFor(const QString &name, const QString &programKey)
{
    return programKey.isEmpty() ? globalKey(name) : programPrefix(programKey) + name;
}

QString DecompilerSettings::globalValue(const QString &name)
{
    const OptionInfo *info = find(name);
    if (info == nullptr)
        return QString();
    return Settings::instance().stringValue(globalKey(name), info->defaultValue);
}

bool DecompilerSettings::overridden(const QString &name, const QString &programKey)
{
    if (programKey.isEmpty())
        return false;
    return !Settings::instance().stringValue(programPrefix(programKey) + name).isEmpty();
}

QString DecompilerSettings::value(const QString &name, const QString &programKey)
{
    if (!programKey.isEmpty()) {
        const QString own = Settings::instance().stringValue(programPrefix(programKey) + name);
        if (!own.isEmpty())
            return own;
    }
    return globalValue(name);
}

bool DecompilerSettings::setValue(const QString &name, const QString &value,
                                  const QString &programKey, QString *error)
{
    const OptionInfo *info = find(name);
    if (info == nullptr) {
        if (error != nullptr)
            *error = QObject::tr("There is no setting named '%1'.").arg(name);
        return false;
    }
    // The library decides what a setting takes, so a value it would refuse
    // never reaches the file.
    if (astral_option_check(name.toUtf8().constData(), value.toUtf8().constData()) != ASTRAL_OK) {
        if (error != nullptr)
            *error = fromC(astral_last_error());
        return false;
    }
    Settings::instance().setString(keyFor(name, programKey), value);
    return true;
}

bool DecompilerSettings::applyAndSet(ProgramDocument *document, const QString &name,
                                     const QString &value, const QString &programKey,
                                     QString *error)
{
    const OptionInfo *info = find(name);
    if (info == nullptr) {
        if (error != nullptr)
            *error = QObject::tr("There is no setting named '%1'.").arg(name);
        return false;
    }
    if (document != nullptr && info->scope != OptionInfo::Scope::Interface) {
        QString failure;
        if (!document->setSetting(name, value, failure)) {
            if (error != nullptr)
                *error = failure;
            return false;
        }
    }
    return setValue(name, value, programKey, error);
}

void DecompilerSettings::clearValue(const QString &name, const QString &programKey)
{
    Settings::instance().remove(keyFor(name, programKey));
}

void DecompilerSettings::resetGroup(const QString &group, const QString &programKey)
{
    for (const OptionInfo &info : options())
        if (info.group == group)
            clearValue(info.name, programKey);
}

void DecompilerSettings::resetAll(const QString &programKey)
{
    for (const OptionInfo &info : options())
        clearValue(info.name, programKey);
}

void DecompilerSettings::recordProgramPath(const QString &programKey, const QString &path)
{
    const QString key = programPrefix(programKey) + QStringLiteral("path");
    if (Settings::instance().stringValue(key) != path)
        Settings::instance().setString(key, path);
}

bool DecompilerSettings::applyTo(ProgramDocument &document, QStringList &problems)
{
    const QString key = programKey(document.path());
    bool anyOverride = false;
    for (const OptionInfo &info : options())
        if (overridden(info.name, key))
            anyOverride = true;
    if (anyOverride)
        recordProgramPath(key, document.path());

    for (const OptionInfo &info : options()) {
        if (info.scope == OptionInfo::Scope::Interface)
            continue;
        const QString wanted = value(info.name, key);
        // Only what the program is not already standing at. A program opened
        // with nothing configured is therefore sent nothing at all, and is
        // decompiled exactly as it was before any of this existed; a setting
        // whose line has just been deleted is sent its default, which is what
        // puts the decompiler back.
        if (document.setting(info.name) == wanted)
            continue;
        QString error;
        if (!document.setSetting(info.name, wanted, error))
            problems << QObject::tr("%1: %2").arg(info.name, error);
    }
    return problems.isEmpty();
}

} // namespace astral::gui
