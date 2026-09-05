#include "views/optionsdialog.hh"

#include "model/programdocument.hh"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace astral::gui {

namespace {

// The word the settings file and the dialog both use for a state.
QString onOff(bool on)
{
    return on ? QStringLiteral("on") : QStringLiteral("off");
}

} // namespace

OptionsDialog::OptionsDialog(ProgramDocument *document, QWidget *parent)
    : QDialog(parent), document_(document)
{
    setWindowTitle(tr("Decompiler Settings"));
    setModal(true);
    if (document_ != nullptr) {
        programKey_ = DecompilerSettings::programKey(document_->path());
        programName_ = QFileInfo(document_->path()).fileName();
    }

    auto *layout = new QVBoxLayout(this);

    auto *scopeRow = new QHBoxLayout;
    scopeRow->addWidget(new QLabel(tr("These settings apply to"), this));
    scope_ = new QComboBox(this);
    scope_->addItem(tr("every program"));
    if (!programName_.isEmpty())
        scope_->addItem(tr("%1 only").arg(programName_));
    scope_->setToolTip(tr("A value set for every program is the one used unless the program "
                          "being read has its own. Written to the settings file under "
                          "decompiler.<name>, or decompiler.program.<program>.<name>."));
    scopeRow->addWidget(scope_, 1);
    layout->addLayout(scopeRow);

    tabs_ = new QTabWidget(this);
    layout->addWidget(tabs_, 1);
    buildGroups();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply
                                             | QDialogButtonBox::Cancel
                                             | QDialogButtonBox::RestoreDefaults,
                                         this);
    buttons->button(QDialogButtonBox::RestoreDefaults)->setText(tr("Reset Everything"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::clicked, this, &OptionsDialog::onButton);

    connect(scope_, &QComboBox::currentIndexChanged, this, [this] { reload(); });
    reload();
    resize(720, 620);
}

void OptionsDialog::buildGroups()
{
    for (const QString &group : DecompilerSettings::groups()) {
        auto *page = new QWidget;
        auto *pageLayout = new QVBoxLayout(page);
        auto *form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

        for (const OptionInfo &info : DecompilerSettings::options()) {
            if (info.group != group)
                continue;
            Row row;
            row.info = info;
            row.control = buildControl(info);
            row.note = new QLabel(page);
            // One line, beside the control rather than under it: a wrapping
            // label makes the row taller than the form laid out room for, and
            // the text is a reminder rather than something to read.
            row.note->setWordWrap(false);
            row.note->setEnabled(false);

            // The name is what someone editing the file by hand has to type,
            // so it is in reach of every control rather than in one list.
            QString tip = tr("<b>%1</b><br>%2<br><br>Default: %3")
                              .arg(info.name, info.explanation, info.defaultValue);
            if (!info.engineName.isEmpty() && info.engineName != info.name)
                tip += tr("<br>Decompiler option: %1").arg(info.engineName);
            if (info.needsReanalysis)
                tip += tr("<br>Shows once the function is analysed again.");
            row.control->setToolTip(tip);

            auto *label = new QLabel(info.label, page);
            label->setToolTip(tip);
            row.note->setToolTip(tip);
            row.control->setMinimumWidth(150);
            row.control->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            auto *field = new QHBoxLayout;
            field->setContentsMargins(0, 0, 0, 0);
            field->addWidget(row.control);
            field->addWidget(row.note, 1);
            form->addRow(label, field);
            rows_.push_back(row);
        }
        pageLayout->addLayout(form);
        pageLayout->addStretch(1);

        auto *reset = new QPushButton(tr("Reset This Group to Defaults"), page);
        connect(reset, &QPushButton::clicked, this, [this, group] { resetGroup(group); });
        pageLayout->addWidget(reset, 0, Qt::AlignLeft);

        auto *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        tabs_->addTab(scroll, group);
    }
}

QWidget *OptionsDialog::buildControl(const OptionInfo &info)
{
    switch (info.kind) {
    case OptionInfo::Kind::Boolean: {
        auto *box = new QCheckBox(this);
        connect(box, &QCheckBox::toggled, this, [this] { refreshNotes(); });
        return box;
    }
    case OptionInfo::Kind::Integer: {
        auto *spin = new QSpinBox(this);
        spin->setRange(info.minimum, info.maximum);
        connect(spin, &QSpinBox::valueChanged, this, [this] { refreshNotes(); });
        return spin;
    }
    case OptionInfo::Kind::Choice:
        break;
    }
    if (info.choices.isEmpty()) {
        // A prototype model, whose names come from the compiler specification
        // the open program was read with. Only the engine can judge one.
        auto *edit = new QLineEdit(this);
        connect(edit, &QLineEdit::textChanged, this, [this] { refreshNotes(); });
        return edit;
    }
    auto *combo = new QComboBox(this);
    combo->addItems(info.choices);
    connect(combo, &QComboBox::currentIndexChanged, this, [this] { refreshNotes(); });
    return combo;
}

QString OptionsDialog::readControl(const Row &row) const
{
    if (auto *box = qobject_cast<QCheckBox *>(row.control))
        return onOff(box->isChecked());
    if (auto *spin = qobject_cast<QSpinBox *>(row.control))
        return QString::number(spin->value());
    if (auto *combo = qobject_cast<QComboBox *>(row.control))
        return combo->currentText();
    if (auto *edit = qobject_cast<QLineEdit *>(row.control))
        return edit->text();
    return QString();
}

void OptionsDialog::writeControl(const Row &row, const QString &value)
{
    // Setting a control fires its signal; the notes are refreshed once the
    // whole page is loaded rather than after every field.
    const QSignalBlocker blocker(row.control);
    if (auto *box = qobject_cast<QCheckBox *>(row.control)) {
        box->setChecked(value == QStringLiteral("on"));
        return;
    }
    if (auto *spin = qobject_cast<QSpinBox *>(row.control)) {
        spin->setValue(value.toInt());
        return;
    }
    if (auto *combo = qobject_cast<QComboBox *>(row.control)) {
        const int index = combo->findText(value);
        combo->setCurrentIndex(index < 0 ? 0 : index);
        return;
    }
    if (auto *edit = qobject_cast<QLineEdit *>(row.control))
        edit->setText(value);
}

void OptionsDialog::reload()
{
    const QString key = scope_->currentIndex() == 1 ? programKey_ : QString();
    for (const Row &row : rows_)
        writeControl(row, DecompilerSettings::value(row.info.name, key));
    refreshNotes();
}

void OptionsDialog::refreshNotes()
{
    const bool perProgram = scope_->currentIndex() == 1;
    for (const Row &row : rows_) {
        const QString shown = readControl(row);
        QStringList parts;
        parts << tr("default %1").arg(row.info.defaultValue);
        if (shown != row.info.defaultValue)
            parts << tr("changed");
        if (perProgram && DecompilerSettings::overridden(row.info.name, programKey_))
            parts << tr("this program's own");
        if (row.info.needsReanalysis)
            parts << tr("analysis");
        row.note->setText(QStringLiteral("%1  ·  %2")
                              .arg(row.info.name, parts.join(QStringLiteral("  ·  "))));
    }
}

void OptionsDialog::resetGroup(const QString &group)
{
    const QString key = scope_->currentIndex() == 1 ? programKey_ : QString();
    DecompilerSettings::resetGroup(group, key);
    reload();
}

bool OptionsDialog::apply(QStringList &problems)
{
    const QString key = scope_->currentIndex() == 1 ? programKey_ : QString();
    bool printing = false;
    bool analysis = false;
    for (const Row &row : rows_) {
        const QString shown = readControl(row);
        const QString before = DecompilerSettings::value(row.info.name, key);
        if (shown == before)
            continue;
        // A value put back to what the level below it holds is a value the
        // file should stop carrying, rather than one to write again.
        const QString below = key.isEmpty() ? row.info.defaultValue
                                            : DecompilerSettings::globalValue(row.info.name);
        if (shown == below) {
            DecompilerSettings::clearValue(row.info.name, key);
        } else {
            QString error;
            if (!DecompilerSettings::applyAndSet(document_, row.info.name, shown, key, &error)) {
                problems << tr("%1: %2").arg(row.info.name, error);
                // Put the control back, so the dialog never shows a value the
                // decompiler did not take.
                writeControl(row, before);
                continue;
            }
        }
        if (row.info.needsReanalysis)
            analysis = true;
        else
            printing = true;
    }

    // Whatever was cleared has to reach the program too, and the simplest way
    // to be sure of that is to hand it the whole set again.
    if (document_ != nullptr) {
        QStringList refused;
        DecompilerSettings::applyTo(*document_, refused);
        problems << refused;
    }
    refreshNotes();
    if (printing || analysis)
        Q_EMIT printingChanged();
    if (analysis)
        Q_EMIT analysisChanged();
    return problems.isEmpty();
}

void OptionsDialog::onButton(QAbstractButton *button)
{
    auto *box = qobject_cast<QDialogButtonBox *>(button->parent());
    if (box == nullptr)
        return;
    const QDialogButtonBox::StandardButton which = box->standardButton(button);
    if (which == QDialogButtonBox::Cancel) {
        reject();
        return;
    }
    if (which == QDialogButtonBox::RestoreDefaults) {
        const QString key = scope_->currentIndex() == 1 ? programKey_ : QString();
        DecompilerSettings::resetAll(key);
        reload();
        return;
    }
    QStringList problems;
    apply(problems);
    if (!problems.isEmpty())
        QMessageBox::warning(this, tr("Some settings were refused"),
                             tr("The decompiler would not take these, so they were not kept:\n\n%1")
                                 .arg(problems.join(QLatin1Char('\n'))));
    if (which == QDialogButtonBox::Ok)
        accept();
}

bool OptionsDialog::setShown(const QString &name, const QString &value)
{
    for (const Row &row : rows_)
        if (row.info.name == name) {
            writeControl(row, value);
            refreshNotes();
            return true;
        }
    return false;
}

void OptionsDialog::setPerProgram(bool own)
{
    if (own && scope_->count() > 1)
        scope_->setCurrentIndex(1);
    else
        scope_->setCurrentIndex(0);
}

QString OptionsDialog::describe() const
{
    const QString key = scope_->currentIndex() == 1 ? programKey_ : QString();
    QString out = QStringLiteral("scope %1\n")
                      .arg(key.isEmpty() ? QStringLiteral("global") : key);
    QString group;
    for (const Row &row : rows_) {
        if (row.info.group != group) {
            group = row.info.group;
            out += QStringLiteral("group %1\n").arg(group);
        }
        out += QStringLiteral("  %1 = %2 (default %3, key %4)%5\n")
                   .arg(row.info.name, readControl(row), row.info.defaultValue,
                        DecompilerSettings::keyFor(row.info.name, key),
                        row.info.needsReanalysis ? QStringLiteral(" [analysis]") : QString());
    }
    return out;
}

} // namespace astral::gui
