#include "views/debugsettingsdialog.hh"

#include "model/runconfig.hh"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace astral::gui {

namespace {

QString onOff(bool on)
{
    return on ? QStringLiteral("on") : QStringLiteral("off");
}

} // namespace

DebugSettingsDialog::DebugSettingsDialog(QString program, QString configuration, QWidget *parent)
    : QDialog(parent), program_(std::move(program)), configuration_(std::move(configuration))
{
    setWindowTitle(tr("Debug Settings"));
    setModal(true);
    resize(720, 560);

    auto *layout = new QVBoxLayout(this);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("These settings apply to"), this));
    which_ = new QComboBox(this);
    for (const RunConfiguration &one : RunConfigurations::forProgram(program_))
        which_->addItem(one.name);
    if (which_->findText(configuration_) < 0 && !configuration_.isEmpty())
        which_->addItem(configuration_);
    which_->setCurrentText(configuration_);
    which_->setToolTip(tr("A way of running %1. Two of them are allowed to disagree about "
                          "everything below, which is the reason to have two.")
                           .arg(QFileInfo(program_).fileName()));
    row->addWidget(which_, 1);
    layout->addLayout(row);

    tabs_ = new QTabWidget(this);
    layout->addWidget(tabs_, 1);
    buildGroups();
    loadValues();
    updateAvailability();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                             QDialogButtonBox::RestoreDefaults,
                                         this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { applyValues(); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
            [this] {
                DebugSettings::reset(program_, configuration_);
                loadValues();
                updateAvailability();
            });
    // Changing which run is being edited saves what is on screen first, so a
    // half-made edit is not lost by looking at the other one.
    connect(which_, &QComboBox::currentTextChanged, this, [this](const QString &name) {
        applyValues();
        configuration_ = name;
        loadValues();
        updateAvailability();
    });
}

void DebugSettingsDialog::buildGroups()
{
    for (const QString &group : DebugSettings::groups()) {
        auto *page = new QWidget;
        auto *form = new QFormLayout(page);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        for (const DebugOption &option : DebugSettings::options()) {
            if (option.group != group)
                continue;
            QWidget *control = nullptr;
            switch (option.kind) {
            case DebugOption::Kind::Boolean: {
                auto *box = new QCheckBox(option.label, page);
                control = box;
                break;
            }
            case DebugOption::Kind::Integer: {
                auto *spin = new QSpinBox(page);
                spin->setRange(option.minimum, option.maximum);
                spin->setGroupSeparatorShown(true);
                control = spin;
                break;
            }
            case DebugOption::Kind::Choice: {
                auto *combo = new QComboBox(page);
                combo->addItems(option.choices);
                control = combo;
                break;
            }
            case DebugOption::Kind::Text: {
                auto *edit = new QLineEdit(page);
                control = edit;
                break;
            }
            }
            control->setToolTip(option.explanation);
            controls_.insert(option.name, control);
            // A checkbox says its own name, so it is not labelled twice.
            if (option.kind == DebugOption::Kind::Boolean)
                form->addRow(QString(), control);
            else
                form->addRow(option.label, control);
            // The explanation under the control rather than only in a tooltip:
            // a setting nobody understands is a setting nobody touches.
            auto *why = new QLabel(option.explanation, page);
            why->setObjectName(QStringLiteral("muted"));
            why->setWordWrap(true);
            form->addRow(QString(), why);
        }
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        tabs_->addTab(scroll, group);
        // Which engine is chosen decides what the rest of the dialog can do.
        if (auto *engine = qobject_cast<QComboBox *>(controls_.value(QStringLiteral("engine"))))
            connect(engine, &QComboBox::currentTextChanged, this,
                    [this] { updateAvailability(); });
    }
}

void DebugSettingsDialog::loadValues()
{
    for (const DebugOption &option : DebugSettings::options()) {
        const QString value = DebugSettings::value(program_, configuration_, option.name);
        QWidget *control = controls_.value(option.name);
        if (control == nullptr)
            continue;
        if (auto *box = qobject_cast<QCheckBox *>(control))
            box->setChecked(value == QStringLiteral("on"));
        else if (auto *spin = qobject_cast<QSpinBox *>(control))
            spin->setValue(value.toInt());
        else if (auto *combo = qobject_cast<QComboBox *>(control))
            combo->setCurrentText(value);
        else if (auto *edit = qobject_cast<QLineEdit *>(control))
            edit->setText(value);
    }
}

void DebugSettingsDialog::applyValues()
{
    for (const auto &pair : values().asKeyValueRange())
        DebugSettings::setValue(program_, configuration_, pair.first, pair.second);
}

QHash<QString, QString> DebugSettingsDialog::values() const
{
    QHash<QString, QString> out;
    for (const DebugOption &option : DebugSettings::options()) {
        QWidget *control = controls_.value(option.name);
        if (control == nullptr)
            continue;
        if (auto *box = qobject_cast<QCheckBox *>(control))
            out.insert(option.name, onOff(box->isChecked()));
        else if (auto *spin = qobject_cast<QSpinBox *>(control))
            out.insert(option.name, QString::number(spin->value()));
        else if (auto *combo = qobject_cast<QComboBox *>(control))
            out.insert(option.name, combo->currentText());
        else if (auto *edit = qobject_cast<QLineEdit *>(control))
            out.insert(option.name, edit->text());
    }
    return out;
}

// A setting the chosen engine cannot answer is shown and disabled rather than
// hidden, so it is clear that the option exists and which engine has it.
void DebugSettingsDialog::updateAvailability()
{
    auto *engine = qobject_cast<QComboBox *>(controls_.value(QStringLiteral("engine")));
    const bool live = engine != nullptr && engine->currentText() == QStringLiteral("live");
    for (const DebugOption &option : DebugSettings::options()) {
        QWidget *control = controls_.value(option.name);
        if (control == nullptr)
            continue;
        const bool available = live ? option.live : option.emulated;
        control->setEnabled(available);
        control->setToolTip(available ? option.explanation
                                      : tr("%1\n\nNot something a %2 run has.")
                                            .arg(option.explanation,
                                                 live ? tr("live") : tr("emulated")));
    }
}

} // namespace astral::gui
