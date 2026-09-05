#include "app/programtab.hh"
#include "model/functionlistmodel.hh"
#include "views/codeview.hh"
#include "views/decompilerview.hh"
#include "views/hexpane.hh"
#include "views/hexview.hh"
#include "model/patchbuilder.hh"
#include "model/settings.hh"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLabel>
#include <QRegularExpression>
#include <QTimer>
#include <QStackedWidget>

#include <QVBoxLayout>

namespace astral::gui {

ProgramTab::ProgramTab(std::unique_ptr<ProgramDocument> document, QWidget *parent)
    : QWidget(parent), document_(std::move(document)),
      functions_(new FunctionListModel(this)), decompiler_(new DecompilerView),
      pseudo_(new DecompilerView), hex_(new HexView), hexPane_(new HexPane(hex_)),
      views_(new QStackedWidget)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    pseudo_->setPseudo(true);
    connect(decompiler_, &DecompilerView::compileRequested, this, [this] { compileCurrent(); });
    // Both source views offer the same actions on the word under the cursor.
    for (DecompilerView *view : {decompiler_, pseudo_})
        connect(view->codeView(), &CodeView::contextMenuAboutToShow, this,
                [this](QMenu *menu, const QString &word) { Q_EMIT contextActionsWanted(menu, word); });
    auto placeholder = [](const QString &text) {
        auto *label = new QLabel(text);
        label->setAlignment(Qt::AlignCenter);
        label->setObjectName(QStringLiteral("muted"));
        return label;
    };
    views_->addWidget(decompiler_);
    views_->addWidget(pseudo_);
    views_->addWidget(placeholder(tr("Control-flow graph arrives in a later step.")));
    views_->addWidget(hexPane_);
    hexPane_->setDocument(document_.get());
    connect(hexPane_, &HexPane::logMessage, this, &ProgramTab::logMessage);
    connect(hexPane_, &HexPane::patchApplied, this, &ProgramTab::patchApplied);
    connect(views_, &QStackedWidget::currentChanged, this, [this](int index) {
        if (index == Hex)
            refreshHex();
        Q_EMIT viewChanged(index);
    });
    layout->addWidget(views_, 1);

    functions_->setFunctions(document_->functions());
    connect(document_.get(), &ProgramDocument::functionsChanged, this, [this] {
        functions_->setFunctions(document_->functions());
    });
    // A patch rewrites the image, so what the views hold is a picture of code
    // that is no longer there. One refresh covers a burst of patches.
    connect(document_.get(), &ProgramDocument::patchesChanged, this, [this] {
        if (refreshPending_)
            return;
        refreshPending_ = true;
        QTimer::singleShot(120, this, [this] {
            refreshPending_ = false;
            refreshCurrent();
        });
    });

    connect(document_.get(), &ProgramDocument::functionReady, this, [this](const Decompiled &f) {
        if (f.address != current_)
            return;
        decompiler_->showFunction(f);
        pseudo_->showFunction(f);
        // The symbol table's size is often zero for a stripped binary; the
        // decompiler's measured span is the better listing bound.
        listing_ = document_->disassemble(f.address, f.size);
        Q_EMIT listingChanged(listing_);
        if (views_->currentIndex() == Hex)
            refreshHex();
    });
    connect(document_.get(), &ProgramDocument::functionFailed, this,
            [this](quint64 address, const QString &error) {
                if (address == current_) {
                    decompiler_->showError(error);
                    pseudo_->showError(error);
                }
            });
}

ProgramTab::~ProgramTab() = default;

void ProgramTab::showAddress(quint64 address)
{
    if (document_->functionAt(address)) {
        showFunction(address);
        if (views_->currentIndex() == Hex)
            views_->setCurrentIndex(Code);
        return;
    }
    hexAddress_ = address;
    if (views_->currentIndex() == Hex)
        refreshHex();
    else
        views_->setCurrentIndex(Hex);
}

void ProgramTab::refreshHex()
{
    // A window of the segment around the address: enough context to see
    // neighbours, small enough to render at once.
    constexpr quint64 kWindow = 16 * 1024;
    const quint64 focus = hexAddress_ ? hexAddress_ : current_;
    const auto cachedFunction = document_->cached(focus);
    quint64 size = cachedFunction ? cachedFunction->size
                                  : document_->functionAt(focus).value_or(FunctionEntry{}).size;
    if (size == 0)
        size = 1;
    quint64 start = focus & ~quint64(15);
    quint64 end = start + qMax<quint64>(size, 16) + kWindow / 2;
    start = start > kWindow / 2 ? start - kWindow / 2 : 0;
    for (const SegmentEntry &seg : document_->segments()) {
        if (focus >= seg.address && focus < seg.address + seg.size) {
            start = qMax(start, seg.address & ~quint64(15));
            end = qMin(end, seg.address + seg.size);
            break;
        }
    }
    hex_->showBytes(start, document_->read(start, end - start), focus, size);
}

void ProgramTab::setView(View view)
{
    views_->setCurrentIndex(static_cast<int>(view));
}

void ProgramTab::reportPatchWritten()
{
    decompiler_->showPatchWritten();
}

void ProgramTab::reportPatchFailed(const QString &reason)
{
    decompiler_->showRefused(reason);
}

QString ProgramTab::currentWord() const
{
    switch (views_->currentIndex()) {
    case Code:
        return decompiler_->codeView()->wordUnderCursor();
    case PseudoC:
        return pseudo_->codeView()->wordUnderCursor();
    default:
        return QString();
    }
}

void ProgramTab::refreshCurrent()
{
    if (current_ == 0)
        return;
    // The pill says what is happening; the patch verdict is in the log and
    // the status text, which showFunction leaves alone.
    const auto entry = document_->functionAt(current_);
    const QString name = entry ? entry->name : QStringLiteral("sub%1").arg(current_, 0, 16);
    decompiler_->showPending(name, current_);
    pseudo_->showPending(name, current_);
    listing_ = document_->disassemble(current_, entry ? entry->size : 0);
    Q_EMIT listingChanged(listing_);
    if (views_->currentIndex() == Hex)
        refreshHex();
    document_->decompile(current_);
}

void ProgramTab::replaceCodeText(const QString &text)
{
    decompiler_->setText(text);
}

void ProgramTab::compileCurrent()
{
    // Astral assembles and writes bytes itself. Turning source back into
    // machine code is the one step it has no engine for, so it runs a C
    // compiler, and the settings file says whether that is wanted.
    if (!Settings::instance().boolValue(QStringLiteral("patch.useCCompiler"), true)) {
        Q_EMIT logMessage(tr("patch refused: patching from source needs a C compiler, and "
                             "patch.useCCompiler is off in %1").arg(Settings::path()));
        decompiler_->showRefused(tr("patch.useCCompiler is off"));
        return;
    }
    const auto entry = document_->functionAt(current_);
    const auto cachedFunction = document_->cached(current_);
    QString name = cachedFunction ? cachedFunction->name : entry ? entry->name : QString();
    if (name.isEmpty()) {
        Q_EMIT logMessage(tr("patch: no function is shown"));
        return;
    }
    // The signature line names the function as emitted, which is what the
    // object will define; the symbol table may know it by an older name.
    static const QRegularExpression signatureName(QStringLiteral(R"(\b([A-Za-z_][A-Za-z0-9_]*)\s*\()"));
    if (cachedFunction) {
        const auto m = signatureName.match(cachedFunction->signature);
        if (m.hasMatch())
            name = m.captured(1);
    }
    const quint64 span = cachedFunction ? cachedFunction->size : entry ? entry->size : 0;
    decompiler_->showPatching();
    auto *builder = new PatchBuilder(document_.get(), this);
    builder->build(decompiler_->text(), name, current_, span, [this, builder, name](const PatchOutcome &outcome) {
        builder->deleteLater();
        if (outcome.ok) {
            Q_EMIT logMessage(tr("patch %1").arg(outcome.report));
            decompiler_->showPatchQueued();
            Q_EMIT patchApplied();
            return;
        }
        if (!outcome.diagnostics.isEmpty())
            Q_EMIT logMessage(tr("patch %1:\n%2").arg(name, outcome.diagnostics));
        else
            Q_EMIT logMessage(tr("patch refused: %1").arg(outcome.report));
        decompiler_->showCompileResult(false, outcome.errors > 0 ? outcome.errors : 1);
        if (outcome.diagnostics.isEmpty())
            decompiler_->showRefused(outcome.report);
    });
}

ProgramTab::View ProgramTab::view() const
{
    return static_cast<View>(views_->currentIndex());
}

void ProgramTab::showFunction(quint64 address)
{
    current_ = address;
    hexAddress_ = 0;
    const auto entry = document_->functionAt(address);
    const QString name = entry ? entry->name : QStringLiteral("sub%1").arg(address, 0, 16);
    decompiler_->showPending(name, address);
    pseudo_->showPending(name, address);
    listing_ = document_->disassemble(address, entry ? entry->size : 0);
    Q_EMIT listingChanged(listing_);
    Q_EMIT locationChanged(address, name);
    document_->decompile(address);
}

bool ProgramTab::navigateTo(const QString &target)
{
    const QString text = target.trimmed();
    if (text.isEmpty())
        return false;
    if (const auto byName = document_->functionNamed(text)) {
        showFunction(byName->address);
        return true;
    }
    bool ok = false;
    QString hex = text;
    if (hex.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        hex = hex.mid(2);
    const quint64 address = hex.toULongLong(&ok, 16);
    if (!ok)
        return false;
    showFunction(address);
    return true;
}

} // namespace astral::gui
