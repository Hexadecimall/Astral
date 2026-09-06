#include "lower_ir.hh"

#include <map>

namespace astral_internal {
namespace nova {

namespace {

using compiler::Diagnostic;

// How wide a type is in bytes, or the target's word when nothing was said.
// A level-1 function says nothing about types at all, so this has to answer
// without one rather than treat its absence as a failure.
int width_of_type(TypePtr type, const ir::Target &target)
{
    const int word = target.word_bytes > 0 ? target.word_bytes : 8;
    const int pointer = target.pointer_bytes > 0 ? target.pointer_bytes : word;
    if (type == nullptr)
        return word;

    switch (type->kind) {
    case compiler::Type::Kind::Integer:
    case compiler::Type::Kind::Floating:
        return type->width > 0 ? type->width : word;
    case compiler::Type::Kind::Pointer:
    case compiler::Type::Kind::Function:
        return pointer;
    case compiler::Type::Kind::Array:
        return static_cast<int>(type->count) * width_of_type(type->target, target);
    case compiler::Type::Kind::Struct: {
        // A recovered record's members may overlap, so its size is where the
        // furthest one ends rather than the sum of them.
        int furthest = 0;
        for (const compiler::Type::Member &member : type->members) {
            const int ends = static_cast<int>(member.offset) + width_of_type(member.type, target);
            if (ends > furthest)
                furthest = ends;
        }
        return furthest;
    }
    case compiler::Type::Kind::Void:
        return 0;
    }
    return word;
}

// How wide a value is, asking the things that know in the order they know it.
//
// A type says so when there is one. Below level 2 there is not, and then the
// register says so instead: `@w0` and `@x0` are the same processor and four
// bytes apart, so taking the machine word would be wrong about one of them
// every time. Only when neither has an answer is the word a guess worth making.
int width_of_value(TypePtr type, const Storage &storage, const ir::Target &target);

bool type_is_signed(TypePtr type)
{
    return type != nullptr && type->kind == compiler::Type::Kind::Integer && type->is_signed;
}

int width_of_value(TypePtr type, const Storage &storage, const ir::Target &target)
{
    if (type != nullptr && type->kind != compiler::Type::Kind::Void)
        return width_of_type(type, target);
    if (storage.kind == Storage::Kind::Register ||
        storage.kind == Storage::Kind::EntryRegister) {
        const int width = target.register_width(storage.register_name);
        if (width > 0)
            return width;
    }
    return width_of_type(type, target);
}

// Where a local lives while the function runs. A local is a frame slot, and the
// slot is what its name means from then on.
struct Slot {
    int64_t offset = 0;
    TypePtr type = nullptr;
    int width = 0;
};

class Lowerer {
public:
    Lowerer(const Unit &unit, Types &types, const ir::Target &target, ir::Unit &out,
            std::vector<Diagnostic> &diagnostics)
        : unit_(unit), types_(types), target_(target), out_(out), diagnostics_(diagnostics)
    {
    }

    bool function(const Function &source, ir::Function &out);

private:
    void complain(const Where &where, const std::string &message)
    {
        Diagnostic diagnostic;
        diagnostic.line = where.line;
        diagnostic.column = where.column;
        diagnostic.message = message;
        diagnostics_.push_back(diagnostic);
        failed_ = true;
    }

    // A frame slot for a local, descending from the frame pointer the way a
    // frame grows.
    Slot &declare(const std::string &name, TypePtr type, const Where &where);
    const Slot *look_up(const std::string &name) const;

    ir::Value address_of_slot(const Slot &slot);

    bool statement(const Statement &statement);
    bool compound(const Statement &statement);
    bool declaration(const Statement &statement);
    bool if_statement(const Statement &statement);
    bool while_statement(const Statement &statement);
    bool do_while_statement(const Statement &statement);
    bool loop_statement(const Statement &statement);
    bool leave_loop(const Statement &statement, bool to_the_end);
    bool for_in_statement(const Statement &statement);
    bool match_statement(const Statement &statement);
    bool label_statement(const Statement &statement);
    bool goto_statement(const Statement &statement);

    // A block written under a name, made the first time the name is mentioned
    // whether that is the label or a goto reaching forward to it.
    uint32_t block_for_label(const std::string &name);
    bool return_statement(const Statement &statement);
    bool assembly(const Statement &statement);

    // Answers with the value the expression produces. An invalid value means it
    // could not be lowered, and a complaint has already been made.
    ir::Value expression(const Expression &expression);
    ir::Value binary(const Expression &expression);
    ir::Value unary(const Expression &expression);
    ir::Value assign(const Expression &expression);
    ir::Value call(const Expression &expression);
    ir::Value name(const Expression &expression);
    ir::Value cast(const Expression &expression);
    ir::Value conditional(const Expression &expression);
    ir::Value member(const Expression &expression);
    ir::Value index(const Expression &expression);

    // The address a member or an element is at, which is what both reading one
    // and writing one need.
    ir::Value address_of(const Expression &expression, int &width, TypePtr &type);

    // The width a binary operation works in. Its own type says so when it has
    // one; a level-1 expression has none, and then the operands do, which is
    // how `w0 + w0` comes out four bytes wide rather than eight.
    int width_of_operands(const Expression &expression) const;
    int width_of(const ir::Value &value) const;

    // What the whole file says, so a call can ask what the thing it calls
    // answers with. The tree's own types are filled in while checking and are
    // not there during this, and how wide the answer is decides which register
    // it comes back in.
    const Unit &unit_;
    Types &types_;
    const ir::Target &target_;
    ir::Unit &out_;
    std::vector<Diagnostic> &diagnostics_;

    ir::Builder *builder_ = nullptr;
    std::map<std::string, Slot> slots_;
    std::map<std::string, ir::Value> pinned_;  // parameters, by the name they were given
    // Where `break` and `continue` go, innermost last. A loop pushes one while
    // its body is lowered, so a break in a nested loop leaves the loop it is
    // written in rather than the outermost one.
    struct Loop {
        uint32_t again = 0;  // where `continue` goes
        uint32_t after = 0;  // where `break` goes
    };
    std::vector<Loop> loops_;

    // Blocks that have names, because a goto can reach one before it has been
    // written. The block is made when the name is first mentioned and filled in
    // when the label itself is reached.
    std::map<std::string, uint32_t> labels_;

    int64_t next_offset_ = 0;
    bool failed_ = false;
};

Slot &Lowerer::declare(const std::string &name, TypePtr type, const Where &where)
{
    const int width = width_of_type(type, target_);
    next_offset_ -= width;
    Slot slot;
    slot.offset = next_offset_;
    slot.type = type;
    slot.width = width;
    auto placed = slots_.emplace(name, slot);
    if (!placed.second)
        complain(where, name + " is declared twice in the same function");
    return placed.first->second;
}

int Lowerer::width_of(const ir::Value &value) const
{
    return width_of_value(value.type, value.storage, target_);
}

int Lowerer::width_of_operands(const Expression &expression) const
{
    if (expression.type != nullptr && expression.type->kind != compiler::Type::Kind::Void)
        return width_of_type(expression.type, target_);
    // No type of its own, so the operands answer. A pinned name is the register
    // it was pinned to and the register says how wide it is; a local is its
    // slot, and the slot was made the width the declaration asked for.
    //
    // Falling through to the machine word makes an addition between two
    // four-byte values answer in eight, which is not what was written and is
    // an instruction wider than it needed to be on every processor.
    for (const Expression *side : {expression.left.get(), expression.right.get()}) {
        if (side == nullptr || side->kind != Expression::Kind::Name)
            continue;
        auto parameter = pinned_.find(side->name);
        if (parameter != pinned_.end()) {
            const int width = width_of(parameter->second);
            if (width > 0)
                return width;
        }
        const Slot *slot = look_up(side->name);
        if (slot != nullptr && slot->width > 0)
            return slot->width;
    }
    return width_of_type(expression.type, target_);
}

const Slot *Lowerer::look_up(const std::string &name) const
{
    auto found = slots_.find(name);
    return found == slots_.end() ? nullptr : &found->second;
}

ir::Value Lowerer::address_of_slot(const Slot &slot)
{
    ir::Instruction instruction;
    instruction.operation = ir::Operation::FrameAddress;
    instruction.immediate = static_cast<uint64_t>(slot.offset);
    instruction.space = ir::Space::Frame;
    instruction.width = target_.pointer_bytes > 0 ? target_.pointer_bytes : 8;
    instruction.result = builder_->value();
    return builder_->emit(std::move(instruction));
}

// ------------------------------------------------------------- expressions

ir::Value Lowerer::name(const Expression &expression)
{
    // A parameter that was pinned is the value itself: at level 1 the register
    // is the name, and reading it through a frame slot would move a value that
    // the caller put exactly where it is.
    auto parameter = pinned_.find(expression.name);
    if (parameter != pinned_.end())
        return parameter->second;

    const Slot *slot = look_up(expression.name);
    if (slot == nullptr) {
        complain(expression.where, "nothing here is called " + expression.name);
        return ir::Value();
    }
    const ir::Value address = address_of_slot(*slot);
    return builder_->load(address, slot->width, ir::Space::Frame, slot->type);
}

// `value as u32`, which reinterprets a view over storage. The bits do not move
// unless the widths differ, and when they do it is the sign of what is being
// widened that decides what fills the top.
ir::Value Lowerer::cast(const Expression &expression)
{
    if (!expression.left) {
        complain(expression.where, "a cast with nothing to cast");
        return ir::Value();
    }
    const ir::Value inner = this->expression(*expression.left);
    if (!inner.is_valid())
        return ir::Value();

    const int from = width_of_value(expression.left->type, inner.storage, target_);
    const int to = width_of_type(expression.named_type, target_);
    if (to == from || to == 0)
        return inner;

    ir::Instruction instruction;
    instruction.operation = to > from ? ir::Operation::Extend : ir::Operation::Truncate;
    instruction.width = to;
    // Widening keeps the sign of what it came from, not of what it becomes: a
    // signed byte in a word is still negative.
    instruction.is_signed = type_is_signed(expression.left->type);
    instruction.arguments.push_back(inner);
    instruction.result = builder_->value(expression.named_type);
    return builder_->emit(std::move(instruction));
}

// `condition ? this : that`, which is an if that answers with something. The
// answer is kept in a slot of its own because both halves have to leave it in
// the same place for whatever reads it afterwards.
ir::Value Lowerer::conditional(const Expression &expression)
{
    if (!expression.left || !expression.right || !expression.third) {
        complain(expression.where, "a conditional missing one of its three parts");
        return ir::Value();
    }
    const ir::Value condition = this->expression(*expression.left);
    if (!condition.is_valid())
        return ir::Value();

    const int width = width_of_type(expression.type, target_);
    Slot answer;
    next_offset_ -= width > 0 ? width : 8;
    answer.offset = next_offset_;
    answer.type = expression.type;
    answer.width = width > 0 ? width : 8;

    const uint32_t deciding = builder_->current();
    const uint32_t when_true = builder_->block();
    const uint32_t when_false = builder_->block();
    const uint32_t after = builder_->block();

    builder_->resume(deciding);
    builder_->branch(condition, when_true, when_false);

    builder_->resume(when_true);
    const ir::Value taken = this->expression(*expression.right);
    if (!taken.is_valid())
        return ir::Value();
    builder_->store(address_of_slot(answer), taken, answer.width, ir::Space::Frame);
    builder_->jump(after);

    builder_->resume(when_false);
    const ir::Value otherwise = this->expression(*expression.third);
    if (!otherwise.is_valid())
        return ir::Value();
    builder_->store(address_of_slot(answer), otherwise, answer.width, ir::Space::Frame);
    builder_->jump(after);

    builder_->resume(after);
    return builder_->load(address_of_slot(answer), answer.width, ir::Space::Frame, answer.type);
}

ir::Value Lowerer::address_of(const Expression &expression, int &width, TypePtr &type)
{
    width = 0;
    type = nullptr;

    if (expression.kind == Expression::Kind::Member) {
        if (!expression.left) {
            complain(expression.where, "a member of nothing");
            return ir::Value();
        }
        // Where the thing itself is. A member is read through the address of
        // what holds it, which is a slot when it is a local and an address when
        // it is already one.
        ir::Value base;
        if (expression.left->kind == Expression::Kind::Name) {
            const Slot *slot = look_up(expression.left->name);
            if (slot == nullptr) {
                complain(expression.left->where, "nothing here is called " + expression.left->name);
                return ir::Value();
            }
            base = address_of_slot(*slot);
        } else {
            base = this->expression(*expression.left);
        }
        if (!base.is_valid())
            return ir::Value();

        // The member's offset within it, which the type says. The tree's own
        // type is filled in while checking and is not there yet, so the slot a
        // name stands for is what knows what it holds.
        TypePtr holding = expression.left->type;
        if (holding == nullptr && expression.left->kind == Expression::Kind::Name) {
            const Slot *slot = look_up(expression.left->name);
            if (slot != nullptr)
                holding = slot->type;
        }
        uint64_t offset = 0;
        bool found = false;
        if (holding != nullptr && holding->kind == compiler::Type::Kind::Struct) {
            for (const compiler::Type::Member &member : holding->members) {
                if (member.name == expression.name) {
                    offset = member.offset;
                    type = member.type;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            complain(expression.where, "nothing in this has a member called " + expression.name);
            return ir::Value();
        }
        width = width_of_type(type, target_);
        const int pointer = target_.pointer_bytes > 0 ? target_.pointer_bytes : 8;
        const ir::Value step = builder_->constant(offset, pointer);
        return builder_->binary(ir::Operation::Add, base, step, pointer, false);
    }

    if (expression.kind == Expression::Kind::Index) {
        if (!expression.left || !expression.right) {
            complain(expression.where, "an index with nothing to index");
            return ir::Value();
        }
        ir::Value base;
        TypePtr holding = expression.left->type;
        if (holding == nullptr && expression.left->kind == Expression::Kind::Name) {
            // A local says what it holds, and so does a parameter - which is
            // not a local and is not in the same place. Looking only at locals
            // left every `buffer[index]` over an argument with nothing to say
            // what an element of it is, which is most of the indexing there is
            // in a recovered function.
            const Slot *slot = look_up(expression.left->name);
            if (slot != nullptr)
                holding = slot->type;
            else {
                auto parameter = pinned_.find(expression.left->name);
                if (parameter != pinned_.end())
                    holding = parameter->second.type;
            }
        }
        if (expression.left->kind == Expression::Kind::Name) {
            const Slot *slot = look_up(expression.left->name);
            if (slot != nullptr && holding != nullptr &&
                holding->kind == compiler::Type::Kind::Array) {
                // An array's name is where it starts rather than something to
                // read: the elements are the thing, not a pointer to them.
                base = address_of_slot(*slot);
            } else {
                base = this->expression(*expression.left);
            }
        } else {
            base = this->expression(*expression.left);
        }
        if (!base.is_valid())
            return ir::Value();

        type = holding != nullptr ? holding->target : nullptr;
        if (type == nullptr) {
            complain(expression.where, "this is not something with elements in it");
            return ir::Value();
        }
        width = width_of_type(type, target_);

        const ir::Value which = this->expression(*expression.right);
        if (!which.is_valid())
            return ir::Value();
        const int pointer = target_.pointer_bytes > 0 ? target_.pointer_bytes : 8;
        // An element is as many bytes along as it is elements in, which is the
        // one multiplication an index means.
        const ir::Value size = builder_->constant(static_cast<uint64_t>(width), pointer);
        const ir::Value along =
            builder_->binary(ir::Operation::Multiply, which, size, pointer, false);
        return builder_->binary(ir::Operation::Add, base, along, pointer, false);
    }

    complain(expression.where, "this is not something with an address");
    return ir::Value();
}

ir::Value Lowerer::member(const Expression &expression)
{
    int width = 0;
    TypePtr type = nullptr;
    const ir::Value where = address_of(expression, width, type);
    if (!where.is_valid())
        return ir::Value();
    return builder_->load(where, width, ir::Space::Data, type);
}

ir::Value Lowerer::index(const Expression &expression)
{
    int width = 0;
    TypePtr type = nullptr;
    const ir::Value where = address_of(expression, width, type);
    if (!where.is_valid())
        return ir::Value();
    return builder_->load(where, width, ir::Space::Data, type);
}

ir::Value Lowerer::binary(const Expression &expression)
{
    // `and` and `or` are not operators over two values. They are a decision:
    // the right-hand side runs only when the left did not already settle the
    // answer, and that is what the words mean rather than an optimisation of
    // them. A recovered function relies on it - `index != length && text[index]
    // != 0` reads past the end of the text if the second half runs anyway - so
    // this is lowered as the branch it is.
    if ((expression.binary_op == compiler::BinaryOp::LogicalAnd ||
         expression.binary_op == compiler::BinaryOp::LogicalOr) &&
        expression.left && expression.right) {
        const bool settles_when_true = expression.binary_op == compiler::BinaryOp::LogicalOr;

        const ir::Value first = this->expression(*expression.left);
        if (!first.is_valid())
            return ir::Value();

        Slot answer;
        next_offset_ -= 1;
        answer.offset = next_offset_;
        answer.type = expression.type;
        answer.width = 1;

        const uint32_t deciding = builder_->current();
        const uint32_t settled = builder_->block();
        const uint32_t ask_again = builder_->block();
        const uint32_t after = builder_->block();

        builder_->resume(deciding);
        builder_->branch(first, settles_when_true ? settled : ask_again,
                         settles_when_true ? ask_again : settled);

        // The left-hand side decided it, so the answer is what it decided.
        builder_->resume(settled);
        builder_->store(address_of_slot(answer), builder_->constant(settles_when_true ? 1 : 0, 1),
                        answer.width, ir::Space::Frame);
        builder_->jump(after);

        // It did not, so the answer is whatever the right-hand side says - and
        // only now does the right-hand side run at all.
        builder_->resume(ask_again);
        const ir::Value second = this->expression(*expression.right);
        if (!second.is_valid())
            return ir::Value();
        builder_->store(address_of_slot(answer), second, answer.width, ir::Space::Frame);
        builder_->jump(after);

        builder_->resume(after);
        return builder_->load(address_of_slot(answer), answer.width, ir::Space::Frame,
                              answer.type);
    }

    // A comma is two things one after the other, and the second is the answer.
    // Recovered code uses it to say that something was assigned on the way
    // through a condition, so the left-hand side is run for what it does rather
    // than for what it is worth.
    if (expression.binary_op == compiler::BinaryOp::Comma && expression.left &&
        expression.right) {
        if (!this->expression(*expression.left).is_valid())
            return ir::Value();
        return this->expression(*expression.right);
    }

    const ir::Value left = expression.left ? this->expression(*expression.left) : ir::Value();
    const ir::Value right = expression.right ? this->expression(*expression.right) : ir::Value();
    if (!left.is_valid() || !right.is_valid())
        return ir::Value();

    const int width = width_of_operands(expression);
    const bool is_signed = type_is_signed(expression.type) ||
                           (expression.left && type_is_signed(expression.left->type));

    ir::Operation operation = ir::Operation::Add;
    switch (expression.binary_op) {
    case compiler::BinaryOp::Add: operation = ir::Operation::Add; break;
    case compiler::BinaryOp::Subtract: operation = ir::Operation::Subtract; break;
    case compiler::BinaryOp::Multiply: operation = ir::Operation::Multiply; break;
    case compiler::BinaryOp::Divide: operation = ir::Operation::Divide; break;
    case compiler::BinaryOp::Modulo: operation = ir::Operation::Remainder; break;
    case compiler::BinaryOp::ShiftLeft: operation = ir::Operation::ShiftLeft; break;
    case compiler::BinaryOp::ShiftRight: operation = ir::Operation::ShiftRight; break;
    case compiler::BinaryOp::BitAnd: operation = ir::Operation::BitAnd; break;
    case compiler::BinaryOp::BitOr: operation = ir::Operation::BitOr; break;
    case compiler::BinaryOp::BitXor: operation = ir::Operation::BitXor; break;
    case compiler::BinaryOp::Equal: operation = ir::Operation::Equal; break;
    case compiler::BinaryOp::NotEqual: operation = ir::Operation::NotEqual; break;
    case compiler::BinaryOp::Less: operation = ir::Operation::Less; break;
    case compiler::BinaryOp::LessEqual: operation = ir::Operation::LessOrEqual; break;
    // Greater is Less with the operands the other way round, which is one
    // operation fewer to select on every processor rather than two spellings
    // of the same comparison.
    case compiler::BinaryOp::Greater:
        return builder_->binary(ir::Operation::Less, right, left, width, is_signed,
                                expression.type);
    case compiler::BinaryOp::GreaterEqual:
        return builder_->binary(ir::Operation::LessOrEqual, right, left, width, is_signed,
                                expression.type);
    default:
        complain(expression.where, "this operator is not lowered yet");
        return ir::Value();
    }

    return builder_->binary(operation, left, right, width, is_signed, expression.type);
}

ir::Value Lowerer::unary(const Expression &expression)
{
    if (!expression.left) {
        complain(expression.where, "an operator with nothing to work on");
        return ir::Value();
    }
    const ir::Value inner = this->expression(*expression.left);
    if (!inner.is_valid())
        return ir::Value();

    const int width = width_of_type(expression.type, target_);
    ir::Instruction instruction;
    instruction.width = width;
    instruction.is_signed = type_is_signed(expression.type);
    instruction.arguments.push_back(inner);
    instruction.result = builder_->value(expression.type);

    switch (expression.unary_op) {
    case compiler::UnaryOp::Plus:
        return inner;
    case compiler::UnaryOp::Minus:
        instruction.operation = ir::Operation::Negate;
        break;
    case compiler::UnaryOp::BitNot:
        instruction.operation = ir::Operation::BitNot;
        break;
    case compiler::UnaryOp::Not:
        // Not a bit operation. `!value` asks whether the value is nothing, and
        // flipping its bits answers a different question: the negation of three
        // is minus four, and the negation of three as a truth is false.
        return builder_->binary(ir::Operation::Equal, inner,
                                builder_->constant(0, width > 0 ? width : 1), 1, false,
                                expression.type);
    case compiler::UnaryOp::PreIncrement:
    case compiler::UnaryOp::PreDecrement:
    case compiler::UnaryOp::PostIncrement:
    case compiler::UnaryOp::PostDecrement: {
        // Stepping something by one, and putting it back. The difference
        // between the two spellings is only which value the expression is: the
        // one before the step or the one after.
        const bool upwards = expression.unary_op == compiler::UnaryOp::PreIncrement ||
                             expression.unary_op == compiler::UnaryOp::PostIncrement;
        const bool answers_with_the_old =
            expression.unary_op == compiler::UnaryOp::PostIncrement ||
            expression.unary_op == compiler::UnaryOp::PostDecrement;

        const int stepping = width > 0 ? width : width_of(inner);
        const ir::Value stepped =
            builder_->binary(upwards ? ir::Operation::Add : ir::Operation::Subtract, inner,
                             builder_->constant(1, stepping), stepping,
                             type_is_signed(expression.left->type), expression.left->type);
        if (!stepped.is_valid())
            return ir::Value();

        // Where it goes back, which is wherever it came from.
        if (expression.left->kind == Expression::Kind::Name) {
            const Slot *slot = look_up(expression.left->name);
            if (slot == nullptr) {
                complain(expression.left->where,
                         "nothing here is called " + expression.left->name);
                return ir::Value();
            }
            builder_->store(address_of_slot(*slot), stepped, slot->width, ir::Space::Frame);
        } else if (expression.left->kind == Expression::Kind::Unary &&
                   expression.left->unary_op == compiler::UnaryOp::Dereference &&
                   expression.left->left) {
            const ir::Value address = this->expression(*expression.left->left);
            if (!address.is_valid())
                return ir::Value();
            builder_->store(address, stepped, stepping, ir::Space::Data);
        } else if (expression.left->kind == Expression::Kind::Index ||
                   expression.left->kind == Expression::Kind::Member) {
            int held = 0;
            TypePtr type = nullptr;
            const ir::Value where = address_of(*expression.left, held, type);
            if (!where.is_valid())
                return ir::Value();
            builder_->store(where, stepped, held, ir::Space::Data);
        } else {
            complain(expression.where, "this is not something that can be stepped");
            return ir::Value();
        }
        return answers_with_the_old ? inner : stepped;
    }
    case compiler::UnaryOp::AddressOf: {
        int held = 0;
        TypePtr type = nullptr;
        const ir::Value where = address_of(*expression.left, held, type);
        if (!where.is_valid())
            complain(expression.where, "there is no address for this to be the address of");
        return where;
    }
    case compiler::UnaryOp::Dereference:
        instruction.operation = ir::Operation::Load;
        instruction.space = ir::Space::Data;
        break;
    default:
        complain(expression.where, "this operator is not lowered yet");
        return ir::Value();
    }
    return builder_->emit(std::move(instruction));
}

ir::Value Lowerer::assign(const Expression &expression)
{
    if (!expression.left || !expression.right) {
        complain(expression.where, "an assignment with nothing on one side of it");
        return ir::Value();
    }
    const ir::Value held = this->expression(*expression.right);
    if (!held.is_valid())
        return ir::Value();

    if (expression.left->kind == Expression::Kind::Name) {
        const Slot *slot = look_up(expression.left->name);
        if (slot == nullptr) {
            complain(expression.left->where, "nothing here is called " + expression.left->name);
            return ir::Value();
        }
        const ir::Value address = address_of_slot(*slot);
        builder_->store(address, held, slot->width, ir::Space::Frame);
        return held;
    }

    if (expression.left->kind == Expression::Kind::Unary &&
        expression.left->unary_op == compiler::UnaryOp::Dereference && expression.left->left) {
        const ir::Value address = this->expression(*expression.left->left);
        if (!address.is_valid())
            return ir::Value();
        builder_->store(address, held, width_of_type(expression.left->type, target_),
                        ir::Space::Data);
        return held;
    }

    if (expression.left->kind == Expression::Kind::Member ||
        expression.left->kind == Expression::Kind::Index) {
        int width = 0;
        TypePtr type = nullptr;
        const ir::Value where = address_of(*expression.left, width, type);
        if (!where.is_valid())
            return ir::Value();
        builder_->store(where, held, width, ir::Space::Data);
        return held;
    }

    complain(expression.where, "this is not something that can be assigned to yet");
    return ir::Value();
}

ir::Value Lowerer::call(const Expression &expression)
{
    if (!expression.callee || expression.callee->kind != Expression::Kind::Name) {
        complain(expression.where, "only a call to a name is lowered yet");
        return ir::Value();
    }

    // Each argument, and how wide it is, because where it goes depends on that.
    std::vector<ir::Value> given;
    std::vector<int> widths;
    for (const ExpressionPtr &argument : expression.arguments) {
        if (!argument)
            continue;
        const ir::Value lowered = this->expression(*argument);
        if (!lowered.is_valid())
            return ir::Value();
        given.push_back(lowered);
        widths.push_back(width_of_value(argument->type, lowered.storage, target_));
    }

    // How wide the answer is, taken from what the called function was declared
    // to answer with. Guessing the machine word would ask for a register that
    // is right by accident on a processor whose narrow registers overlap its
    // wide ones, and wrong on one whose do not.
    TypePtr answers = expression.type;
    if (answers == nullptr) {
        for (const Function &other : unit_.functions) {
            if (other.name == expression.callee->name) {
                answers = other.result;
                break;
            }
        }
    }
    const int answers_with = width_of_type(answers, target_);
    std::vector<Storage> places;
    Storage answer_place;
    std::string error;
    if (!target_.calling_convention(widths, answers_with, places, answer_place, error)) {
        complain(expression.where,
                 "this processor's specification does not say how a call is made: " + error);
        return ir::Value();
    }

    // The arguments are put where the callee will look for them. This is a copy
    // into a place rather than an argument handed over: a call does not carry
    // values, it agrees with the callee about where they already are.
    ir::Instruction instruction;
    instruction.operation = ir::Operation::Call;
    instruction.callee = expression.callee->name;
    instruction.width = answers_with;
    for (size_t i = 0; i < given.size(); ++i) {
        const Storage where = i < places.size() ? places[i] : Storage();
        ir::Instruction put;
        put.operation = ir::Operation::Copy;
        put.width = widths[i];
        put.arguments.push_back(given[i]);
        put.result = builder_->value(nullptr, where);
        const ir::Value placed = builder_->emit(std::move(put));
        if (!placed.is_valid())
            return ir::Value();
        instruction.arguments.push_back(placed);
    }

    // And the answer comes back where the convention says it does.
    instruction.result = builder_->value(answers, answer_place);
    return builder_->emit(std::move(instruction));
}

ir::Value Lowerer::expression(const Expression &expression)
{
    switch (expression.kind) {
    case Expression::Kind::IntegerLiteral:
        return builder_->constant(expression.integer_value,
                                  width_of_type(expression.type, target_), expression.type);
    case Expression::Kind::BooleanLiteral:
        return builder_->constant(expression.boolean_value ? 1 : 0, 1, expression.type);
    case Expression::Kind::NullLiteral:
        return builder_->constant(0, target_.pointer_bytes > 0 ? target_.pointer_bytes : 8,
                                  expression.type);
    case Expression::Kind::StringLiteral: {
        // A string is not a value, it is the address of bytes that have to be
        // in the image. The bytes go into the unit and the expression becomes
        // the address of them, which is a number once somebody lays the image
        // out and a name until then.
        std::string called = "text_" + std::to_string(out_.data.size());
        for (const ir::Datum &already : out_.data) {
            if (already.bytes.size() == expression.text.size() + 1 &&
                std::equal(expression.text.begin(), expression.text.end(),
                           already.bytes.begin())) {
                called = already.name;
                break;
            }
        }
        if (called.rfind("text_", 0) == 0 && called == "text_" + std::to_string(out_.data.size())) {
            ir::Datum held;
            held.name = called;
            for (char letter : expression.text)
                held.bytes.push_back(static_cast<uint8_t>(letter));
            held.bytes.push_back(0);  // what a string ends with
            out_.data.push_back(std::move(held));
        }

        ir::Instruction instruction;
        instruction.operation = ir::Operation::GlobalAddress;
        instruction.width = target_.pointer_bytes > 0 ? target_.pointer_bytes : 8;
        instruction.symbol = called;
        instruction.where = expression.where;
        instruction.result = builder_->value(expression.type);
        return builder_->emit(std::move(instruction));
    }
    case Expression::Kind::Name:
        return name(expression);
    case Expression::Kind::Register: {
        // A register named where a value is expected is level 1 reading its own
        // storage, so the value is that register and nothing is loaded.
        ir::Instruction instruction;
        instruction.operation = ir::Operation::Copy;
        instruction.width = width_of_type(expression.type, target_);
        Storage storage;
        storage.kind = Storage::Kind::Register;
        storage.register_name = expression.name;
        instruction.result = builder_->value(expression.type, storage);
        return builder_->emit(std::move(instruction));
    }
    case Expression::Kind::Binary:
        return binary(expression);
    case Expression::Kind::Unary:
        return unary(expression);
    case Expression::Kind::Assign:
        return assign(expression);
    case Expression::Kind::Call:
        return call(expression);
    case Expression::Kind::As:
        return cast(expression);
    case Expression::Kind::Conditional:
        return conditional(expression);
    case Expression::Kind::Member:
        return member(expression);
    case Expression::Kind::Index:
        return index(expression);
    case Expression::Kind::SizeOf: {
        // How many bytes the thing takes, which is a number known here and so
        // is one by the time anything runs.
        const TypePtr measured =
            expression.named_type != nullptr
                ? expression.named_type
                : (expression.left != nullptr ? expression.left->type : nullptr);
        if (measured == nullptr) {
            complain(expression.where, "there is no saying how big this is");
            return ir::Value();
        }
        return builder_->constant(static_cast<uint64_t>(width_of_type(measured, target_)),
                                  target_.pointer_bytes > 0 ? target_.pointer_bytes : 8,
                                  expression.type);
    }
    case Expression::Kind::AddressOfName: {
        const Slot *slot = look_up(expression.name);
        if (slot == nullptr) {
            complain(expression.where, "nothing here is called " + expression.name);
            return ir::Value();
        }
        return address_of_slot(*slot);
    }
    default:
        complain(expression.where, "this expression is not lowered yet");
        return ir::Value();
    }
}

// -------------------------------------------------------------- statements

bool Lowerer::compound(const Statement &statement)
{
    for (const StatementPtr &inner : statement.body) {
        if (!inner)
            continue;
        // Statements after a return or a goto are still statements, and they
        // still have to go somewhere: control cannot arrive at them, but the
        // representation has no way to say "nowhere" and a block that has been
        // left cannot be added to. A block nothing jumps to says exactly that,
        // and it is what a later pass will drop.
        if (!builder_->block_is_open())
            builder_->block();
        if (!this->statement(*inner))
            return false;
    }
    return true;
}

bool Lowerer::declaration(const Statement &statement)
{
    for (const Variable &variable : statement.variables) {
        Slot &slot = declare(variable.name, variable.type, variable.where);
        if (variable.initialiser) {
            const ir::Value held = expression(*variable.initialiser);
            if (!held.is_valid())
                return false;
            const ir::Value address = address_of_slot(slot);
            builder_->store(address, held, slot.width, ir::Space::Frame);
        }
    }
    return true;
}

bool Lowerer::if_statement(const Statement &statement)
{
    if (!statement.value) {
        complain(statement.where, "an if with nothing to decide on");
        return false;
    }
    const ir::Value condition = expression(*statement.value);
    if (!condition.is_valid())
        return false;

    // Where the branch is written from, remembered before opening anything:
    // opening a block makes it the one being written into.
    const uint32_t deciding = builder_->current();

    const uint32_t taken = builder_->block();
    const uint32_t otherwise = statement.else_branch ? builder_->block() : 0;
    const uint32_t after = builder_->block();

    builder_->resume(deciding);
    builder_->branch(condition, taken, otherwise != 0 ? otherwise : after);

    // Each half joins back at `after` only if it did not already leave. A half
    // that ends in a return has gone, and sending it onwards as well is how a
    // function ends up leaving twice.
    builder_->resume(taken);
    if (statement.then_branch && !this->statement(*statement.then_branch))
        return false;
    if (builder_->block_is_open())
        builder_->jump(after);

    if (otherwise != 0) {
        builder_->resume(otherwise);
        if (statement.else_branch && !this->statement(*statement.else_branch))
            return false;
        if (builder_->block_is_open())
            builder_->jump(after);
    }

    builder_->resume(after);
    return true;
}

// while (condition) { body }
//
// The condition is tested in a block of its own rather than before the loop,
// because it is tested again on every turn and the body has to be able to
// reach it. That block is also where `continue` goes.
bool Lowerer::while_statement(const Statement &statement)
{
    if (!statement.value) {
        complain(statement.where, "a while with nothing to decide on");
        return false;
    }

    // Where control is now, remembered before opening anything: opening a block
    // makes it the one being written into.
    const uint32_t before = builder_->current();

    const uint32_t testing = builder_->block();
    const uint32_t body = builder_->block();
    const uint32_t after = builder_->block();

    // Falling into the loop is a jump to the test, from wherever control was.
    builder_->resume(before);
    builder_->jump(testing);

    builder_->resume(testing);
    const ir::Value condition = expression(*statement.value);
    if (!condition.is_valid())
        return false;
    builder_->branch(condition, body, after);

    loops_.push_back({testing, after});
    builder_->resume(body);
    const bool walked = statement.then_branch ? this->statement(*statement.then_branch) : true;
    loops_.pop_back();
    if (!walked)
        return false;
    // A body that already left does not go round again.
    if (builder_->block_is_open())
        builder_->jump(testing);

    builder_->resume(after);
    return true;
}

// do { body } while (condition)
//
// The body runs before anything is tested, so control enters it directly and
// the test sits after it. `continue` goes to the test, which is where the next
// turn is decided, and not back to the top.
bool Lowerer::do_while_statement(const Statement &statement)
{
    if (!statement.value) {
        complain(statement.where, "a do while with nothing to decide on");
        return false;
    }

    const uint32_t before = builder_->current();

    const uint32_t body = builder_->block();
    const uint32_t testing = builder_->block();
    const uint32_t after = builder_->block();

    builder_->resume(before);
    builder_->jump(body);

    loops_.push_back({testing, after});
    builder_->resume(body);
    const bool walked = statement.then_branch ? this->statement(*statement.then_branch) : true;
    loops_.pop_back();
    if (!walked)
        return false;
    if (builder_->block_is_open())
        builder_->jump(testing);

    builder_->resume(testing);
    const ir::Value condition = expression(*statement.value);
    if (!condition.is_valid())
        return false;
    builder_->branch(condition, body, after);

    builder_->resume(after);
    return true;
}

// loop { body }
//
// Nothing is tested. The only way out is something in the body breaking, and
// `after` exists so that break has somewhere to go; when nothing breaks it is
// left for the ending nobody wrote, the same as any other unreached block.
bool Lowerer::loop_statement(const Statement &statement)
{
    const uint32_t before = builder_->current();

    const uint32_t body = builder_->block();
    const uint32_t after = builder_->block();

    builder_->resume(before);
    builder_->jump(body);

    loops_.push_back({body, after});
    builder_->resume(body);
    const bool walked = statement.then_branch ? this->statement(*statement.then_branch) : true;
    loops_.pop_back();
    if (!walked)
        return false;
    if (builder_->block_is_open())
        builder_->jump(body);

    builder_->resume(after);
    return true;
}

bool Lowerer::leave_loop(const Statement &statement, bool to_the_end)
{
    if (loops_.empty()) {
        complain(statement.where, to_the_end ? "a break outside any loop"
                                             : "a continue outside any loop");
        return false;
    }
    const Loop &innermost = loops_.back();
    builder_->jump(to_the_end ? innermost.after : innermost.again);
    return true;
}

uint32_t Lowerer::block_for_label(const std::string &name)
{
    auto found = labels_.find(name);
    if (found != labels_.end())
        return found->second;
    // A goto can name a label that has not been written yet, so the block is
    // made when the name is first mentioned rather than when it is reached.
    const uint32_t where = builder_->current();
    const uint32_t made = builder_->block();
    builder_->resume(where);
    labels_.emplace(name, made);
    return made;
}

// A label is a place control can arrive at from somewhere else, which is a
// block. Falling into it from the statement before is a jump like any other.
bool Lowerer::label_statement(const Statement &statement)
{
    const uint32_t named = block_for_label(statement.name);
    if (builder_->block_is_open())
        builder_->jump(named);
    builder_->resume(named);
    return true;
}

bool Lowerer::goto_statement(const Statement &statement)
{
    builder_->jump(block_for_label(statement.name));
    return true;
}

// match (subject) { a, b: ... else: ... }
//
// Each arm is a run of comparisons against the subject, any of which taking it.
// Written out this way rather than as a table because a table is an
// optimisation and this is a lowering: what it has to be is right, and a
// comparison per value is right on every processor.
// for (name in first..last) { body }
//
// The bound name is a local like any other, so it is a frame slot, and the loop
// is the one a while would have made: test at the top, step at the bottom, and
// the step is where `continue` goes so that continuing still advances. A
// continue that skipped the step would be a loop that never ends, which is the
// mistake this shape exists to make impossible.
bool Lowerer::for_in_statement(const Statement &statement)
{
    if (!statement.subject || statement.subject->kind != Expression::Kind::Range) {
        complain(statement.where,
                 "only a for over a range is lowered yet, as in for (step in 0..10)");
        return false;
    }
    const Expression &range = *statement.subject;
    if (!range.left || !range.right) {
        complain(range.where, "a range with no end to it");
        return false;
    }

    // The bound name holds where the count has got to.
    Slot &counting = declare(statement.name, range.left->type, statement.where);
    const ir::Value first = expression(*range.left);
    if (!first.is_valid())
        return false;
    builder_->store(address_of_slot(counting), first, counting.width, ir::Space::Frame);

    const uint32_t before = builder_->current();
    const uint32_t testing = builder_->block();
    const uint32_t body = builder_->block();
    const uint32_t stepping = builder_->block();
    const uint32_t after = builder_->block();

    builder_->resume(before);
    builder_->jump(testing);

    // The end is worked out on every turn rather than once, because this is a
    // lowering and hoisting it is an optimisation.
    builder_->resume(testing);
    const ir::Value last = expression(*range.right);
    if (!last.is_valid())
        return false;
    const ir::Value now =
        builder_->load(address_of_slot(counting), counting.width, ir::Space::Frame, counting.type);
    // `..` stops before its end and `..=` includes it, which is the only
    // difference between the two spellings.
    const ir::Value more = builder_->binary(
        range.inclusive ? ir::Operation::LessOrEqual : ir::Operation::Less, now, last,
        counting.width, type_is_signed(counting.type));
    builder_->branch(more, body, after);

    // Continuing goes to the step, not to the test, so that a continue still
    // advances the count.
    loops_.push_back({stepping, after});
    builder_->resume(body);
    const bool walked = statement.then_branch ? this->statement(*statement.then_branch) : true;
    loops_.pop_back();
    if (!walked)
        return false;
    if (builder_->block_is_open())
        builder_->jump(stepping);

    builder_->resume(stepping);
    const ir::Value held =
        builder_->load(address_of_slot(counting), counting.width, ir::Space::Frame, counting.type);
    const ir::Value one = builder_->constant(1, counting.width, counting.type);
    const ir::Value next = builder_->binary(ir::Operation::Add, held, one, counting.width,
                                            type_is_signed(counting.type));
    builder_->store(address_of_slot(counting), next, counting.width, ir::Space::Frame);
    builder_->jump(testing);

    builder_->resume(after);
    return true;
}

bool Lowerer::match_statement(const Statement &statement)
{
    if (!statement.value) {
        complain(statement.where, "a match with nothing to match on");
        return false;
    }
    const ir::Value subject = expression(*statement.value);
    if (!subject.is_valid())
        return false;

    // Where the first test goes, taken before anything is opened: opening a
    // block makes it the one being written into, so asking afterwards would
    // put the tests inside the last arm's body.
    uint32_t testing = builder_->current();

    const uint32_t after = builder_->block();

    // The arm bodies, and the block each arm's test starts in. `else` has no
    // values and is where control goes when nothing else took it.
    uint32_t otherwise = after;
    std::vector<std::pair<const MatchArm *, uint32_t>> bodies;
    for (const MatchArm &arm : statement.arms) {
        const uint32_t body = builder_->block();
        bodies.emplace_back(&arm, body);
        if (arm.values.empty())
            otherwise = body;
    }

    // The tests, each falling through to the next when it does not take.
    for (size_t i = 0; i < bodies.size(); ++i) {
        const MatchArm &arm = *bodies[i].first;
        if (arm.values.empty())
            continue;  // else is not tested, it is where the tests run out
        for (const ExpressionPtr &value : arm.values) {
            if (!value)
                continue;
            builder_->resume(testing);
            const ir::Value against = expression(*value);
            if (!against.is_valid())
                return false;
            const int width = width_of(subject);
            const ir::Value same =
                builder_->binary(ir::Operation::Equal, subject, against, width, false);
            const uint32_t next = builder_->block();
            builder_->resume(testing);
            builder_->branch(same, bodies[i].second, next);
            testing = next;
        }
    }

    // Nothing took it, so it goes to the else arm or past the whole thing.
    builder_->resume(testing);
    if (builder_->block_is_open())
        builder_->jump(otherwise);

    for (const auto &one : bodies) {
        builder_->resume(one.second);
        if (one.first->body && !this->statement(*one.first->body))
            return false;
        if (builder_->block_is_open())
            builder_->jump(after);
    }

    builder_->resume(after);
    return true;
}

bool Lowerer::return_statement(const Statement &statement)
{
    if (!statement.value) {
        builder_->ret();
        return true;
    }
    const ir::Value held = expression(*statement.value);
    if (!held.is_valid())
        return false;
    builder_->ret(held);
    return true;
}

bool Lowerer::assembly(const Statement &statement)
{
    // Level 0. The bytes are not read here: what was written is kept as it was,
    // and turning it into bytes is the assembler's answer, not the lowering's.
    ir::Instruction instruction;
    instruction.operation = ir::Operation::Raw;
    instruction.where = statement.where;
    for (char character : statement.assembly)
        instruction.bytes.push_back(static_cast<uint8_t>(character));
    builder_->emit(std::move(instruction));
    return true;
}

bool Lowerer::statement(const Statement &statement)
{
    switch (statement.kind) {
    case Statement::Kind::Compound:
        return compound(statement);
    case Statement::Kind::Declaration:
        return declaration(statement);
    case Statement::Kind::Expression:
        if (!statement.value)
            return true;
        expression(*statement.value);
        return !failed_;
    case Statement::Kind::If:
        return if_statement(statement);
    case Statement::Kind::While:
        return while_statement(statement);
    case Statement::Kind::DoWhile:
        return do_while_statement(statement);
    case Statement::Kind::Loop:
        return loop_statement(statement);
    case Statement::Kind::Break:
        return leave_loop(statement, true);
    case Statement::Kind::Continue:
        return leave_loop(statement, false);
    case Statement::Kind::ForIn:
        return for_in_statement(statement);
    case Statement::Kind::Match:
        return match_statement(statement);
    case Statement::Kind::Label:
        return label_statement(statement);
    case Statement::Kind::Goto:
        return goto_statement(statement);
    case Statement::Kind::Return:
        return return_statement(statement);
    case Statement::Kind::Asm:
        return assembly(statement);
    case Statement::Kind::Empty:
        return true;
    default:
        complain(statement.where, "this statement is not lowered yet");
        return false;
    }
}

bool Lowerer::function(const Function &source, ir::Function &out)
{
    ir::Builder builder(source.name);
    builder_ = &builder;
    slots_.clear();
    pinned_.clear();
    labels_.clear();
    loops_.clear();
    next_offset_ = 0;
    failed_ = false;

    builder.set_level(source.level);
    if (source.has_address)
        builder.set_address(source.address);

    // A parameter the caller has already put somewhere keeps where it is. That
    // is the pin the whole representation exists to carry, so it is the first
    // thing written down.
    //
    // A parameter nothing has said about goes where this processor's calling
    // convention puts it, which is read from the same specification everything
    // else here is read from. Both end up as storage; the difference is only
    // who decided.
    std::vector<Storage> by_convention;
    Storage result_storage = source.result_storage;
    {
        bool anything_unsaid = source.result_storage.is_none() && source.result != nullptr;
        std::vector<int> widths;
        for (const Variable &parameter : source.parameters) {
            widths.push_back(width_of_type(parameter.type, target_));
            if (parameter.storage.is_none())
                anything_unsaid = true;
        }
        if (anything_unsaid) {
            std::string error;
            // Reading the convention loads the processor's specification, so it
            // is asked once for the whole function rather than once per
            // parameter. Keeping the loaded specification between functions is
            // worth doing and is not done here.
            if (!target_.calling_convention(widths, width_of_type(source.result, target_),
                                            by_convention, result_storage, error)) {
                complain(source.where,
                         "this processor's specification does not say how a call is made: " +
                             error);
                builder_ = nullptr;
                return false;
            }
        }
    }

    for (size_t i = 0; i < source.parameters.size(); ++i) {
        const Variable &parameter = source.parameters[i];
        Storage where = parameter.storage;
        if (where.is_none() && i < by_convention.size())
            where = by_convention[i];
        const ir::Value value = builder.parameter(parameter.type, where);
        pinned_.emplace(parameter.name, value);
    }

    builder.block();

    if (source.body == nullptr) {
        complain(source.where, source.name + " has no body, so there is nothing to lower");
        builder_ = nullptr;
        return false;
    }

    const bool walked = statement(*source.body);

    // A body that runs off its end still has to leave, the way falling off the
    // end of a function returns. A body that already left does not leave twice,
    // and this is also what fills the block after an if whose halves both
    // returned, so nothing is left empty.
    if (walked && !failed_ && builder.block_is_open())
        builder.ret();

    // The frame is as deep as the lowest slot handed out, which is known only
    // once every one of them has been.
    builder.set_frame_bytes(next_offset_ < 0 ? static_cast<uint64_t>(-next_offset_) : 0);

    std::vector<std::string> problems;
    if (!builder.finish(out, problems)) {
        for (const std::string &problem : problems)
            complain(source.where, problem);
        builder_ = nullptr;
        return false;
    }
    builder_ = nullptr;
    return walked && !failed_;
}

} // namespace

bool lower_to_ir(const Unit &nova, Types &types, const ir::Target &target, ir::Unit &out,
                 std::vector<Diagnostic> &diagnostics)
{
    // Widths below level 2 come from the registers, and the registers are in the
    // compiled specification rather than in a language id. Reading it here means
    // a caller who only named a processor still gets `@w0` four bytes wide
    // instead of whatever the machine word happens to be.
    ir::Target read = target;
    if (!read.spaces_read) {
        std::string error;
        if (!read.read_specification(error)) {
            compiler::Diagnostic diagnostic;
            diagnostic.message = "the specification for " + read.language_id +
                                 " could not be read: " + error;
            diagnostics.push_back(diagnostic);
            return false;
        }
    }

    out.target = read;
    bool all = true;
    for (const Function &source : nova.functions) {
        if (source.body == nullptr)
            continue;  // a declaration says a function exists, not what it does
        ir::Function lowered;
        Lowerer lowerer(nova, types, read, out, diagnostics);
        if (!lowerer.function(source, lowered)) {
            all = false;
            continue;
        }
        out.functions.push_back(std::move(lowered));
    }
    return all;
}

} // namespace nova
} // namespace astral_internal
