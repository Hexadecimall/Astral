#include "app/programtab.hh"
#include "model/functionlistmodel.hh"
#include "views/codeview.hh"
#include "views/decompilerview.hh"
#include "views/hexpane.hh"
#include "views/hexview.hh"
#include "views/listingpane.hh"
#include "views/listingview.hh"
#include "model/patchbuilder.hh"
#include "model/settings.hh"
#include "model/sourcepatcher.hh"

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
      pseudo_(new DecompilerView), centreListingView_(new ListingView),
      centreListing_(new ListingPane(centreListingView_)), hex_(new HexView),
      hexPane_(new HexPane(hex_)), views_(new QStackedWidget)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    pseudo_->setPseudo(true);
    for (DecompilerView *view : {decompiler_, pseudo_})
        connect(view, &DecompilerView::compileRequested, this, [this, view] { compileCurrent(view); });
    // Both source views offer the same actions on the word under the cursor.
    for (DecompilerView *view : {decompiler_, pseudo_})
        connect(view->codeView(), &CodeView::contextMenuAboutToShow, this,
                [this, view](QMenu *menu, const QString &word) {
                    Q_EMIT contextActionsWanted(menu, word,
                                                view->codeView()->textCursor().block().text());
                });
    auto placeholder = [](const QString &text) {
        auto *label = new QLabel(text);
        label->setAlignment(Qt::AlignCenter);
        label->setObjectName(QStringLiteral("muted"));
        return label;
    };
    // The order here is the order of the View enum, save for Nova and Pseudo-C
    // which are one widget saying the same function two ways.
    views_->addWidget(pseudo_);
    views_->addWidget(decompiler_);
    views_->addWidget(centreListing_);
    views_->addWidget(hexPane_);
    views_->addWidget(placeholder(tr("Control-flow graph arrives in a later step.")));
    connect(centreListing_, &ListingPane::logMessage, this, &ProgramTab::logMessage);
    connect(centreListing_, &ListingPane::patchApplied, this, &ProgramTab::patchApplied);
    connect(centreListingView_, &ListingView::navigateRequested, this, &ProgramTab::showAddress);
    hexPane_->setDocument(document_.get());
    connect(hexPane_, &HexPane::logMessage, this, &ProgramTab::logMessage);
    connect(hexPane_, &HexPane::patchApplied, this, &ProgramTab::patchApplied);
    connect(views_, &QStackedWidget::currentChanged, this, [this](int) {
        if (view_ == Hex)
            refreshHex();
        Q_EMIT viewChanged(static_cast<int>(view_));
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
        if (view_ == Assembly) {
            centreListing_->setListing(listing_);
            centreListing_->setProgram(document_.get(), current_);
        }
        Q_EMIT listingChanged(listing_);
        if (view_ == Hex)
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

bool ProgramTab::showAddress(quint64 address)
{
    if (document_->functionAt(address)) {
        showFunction(address);
        if (view_ == Hex)
            setView(Nova);
        return true;
    }
    // Inside a function rather than at the top of one, which is where a
    // debugger stops. Read the function it is in: going to the hex instead
    // answers a question nobody asked.
    if (const auto holding = document_->functionContaining(address)) {
        showFunction(holding->address);
        if (view_ == Hex)
            setView(Nova);
        return true;
    }
    // An address the image does not map is nowhere to go. Showing it anyway
    // asks the hex view for bytes that were never loaded and puts the cursor
    // outside what it holds, so it is refused here and said out loud by
    // whoever asked.
    bool mapped = false;
    for (const SegmentEntry &segment : document_->segments())
        mapped = mapped || (address >= segment.address &&
                            address < segment.address + segment.size);
    if (!mapped)
        return false;
    hexAddress_ = address;
    if (view_ == Hex)
        refreshHex();
    else
        setView(Hex);
    return true;
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

QString ProgramTab::viewName(View view)
{
    switch (view) {
    case Nova: return QCoreApplication::translate("ProgramTab", "Nova");
    case Code: return QCoreApplication::translate("ProgramTab", "C");
    case Assembly: return QCoreApplication::translate("ProgramTab", "Assembly");
    case PseudoC: return QCoreApplication::translate("ProgramTab", "Pseudo-C");
    case Hex: return QCoreApplication::translate("ProgramTab", "Hex");
    case Graph: return QCoreApplication::translate("ProgramTab", "Graph");
    }
    return QString();
}

void ProgramTab::setView(View view)
{
    const View was = view_;
    view_ = view;

    // Nova and Pseudo-C are the same listing written two ways, and which one
    // the engine writes is a setting. Changing it means the text already in
    // hand is in the other language, so the function is read again.
    if (view == Nova || view == PseudoC) {
        const QString wanted = view == Nova ? QStringLiteral("nova") : QStringLiteral("pseudo-c");
        if (document_->setting(QStringLiteral("readableLanguage")) != wanted) {
            QString error;
            document_->setSetting(QStringLiteral("readableLanguage"), wanted, error);
            if (current_ != 0) {
                document_->invalidate(current_);
                refreshCurrent();
            }
        }
    }

    switch (view) {
    case Nova:
    case PseudoC:
        views_->setCurrentWidget(pseudo_);
        break;
    case Code:
        views_->setCurrentWidget(decompiler_);
        break;
    case Assembly:
        views_->setCurrentWidget(centreListing_);
        centreListing_->setListing(listing_);
        centreListing_->setProgram(document_.get(), current_);
        break;
    case Hex:
        views_->setCurrentWidget(hexPane_);
        break;
    case Graph:
        views_->setCurrentIndex(views_->count() - 1);
        break;
    }
    if (view == Hex)
        refreshHex();
    if (view != was)
        Q_EMIT viewChanged(static_cast<int>(view));
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
    switch (view_) {
    case Code:
        return decompiler_->codeView()->wordUnderCursor();
    case Nova:
    case PseudoC:
        return pseudo_->codeView()->wordUnderCursor();
    case Assembly:
        return centreListingView_->wordUnderCursor();
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
    if (view_ == Hex)
        refreshHex();
    document_->decompile(current_);
}

void ProgramTab::replaceCodeText(const QString &text)
{
    // Into whichever view a patch would be built from, which is the one being
    // read. Writing it into the other one edits a document nobody compiles.
    ((view_ == Nova || view_ == PseudoC) ? pseudo_ : decompiler_)->setText(text);
}

void ProgramTab::compileCurrent(DecompilerView *view)
{
    if (view == nullptr)
        view = (view_ == Nova || view_ == PseudoC) ? pseudo_ : decompiler_;
    const auto entry = document_->functionAt(current_);
    const auto cachedFunction = document_->cached(current_);
    QString name = cachedFunction ? cachedFunction->name : entry ? entry->name : QString();
    if (name.isEmpty()) {
        Q_EMIT logMessage(tr("patch: no function is shown"));
        return;
    }
    // The signature line names the function as emitted, which is what the
    // compiler will define; the symbol table may know it by an older name.
    static const QRegularExpression signatureName(QStringLiteral(R"(\b([A-Za-z_][A-Za-z0-9_]*)\s*\()"));
    if (cachedFunction) {
        const auto m = signatureName.match(cachedFunction->signature);
        if (m.hasMatch())
            name = m.captured(1);
    }
    const quint64 span = cachedFunction ? cachedFunction->size : entry ? entry->size : 0;

    // Astral compiles for itself. Where it cannot yet, the settings file says
    // whether running a C compiler instead is wanted; it is not, by default.
    if (!SourcePatcher::supports(document_->languageId())) {
        const QString architecture = SourcePatcher::architectureName(document_->languageId());
        if (!Settings::instance().boolValue(QStringLiteral("patch.useCCompiler"), false)) {
            Q_EMIT logMessage(tr("patch refused: Astral's compiler writes arm64, not %1, so %2 "
                                 "cannot be patched from source. Editing the disassembly and "
                                 "editing the bytes both still work: those go through Astral's "
                                 "own assembler. Setting patch.useCCompiler in %3 lets it fall "
                                 "back to a C compiler on this machine.")
                                  .arg(architecture, name, Settings::path()));
            view->showRefused(tr("Astral cannot compile %1 yet").arg(architecture));
            return;
        }
        view->showPatching();
        auto *builder = new PatchBuilder(document_.get(), this);
        builder->build(view->text(), name, current_, span,
                       [this, builder, view, name](const PatchOutcome &outcome) {
                           builder->deleteLater();
                           if (outcome.ok) {
                               Q_EMIT logMessage(tr("patch %1").arg(outcome.report));
                               view->showPatchQueued();
                               Q_EMIT patchApplied();
                               return;
                           }
                           if (!outcome.diagnostics.isEmpty())
                               Q_EMIT logMessage(tr("patch %1:\n%2").arg(name, outcome.diagnostics));
                           else
                               Q_EMIT logMessage(tr("patch refused: %1").arg(outcome.report));
                           view->showCompileResult(false, outcome.errors > 0 ? outcome.errors : 1);
                           if (outcome.diagnostics.isEmpty())
                               view->showRefused(outcome.report);
                       });
        return;
    }

    // What Astral emitted for this function is what the bytes in the program
    // stand for, so it is the only trustworthy account of the code as it is.
    // Each view edits its own text, and the patch is the difference between
    // that text and what the view was given.
    QString before;
    if (cachedFunction)
        before = view == pseudo_ ? cachedFunction->pseudoCode : cachedFunction->code;
    // Nova is read by Nova's own front end. Pseudo-C is not a language anything
    // compiles, so an edit there is refused before it reaches a compiler.
    const SourcePatcher::Language language =
        (view == pseudo_ && view_ == Nova) ? SourcePatcher::Language::Nova
                                           : SourcePatcher::Language::C;
    if (view == pseudo_ && view_ == PseudoC) {
        Q_EMIT logMessage(tr("patch refused: Pseudo-C is a reading of the code, not a language "
                             "Astral compiles. Switch the source tab to Nova and edit there; "
                             "Nova says the same thing and compiles back into the program."));
        view->showRefused(tr("Pseudo-C does not compile; edit in Nova"));
        return;
    }

    view->showPatching();
    SourcePatcher patcher(document_.get(), this);
    const SourcePatchOutcome outcome =
        patcher.patch(before, view->text(), name, current_, span, language);
    if (outcome.ok && !outcome.changed) {
        Q_EMIT logMessage(tr("patch: %1").arg(outcome.report));
        view->showNothingToChange(outcome.report);
        return;
    }
    if (outcome.ok) {
        Q_EMIT logMessage(tr("patch %1").arg(outcome.report));
        view->showPatchQueued();
        Q_EMIT patchApplied();
        return;
    }
    if (!outcome.diagnostics.isEmpty())
        Q_EMIT logMessage(tr("patch refused: %1\n%2").arg(outcome.report, outcome.diagnostics));
    else
        Q_EMIT logMessage(tr("patch refused: %1").arg(outcome.report));
    view->showCompileResult(false, outcome.errors > 0 ? outcome.errors : 1);
    if (outcome.diagnostics.isEmpty())
        view->showRefused(outcome.report);
}

ProgramTab::View ProgramTab::view() const
{
    return view_;
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
    // Only somewhere the image has. A number that parses is not an address
    // this program contains, and going to one it does not have puts every
    // view in front of bytes that were never loaded.
    return showAddress(address);
}

} // namespace astral::gui
