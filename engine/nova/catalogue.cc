#include "catalogue.hh"

#include "semantics.hh"
#include "sleighbase.hh"
#include "slghsymbol.hh"
#include "specification.hh"

#include <sstream>

namespace astral_internal {
namespace nova {
namespace catalogue {

namespace {

// The subtable everything starts from. A specification builds instructions out
// of tables that refer to one another, and this is the one an instruction is
// read from first.
const char *const kRoot = "instruction";

// Operations a template holds that say nothing about what the instruction
// computes. A build is the specification stitching in another table, and a
// delay slot is a scheduling remark; neither is part of the meaning, and
// counting them would make almost every form look like it does several things.
bool is_bookkeeping(ghidra::OpCode opcode)
{
    return opcode == ghidra::CPUI_MULTIEQUAL || opcode == ghidra::CPUI_INDIRECT;
}

} // namespace

bool Catalogue::read(const ir::Target &target, std::string &error)
{
    forms_.clear();
    by_operation_.clear();

    ghidra::SleighArchitecture *architecture = specification_for(
        target.compiler.empty() ? target.language_id : target.language_id + ":" + target.compiler,
        error);
    if (architecture == nullptr)
        return false;

    const ghidra::SleighBase *sleigh =
        dynamic_cast<const ghidra::SleighBase *>(architecture->translate);
    if (sleigh == nullptr) {
        error = "this processor is not described by a specification that can be read this way";
        return false;
    }

    ghidra::SleighSymbol *symbol = sleigh->findSymbol(kRoot);
    ghidra::SubtableSymbol *root = dynamic_cast<ghidra::SubtableSymbol *>(symbol);
    if (root == nullptr) {
        error = std::string("this specification has no ") + kRoot + " table to read";
        return false;
    }

    for (int i = 0; i < root->getNumConstructors(); ++i) {
        ghidra::Constructor *made = root->getConstructor(i);
        if (made == nullptr)
            continue;

        Form form;
        form.operands = made->getNumOperands();
        form.shortest = made->getMinimumLength();
        form.line = made->getLineno();

        ghidra::ConstructTpl *templ = made->getTempl();
        if (templ != nullptr) {
            for (const ghidra::OpTpl *operation : templ->getOpvec()) {
                if (operation == nullptr || is_bookkeeping(operation->getOpcode()))
                    continue;
                form.does.push_back(operation->getOpcode());
            }
        }

        forms_.push_back(std::move(form));
    }

    // Indexed by what they do, and only the ones that do exactly one thing: a
    // form that does several can still be chosen, but not without reasoning
    // about the rest of what it did, which is a later question.
    for (const Form &form : forms_) {
        if (form.does.size() == 1)
            by_operation_[form.does.front()].push_back(&form);
    }
    return true;
}

const std::vector<const Form *> &Catalogue::doing(ghidra::OpCode opcode) const
{
    static const std::vector<const Form *> none;
    auto found = by_operation_.find(opcode);
    return found == by_operation_.end() ? none : found->second;
}

std::string Catalogue::summary() const
{
    size_t single = 0;
    size_t silent = 0;
    for (const Form &form : forms_) {
        if (form.does.size() == 1)
            ++single;
        else if (form.does.empty())
            ++silent;
    }

    std::ostringstream out;
    out << forms_.size() << " forms, " << single << " doing one thing, " << silent
        << " doing nothing on their own";
    return out.str();
}

} // namespace catalogue
} // namespace nova
} // namespace astral_internal
