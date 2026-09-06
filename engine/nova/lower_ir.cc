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
    Lowerer(Types &types, const ir::Target &target, std::vector<Diagnostic> &diagnostics)
        : types_(types), target_(target), diagnostics_(diagnostics)
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

    // The width a binary operation works in. Its own type says so when it has
    // one; a level-1 expression has none, and then the operands do, which is
    // how `w0 + w0` comes out four bytes wide rather than eight.
    int width_of_operands(const Expression &expression) const;
    int width_of(const ir::Value &value) const;

    Types &types_;
    const ir::Target &target_;
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
    // it was pinned to, and the register is what says how wide it is.
    for (const Expression *side : {expression.left.get(), expression.right.get()}) {
        if (side == nullptr || side->kind != Expression::Kind::Name)
            continue;
        auto parameter = pinned_.find(side->name);
        if (parameter != pinned_.end()) {
            const int width = width_of(parameter->second);
            if (width > 0)
                return width;
        }
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

ir::Value Lowerer::binary(const Expression &expression)
{
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

    complain(expression.where, "this is not something that can be assigned to yet");
    return ir::Value();
}

ir::Value Lowerer::call(const Expression &expression)
{
    if (!expression.callee || expression.callee->kind != Expression::Kind::Name) {
        complain(expression.where, "only a call to a name is lowered yet");
        return ir::Value();
    }

    ir::Instruction instruction;
    instruction.operation = ir::Operation::Call;
    instruction.callee = expression.callee->name;
    instruction.width = width_of_type(expression.type, target_);
    for (const ExpressionPtr &argument : expression.arguments) {
        if (!argument)
            continue;
        const ir::Value lowered = this->expression(*argument);
        if (!lowered.is_valid())
            return ir::Value();
        instruction.arguments.push_back(lowered);
    }
    instruction.result = builder_->value(expression.type);
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
        if (inner && !this->statement(*inner))
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
    next_offset_ = 0;
    failed_ = false;

    builder.set_level(source.level);
    if (source.has_address)
        builder.set_address(source.address);

    // A parameter the caller has already put somewhere keeps where it is. That
    // is the pin the whole representation exists to carry, so it is the first
    // thing written down.
    for (const Variable &parameter : source.parameters) {
        const ir::Value value = builder.parameter(parameter.type, parameter.storage);
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
        Lowerer lowerer(types, read, diagnostics);
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
