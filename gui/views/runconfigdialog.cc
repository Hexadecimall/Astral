#include "views/runconfigdialog.hh"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace astral::gui {

RunConfigDialog::RunConfigDialog(const QString &program, std::vector<RunConfiguration> configurations,
                                 const QString &chosen, QWidget *parent)
    : QDialog(parent), program_(program), configurations_(std::move(configurations))
{
    setWindowTitle(tr("Run Configurations"));
    resize(640, 420);

    auto *outer = new QVBoxLayout(this);
    auto *columns = new QHBoxLayout;
    outer->addLayout(columns, 1);

    // Left: the configurations there are, and the buttons that change which.
    auto *left = new QVBoxLayout;
    list_ = new QListWidget;
    list_->setMinimumWidth(170);
    left->addWidget(list_, 1);
    auto *buttons = new QHBoxLayout;
    auto *addButton = new QPushButton(tr("Add"));
    auto *copyButton = new QPushButton(tr("Duplicate"));
    auto *removeButton = new QPushButton(tr("Remove"));
    for (QPushButton *button : {addButton, copyButton, removeButton})
        buttons->addWidget(button);
    left->addLayout(buttons);
    columns->addLayout(left);

    // Right: what the chosen one says.
    auto *form = new QFormLayout;
    name_ = new QLineEdit;
    arguments_ = new QLineEdit;
    arguments_->setPlaceholderText(tr("separated by spaces, argv[0] is added"));
    entry_ = new QLineEdit;
    entry_->setPlaceholderText(tr("a function name or an address; empty is the entry point"));
    entry_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    input_ = new QPlainTextEdit;
    input_->setPlaceholderText(tr("what the program reads"));
    input_->setMaximumHeight(90);
    stepLimit_ = new QSpinBox;
    stepLimit_->setRange(0, 1000000000);
    stepLimit_->setSingleStep(100000);
    stepLimit_->setSpecialValueText(tr("no limit set"));
    stepLimit_->setToolTip(tr("Instructions to allow before stopping a run that will not end"));
    stopAtStart_ = new QCheckBox(tr("Stop at the first instruction"));
    form->addRow(tr("Name"), name_);
    form->addRow(tr("Arguments"), arguments_);
    form->addRow(tr("Start at"), entry_);
    form->addRow(tr("Input"), input_);
    form->addRow(tr("Step limit"), stepLimit_);
    form->addRow(QString(), stopAtStart_);
    auto *right = new QVBoxLayout;
    auto *heading = new QLabel(QFileInfo(program).fileName());
    heading->setObjectName(QStringLiteral("welcomeHeading"));
    right->addWidget(heading);
    right->addLayout(form);
    right->addStretch(1);
    columns->addLayout(right, 1);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    outer->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, [this] {
        takeCurrent();
        accept();
    });
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(addButton, &QPushButton::clicked, this, &RunConfigDialog::add);
    connect(copyButton, &QPushButton::clicked, this, &RunConfigDialog::duplicate);
    connect(removeButton, &QPushButton::clicked, this, &RunConfigDialog::remove);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        // What was being edited is kept before moving on to another.
        takeCurrent();
        current_ = row;
        showCurrent();
    });

    for (const RunConfiguration &one : configurations_)
        list_->addItem(one.name);
    int start = 0;
    for (size_t i = 0; i < configurations_.size(); ++i)
        if (configurations_[i].name == chosen)
            start = static_cast<int>(i);
    current_ = start;
    list_->setCurrentRow(start);
    showCurrent();
}

QString RunConfigDialog::chosen() const
{
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(configurations_.size()))
        return QString();
    return configurations_[row].name;
}

void RunConfigDialog::showCurrent()
{
    const bool valid = current_ >= 0 && current_ < static_cast<int>(configurations_.size());
    const std::initializer_list<QWidget *> fields = {name_, arguments_, entry_, input_, stepLimit_,
                                                     stopAtStart_};
    for (QWidget *widget : fields)
        widget->setEnabled(valid);
    if (!valid) {
        name_->clear();
        arguments_->clear();
        entry_->clear();
        input_->clear();
        return;
    }
    const RunConfiguration &one = configurations_[current_];
    name_->setText(one.name);
    arguments_->setText(one.arguments.join(QLatin1Char(' ')));
    entry_->setText(one.entry);
    input_->setPlainText(one.input);
    stepLimit_->setValue(static_cast<int>(one.stepLimit));
    stopAtStart_->setChecked(one.stopAtStart);
}

void RunConfigDialog::takeCurrent()
{
    if (current_ < 0 || current_ >= static_cast<int>(configurations_.size()))
        return;
    RunConfiguration &one = configurations_[current_];
    one.name = name_->text().trimmed().isEmpty() ? one.name : name_->text().trimmed();
    one.arguments = arguments_->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    one.entry = entry_->text().trimmed();
    one.input = input_->toPlainText();
    one.stepLimit = static_cast<quint64>(stepLimit_->value());
    one.stopAtStart = stopAtStart_->isChecked();
    list_->item(current_)->setText(one.name);
}

void RunConfigDialog::add()
{
    takeCurrent();
    RunConfiguration one = RunConfigurations::byDefault();
    one.name = tr("Run %1").arg(configurations_.size() + 1);
    configurations_.push_back(one);
    list_->addItem(one.name);
    list_->setCurrentRow(list_->count() - 1);
}

void RunConfigDialog::duplicate()
{
    takeCurrent();
    if (current_ < 0 || current_ >= static_cast<int>(configurations_.size()))
        return;
    RunConfiguration one = configurations_[current_];
    one.name = tr("%1 copy").arg(one.name);
    configurations_.push_back(one);
    list_->addItem(one.name);
    list_->setCurrentRow(list_->count() - 1);
}

void RunConfigDialog::remove()
{
    if (current_ < 0 || current_ >= static_cast<int>(configurations_.size()))
        return;
    // One always remains, so there is always something to run.
    if (configurations_.size() == 1)
        return;
    configurations_.erase(configurations_.begin() + current_);
    const int row = current_;
    current_ = -1;
    delete list_->takeItem(row);
    list_->setCurrentRow(std::min(row, list_->count() - 1));
}

} // namespace astral::gui
