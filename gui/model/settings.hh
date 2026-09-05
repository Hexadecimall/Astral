// Settings the user is meant to be able to read and change by hand, kept in
// one plain text file so a choice made in a dialog can be undone in an editor.
//
// Window layouts are not here: they are opaque blobs the platform writes, and
// nothing is gained by putting them where someone might try to edit them.
#ifndef ASTRAL_GUI_SETTINGS_HH
#define ASTRAL_GUI_SETTINGS_HH

#include <QString>
#include <QStringList>

#include <map>

namespace astral::gui {

class Settings {
public:
    // The one instance the application reads. Loads the file on first use.
    static Settings &instance();

    // Where the file lives, worked out while running so no path is compiled in.
    static QString path();

    bool boolValue(const QString &key, bool fallback) const;
    QString stringValue(const QString &key, const QString &fallback = QString()) const;
    int intValue(const QString &key, int fallback) const;
    // Reads `key.1`, `key.2` and so on, in order.
    QStringList listValue(const QString &key) const;

    void setBool(const QString &key, bool value);
    void setString(const QString &key, const QString &value);
    void setInt(const QString &key, int value);
    void setList(const QString &key, const QStringList &values);
    void remove(const QString &key);

    // Re-reads the file, picking up edits made outside the application.
    void reload();
    // Writes the file, keeping the explanatory header. Returns false and fills
    // `error` when the file cannot be written.
    bool save(QString *error = nullptr);

    // The text every fresh file starts from: defaults with a note on each.
    static QString defaultText();

private:
    Settings();
    // Appends any documented default the file does not mention yet.
    void addMissingDefaults();

    std::map<QString, QString> values_;
    // The file as it was read, so saving keeps its comments and its order.
    QStringList lines_;
};

} // namespace astral::gui

#endif
