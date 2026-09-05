#include "model/settings.hh"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

namespace astral::gui {

namespace {

// A `key = value` line, with `#` starting a comment. The same shape as the
// theme file and the knowledge base, so one format covers everything a user
// edits by hand.
QString keyOf(const QString &raw)
{
    QString line = raw;
    const int hash = line.indexOf(QLatin1Char('#'));
    if (hash >= 0)
        line = line.left(hash);
    const int equals = line.indexOf(QLatin1Char('='));
    return equals <= 0 ? QString() : line.left(equals).trimmed();
}

void parseInto(const QString &text, std::map<QString, QString> &values)
{
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        QString line = raw;
        const int hash = line.indexOf(QLatin1Char('#'));
        if (hash >= 0)
            line = line.left(hash);
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        values[line.left(equals).trimmed()] = line.mid(equals + 1).trimmed();
    }
}

} // namespace

Settings::Settings()
{
    reload();
}

Settings &Settings::instance()
{
    static Settings settings;
    return settings;
}

QString Settings::path()
{
    return QDir::homePath() + QStringLiteral("/.astral/configuration.astral");
}

QString Settings::defaultText()
{
    return QStringLiteral(R"(# Astral settings. One setting per line, `key = value`.
# Lines starting with # are ignored. Delete a line to return it to its default.
# Astral reads this file when it starts and writes it when a setting changes.

# Ask where to write the patched binary each time a patch is applied. Turning
# this off makes every patch overwrite the original, with a backup beside it.
patch.askWhereToWrite = true

# Offer to analyze a program when it opens.
analysis.offerOnOpen = true

# Show the summary when analysis finishes.
analysis.showRundown = true

# How many recent projects and binaries the welcome screen remembers.
recent.limit = 8

# How edited assembly becomes bytes. `engine` uses the assembler built into
# Astral and runs nothing else. `toolchain` runs the system assembler through
# a C compiler, which accepts more syntax and can rewrite a whole block, but
# needs a compiler installed and starts another program to do it.
patch.assembler = engine

# Patching from edited C needs a C compiler, which Astral does not contain.
# With this off, the source views are read-only and only assembly and hex can
# be patched.
patch.useCCompiler = true
)");
}

void Settings::reload()
{
    values_.clear();
    lines_.clear();
    QFile file(path());
    if (!file.exists()) {
        lines_ = defaultText().split(QLatin1Char('\n'));
        parseInto(defaultText(), values_);
        save();
        return;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    const QString text = QString::fromUtf8(file.readAll());
    lines_ = text.split(QLatin1Char('\n'));
    parseInto(text, values_);
    addMissingDefaults();
}

void Settings::addMissingDefaults()
{
    // A file written by an older build knows nothing of settings added since.
    // Each one arrives with the comment that explains it, so the file stays
    // able to teach whoever opens it.
    QStringList block;
    bool changed = false;
    for (const QString &line : defaultText().split(QLatin1Char('\n'))) {
        const QString key = keyOf(line);
        if (key.isEmpty()) {
            if (line.trimmed().startsWith(QLatin1Char('#')))
                block << line;
            else
                block.clear();
            continue;
        }
        if (!values_.count(key)) {
            while (!lines_.isEmpty() && lines_.last().trimmed().isEmpty())
                lines_.removeLast();
            lines_ << QString() << block << line;
            parseInto(line, values_);
            changed = true;
        }
        block.clear();
    }
    if (changed)
        save();
}

bool Settings::boolValue(const QString &key, bool fallback) const
{
    const auto found = values_.find(key);
    if (found == values_.end())
        return fallback;
    const QString value = found->second.toLower();
    if (value == QStringLiteral("true") || value == QStringLiteral("yes") || value == QStringLiteral("1"))
        return true;
    if (value == QStringLiteral("false") || value == QStringLiteral("no") || value == QStringLiteral("0"))
        return false;
    return fallback;
}

QString Settings::stringValue(const QString &key, const QString &fallback) const
{
    const auto found = values_.find(key);
    return found == values_.end() ? fallback : found->second;
}

int Settings::intValue(const QString &key, int fallback) const
{
    bool ok = false;
    const int value = stringValue(key).toInt(&ok);
    return ok ? value : fallback;
}

QStringList Settings::listValue(const QString &key) const
{
    QStringList out;
    for (int i = 1;; ++i) {
        const auto found = values_.find(QStringLiteral("%1.%2").arg(key).arg(i));
        if (found == values_.end())
            break;
        out << found->second;
    }
    return out;
}

void Settings::setBool(const QString &key, bool value)
{
    setString(key, value ? QStringLiteral("true") : QStringLiteral("false"));
}

void Settings::setInt(const QString &key, int value)
{
    setString(key, QString::number(value));
}

void Settings::setString(const QString &key, const QString &value)
{
    values_[key] = value;
    save();
}

void Settings::setList(const QString &key, const QStringList &values)
{
    // The old entries go first, or a shorter list would leave a tail behind.
    // Only the numbered ones: `recent.limit` is a setting, not an entry.
    static const QRegularExpression numbered(QStringLiteral("^\\.[0-9]+$"));
    for (auto it = values_.begin(); it != values_.end();) {
        const bool isEntry = it->first.startsWith(key)
                             && numbered.match(it->first.mid(key.size())).hasMatch();
        it = isEntry ? values_.erase(it) : std::next(it);
    }
    for (int i = 0; i < values.size(); ++i)
        values_[QStringLiteral("%1.%2").arg(key).arg(i + 1)] = values[i];
    save();
}

void Settings::remove(const QString &key)
{
    values_.erase(key);
    save();
}

bool Settings::save(QString *error)
{
    const QString target = path();
    QDir().mkpath(QFileInfo(target).absolutePath());
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error != nullptr)
            *error = file.errorString();
        return false;
    }

    // The file is rewritten line by line rather than regenerated, so the
    // comments explaining each setting survive every write.
    QStringList out;
    std::map<QString, bool> written;
    for (const QString &line : lines_) {
        const QString key = keyOf(line);
        if (key.isEmpty()) {
            out << line;
            continue;
        }
        const auto found = values_.find(key);
        if (found == values_.end())
            continue; // removed since the file was read
        out << QStringLiteral("%1 = %2").arg(key, found->second);
        written[key] = true;
    }
    // Anything set that the file did not already mention joins its family,
    // so the numbered entries of a list stay together, and lands at the end
    // when the file has nothing like it.
    for (const auto &[key, value] : values_) {
        if (written.count(key))
            continue;
        const QString line = QStringLiteral("%1 = %2").arg(key, value);
        const QString family = key.section(QLatin1Char('.'), 0, 0) + QLatin1Char('.');
        int after = -1;
        for (int i = 0; i < out.size(); ++i)
            if (keyOf(out[i]).startsWith(family))
                after = i;
        if (after >= 0) {
            out.insert(after + 1, line);
        } else {
            while (!out.isEmpty() && out.last().trimmed().isEmpty())
                out.removeLast();
            out << QString() << line;
        }
    }
    while (!out.isEmpty() && out.last().trimmed().isEmpty())
        out.removeLast();
    out << QString();

    file.write(out.join(QLatin1Char('\n')).toUtf8());
    if (!file.commit()) {
        if (error != nullptr)
            *error = file.errorString();
        return false;
    }
    lines_ = out;
    return true;
}

} // namespace astral::gui
