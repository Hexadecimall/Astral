// Walking the tree, and counting what the frame has to hold.
#include "codegen.hh"

#include <algorithm>
#include <sstream>

namespace astral_internal {
namespace compiler {
namespace {

// Where a named thing lives while the body runs.
struct Slot {
    int64_t offset = 0;      // from the bottom of the frame
    TypePtr type = nullptr;
};

// What `break` and `continue` mean at this point in the body.
struct Loop {
    std::string leave;
    std::string again;
    bool is_switch = false;
};

uint64_t round_up(uint64_t value, uint64_t to)
{
    if (to == 0)
        return value;
    return (value + to - 1) / to * to;
}

// An array or a function used as a value is its address; everything else is
// read from where it sits.
bool decays(TypePtr type)
{
    return type != nullptr &&
           (type->kind == Type::Kind::Array || type->kind == Type::Kind::Function);
}

// What an operation on values of this type works at.
Operation operation_for(TypePtr type)
{
    Operation op;
    if (type == nullptr) {
        return op;
    }
    if (type->kind == Type::Kind::Pointer || decays(type)) {
        op.width = 8;
        op.is_signed = false;
    } else if (type->kind == Type::Kind::Integer) {
        op.width = type->width > 0 ? type->width : 4;
        op.is_signed = type->is_signed;
    } else if (type->kind == Type::Kind::Void) {
        op.width = 8;
        op.is_signed = false;
    } else {
        op.width = 8;
        op.is_signed = false;
    }
    op.result_width = op.width;
    op.result_signed = op.is_signed;
    return op;
}

const char *kind_name(Type::Kind kind)
{
    switch (kind) {
    case Type::Kind::Void: return "void";
    case Type::Kind::Integer: return "an integer";
    case Type::Kind::Floating: return "a floating-point number";
    case Type::Kind::Pointer: return "a pointer";
    case Type::Kind::Array: return "an array";
    case Type::Kind::Function: return "a function";
    default: return "a structure";
    }
}

class Generator {
public:
    Generator(Machine &machine, AsmBuffer &out, const Unit &unit, const Environment &environment,
              const Options &options, std::vector<Result::Datum> &placed,
              std::vector<Diagnostic> &diagnostics)
        : machine_(machine), out_(out), unit_(unit), environment_(environment), options_(options),
          placed_(placed), diagnostics_(diagnostics)
    {
    }

    bool run(const Function &function, uint64_t frame_size, std::string &error);

    // Where the evaluation stack and the named variables start, both counted
    // from the bottom of the frame.
    void set_layout(int64_t eval_base, int64_t locals_top)
    {
        eval_base_ = eval_base;
        locals_top_ = locals_top;
    }
    // What the round just walked turned out to need.
    uint64_t deepest() const { return static_cast<uint64_t>(max_depth_); }
    uint64_t outgoing() const { return outgoing_; }
    uint64_t locals() const { return locals_bytes_; }
    bool makes_call() const { return calls_; }

private:
    // --- complaints -----------------------------------------------------
    void say(const Where &where, const std::string &message)
    {
        if (error_.empty()) {
            error_ = message;
            Diagnostic diagnostic;
            diagnostic.line = where.line;
            diagnostic.column = where.column;
            diagnostic.message = message;
            diagnostics_.push_back(diagnostic);
        }
    }
    bool failed() const { return !error_.empty(); }

    // --- the frame ------------------------------------------------------
    int64_t reserve(TypePtr type);
    void open_scope() { scopes_.emplace_back(); }
    void close_scope() { scopes_.pop_back(); }
    void declare(const std::string &name, TypePtr type, int64_t offset);
    const Slot *look_up(const std::string &name) const;

    // --- the stack of values --------------------------------------------
    void push() { if (++depth_ > max_depth_) max_depth_ = depth_; }
    void pop(int count = 1) { depth_ -= count; }

    // --- expressions ----------------------------------------------------
    void value(const Expression &expression);
    void address(const Expression &expression);
    void call(const Expression &expression, bool wanted);
    void assignment(const Expression &expression);
    void step_in_place(const Expression &expression);
    void logical(const Expression &expression);
    void conditional(const Expression &expression);
    void discard(const Expression &expression);
    uint64_t text_address(const Expression &expression);
    // The size one step of pointer arithmetic moves by, or one when the type is
    // not a pointer.
    uint64_t step_of(TypePtr type) const;

    // --- where control can arrive from more than one direction ----------
    // Naming a point is also telling the architecture that every value has to
    // be where the slower model says it is, and that nothing held over from
    // the instructions just written is still known there.
    void place(const std::string &label)
    {
        machine_.settle(depth_);
        machine_.forget_registers();
        out_.label(label);
    }

    // --- statements -----------------------------------------------------
    void statement(const Statement &node);
    void collect_labels(const Statement &node);
    void collect_cases(const Statement &node, std::vector<std::pair<uint64_t, std::string>> &cases,
                       std::string &fallback, bool outermost);

    Machine &machine_;
    AsmBuffer &out_;
    const Unit &unit_;
    const Environment &environment_;
    const Options &options_;
    std::vector<Result::Datum> &placed_;
    std::vector<Diagnostic> &diagnostics_;

    std::vector<std::map<std::string, Slot>> scopes_;
    std::vector<Loop> loops_;
    std::map<std::string, std::string> goto_labels_;
    std::map<const Statement *, std::string> case_labels_;

    int64_t eval_base_ = 0;
    int64_t locals_top_ = 0;
    uint64_t locals_bytes_ = 0;
    int depth_ = 0;
    int max_depth_ = 0;
    uint64_t outgoing_ = 0;
    bool calls_ = false;
    std::string leave_;
    TypePtr result_type_ = nullptr;
    std::string error_;
};

// ------------------------------------------------------------------ frame

int64_t Generator::reserve(TypePtr type)
{
    uint64_t size = TypeStore::size_of(type);
    if (size == 0)
        size = 8;
    uint64_t align = TypeStore::align_of(type);
    if (align == 0)
        align = 8;
    if (align > 16)
        align = 16;
    locals_bytes_ = round_up(locals_bytes_, align);
    const int64_t at = static_cast<int64_t>(locals_bytes_);
    locals_bytes_ += size;
    return at;
}

void Generator::declare(const std::string &name, TypePtr type, int64_t offset)
{
    Slot slot;
    slot.offset = offset;
    slot.type = type;
    scopes_.back()[name] = slot;
}

const Slot *Generator::look_up(const std::string &name) const
{
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto found = scope->find(name);
        if (found != scope->end())
            return &found->second;
    }
    return nullptr;
}

uint64_t Generator::step_of(TypePtr type) const
{
    if (type == nullptr || type->target == nullptr)
        return 1;
    if (type->kind != Type::Kind::Pointer && type->kind != Type::Kind::Array)
        return 1;
    const uint64_t size = TypeStore::size_of(type->target);
    return size == 0 ? 1 : size;
}

// ------------------------------------------------------------------ literals

uint64_t Generator::text_address(const Expression &expression)
{
    for (const Result::Datum &already : placed_)
        if (already.text == expression.text)
            return already.address;
    if (environment_.address_of_text) {
        const std::optional<uint64_t> found = environment_.address_of_text(expression.text);
        if (found)
            return *found;
    }
    if (options_.place_text) {
        const std::optional<uint64_t> given = options_.place_text(expression.text);
        if (given) {
            Result::Datum datum;
            datum.text = expression.text;
            datum.address = *given;
            placed_.push_back(datum);
            return *given;
        }
    }
    say(expression.where, "the string \"" + expression.text +
                              "\" is not already in the program and there is nowhere to put it");
    return 0;
}

// ------------------------------------------------------------------ addresses

void Generator::address(const Expression &expression)
{
    if (failed())
        return;
    switch (expression.kind) {
    case Expression::Kind::Name: {
        const Slot *slot = look_up(expression.name);
        if (slot != nullptr) {
            machine_.push_frame_address(depth_, locals_top_ + slot->offset);
            push();
            return;
        }
        if (environment_.address_of) {
            const std::optional<uint64_t> found = environment_.address_of(expression.name);
            if (found) {
                machine_.push_absolute(depth_, *found);
                push();
                return;
            }
        }
        say(expression.where, "nothing called " + expression.name +
                                  " is declared here and the program has no such name");
        return;
    }
    case Expression::Kind::Unary:
        if (expression.unary_op == UnaryOp::Dereference) {
            value(*expression.left);
            return;
        }
        break;
    case Expression::Kind::Index: {
        // The base is a value either way: an array decays to its address, a
        // pointer already is one.
        value(*expression.left);
        value(*expression.right);
        const uint64_t step = step_of(expression.left->type);
        if (step != 1) {
            machine_.push_constant(depth_, step);
            push();
            Operation op;
            op.width = 8;
            op.is_signed = false;
            op.result_width = 8;
            op.result_signed = false;
            machine_.binary(depth_, BinaryOp::Multiply, op);
            pop();
        }
        Operation op;
        op.width = 8;
        op.is_signed = false;
        op.result_width = 8;
        op.result_signed = false;
        machine_.binary(depth_, BinaryOp::Add, op);
        pop();
        return;
    }
    case Expression::Kind::Member: {
        if (expression.through_pointer)
            value(*expression.left);
        else
            address(*expression.left);
        if (failed())
            return;
        TypePtr owner = expression.left->type;
        if (expression.through_pointer && owner != nullptr)
            owner = owner->target;
        uint64_t offset = 0;
        bool known = false;
        if (owner != nullptr && owner->kind == Type::Kind::Struct) {
            for (const Type::Member &member : owner->members)
                if (member.name == expression.name) {
                    offset = member.offset;
                    known = true;
                    break;
                }
        }
        if (!known) {
            say(expression.where, "there is no member called " + expression.name + " here");
            return;
        }
        if (offset != 0) {
            machine_.push_constant(depth_, offset);
            push();
            Operation op;
            op.width = 8;
            op.is_signed = false;
            op.result_width = 8;
            op.result_signed = false;
            machine_.binary(depth_, BinaryOp::Add, op);
            pop();
        }
        return;
    }
    case Expression::Kind::StringLiteral:
        machine_.push_absolute(depth_, text_address(expression));
        push();
        return;
    default:
        break;
    }
    say(expression.where, "this is not something an address can be taken of");
}

// ------------------------------------------------------------------ values

void Generator::logical(const Expression &expression)
{
    const bool is_and = expression.binary_op == BinaryOp::LogicalAnd;
    const std::string shortcut = out_.fresh(is_and ? "and.short" : "or.short");
    const std::string done = out_.fresh("logic.done");
    const int base = depth_;

    value(*expression.left);
    if (failed())
        return;
    if (is_and)
        machine_.branch_if_zero(depth_, shortcut);
    else
        machine_.branch_if_nonzero(depth_, shortcut);
    pop();

    value(*expression.right);
    if (failed())
        return;
    // Anything non-zero has to become exactly one, which comparing against zero
    // does for free.
    machine_.push_constant(depth_, 0);
    push();
    Operation op;
    op.width = 8;
    op.is_signed = false;
    op.result_width = 4;
    op.result_signed = true;
    machine_.binary(depth_, BinaryOp::NotEqual, op);
    pop();
    machine_.jump(depth_, done);

    depth_ = base;
    place(shortcut);
    machine_.push_constant(depth_, is_and ? 0 : 1);
    push();
    place(done);
}

void Generator::conditional(const Expression &expression)
{
    const std::string otherwise = out_.fresh("cond.else");
    const std::string done = out_.fresh("cond.done");
    const int base = depth_;

    value(*expression.left);
    if (failed())
        return;
    machine_.branch_if_zero(depth_, otherwise);
    pop();

    value(*expression.right);
    if (failed())
        return;
    machine_.jump(depth_, done);

    depth_ = base;
    place(otherwise);
    value(*expression.third);
    if (failed())
        return;
    place(done);
    depth_ = base + 1;
    if (depth_ > max_depth_)
        max_depth_ = depth_;
}

void Generator::assignment(const Expression &expression)
{
    TypePtr target = expression.left->type;
    if (target != nullptr && target->kind == Type::Kind::Struct) {
        say(expression.where, "whole structures cannot be assigned yet; assign the members");
        return;
    }
    address(*expression.left);
    if (failed())
        return;
    if (expression.assign_op == BinaryOp::Comma) {
        value(*expression.right);
        if (failed())
            return;
    } else {
        // A compound assignment reads what is there, works on it and puts it
        // back, so the address is computed once and used twice.
        machine_.duplicate(depth_);
        push();
        const Operation load = operation_for(target);
        machine_.load_indirect(depth_, load);
        value(*expression.right);
        if (failed())
            return;
        const bool stepping = target != nullptr && target->kind == Type::Kind::Pointer &&
                              (expression.assign_op == BinaryOp::Add ||
                               expression.assign_op == BinaryOp::Subtract);
        if (stepping) {
            const uint64_t step = step_of(target);
            if (step != 1) {
                machine_.push_constant(depth_, step);
                push();
                Operation scale;
                scale.width = 8;
                scale.is_signed = false;
                scale.result_width = 8;
                scale.result_signed = false;
                machine_.binary(depth_, BinaryOp::Multiply, scale);
                pop();
            }
        }
        Operation op = operation_for(target);
        if (expression.assign_op == BinaryOp::ShiftLeft ||
            expression.assign_op == BinaryOp::ShiftRight) {
            // The count is its own type; the value's decides the shift.
            op = operation_for(target);
        }
        machine_.binary(depth_, expression.assign_op, op);
        pop();
    }
    const Operation store = operation_for(target);
    machine_.store_indirect(depth_, store);
    pop();
}

void Generator::step_in_place(const Expression &expression)
{
    const UnaryOp what = expression.unary_op;
    const bool after = what == UnaryOp::PostIncrement || what == UnaryOp::PostDecrement;
    const bool up = what == UnaryOp::PreIncrement || what == UnaryOp::PostIncrement;
    const Expression &target = *expression.left;
    const TypePtr type = target.type;
    const uint64_t step = type != nullptr && type->kind == Type::Kind::Pointer ? step_of(type) : 1;

    const int base = depth_;
    address(target);
    if (failed())
        return;
    machine_.duplicate(depth_);
    push();
    const Operation op = operation_for(type);
    machine_.load_indirect(depth_, op);
    if (after) {
        // The answer is the value before the step, so it is kept aside first.
        machine_.duplicate(depth_);
        push();
    }
    machine_.push_constant(depth_, step);
    push();
    machine_.binary(depth_, up ? BinaryOp::Add : BinaryOp::Subtract, op);
    pop();
    if (after) {
        // base: the address, base + 1: the old value, base + 2: the new one.
        machine_.swap_slots(base, base + 1);
        machine_.store_indirect(depth_, op);
        pop();
        // What is left is the old value in `base` and the stored one above it.
        pop();
    } else {
        machine_.store_indirect(depth_, op);
        pop();
    }
    depth_ = base + 1;
}

void Generator::call(const Expression &expression, bool wanted)
{
    CallSite site;
    const Expression &callee = *expression.callee;
    TypePtr signature = callee.type;
    if (signature != nullptr && signature->kind == Type::Kind::Pointer)
        signature = signature->target;

    const int base = depth_;
    bool direct = false;
    if (callee.kind == Expression::Kind::Name) {
        const Slot *local = look_up(callee.name);
        if (local == nullptr) {
            if (environment_.address_of) {
                const std::optional<uint64_t> found = environment_.address_of(callee.name);
                if (found) {
                    site.address = *found;
                    direct = true;
                }
            }
            if (!direct) {
                say(expression.where, "the program has no function called " + callee.name +
                                          ", so there is nothing to call");
                return;
            }
        }
        site.name = callee.name;
    }
    if (!direct) {
        // Called through a value: it goes below the arguments so the arguments
        // stay where the architecture expects to find them.
        value(callee);
        if (failed())
            return;
        site.through_pointer = true;
        if (site.name.empty())
            site.name = "a function pointer";
    }

    for (const ExpressionPtr &argument : expression.arguments) {
        value(*argument);
        if (failed())
            return;
        site.types.push_back(argument->type);
    }

    site.variadic = signature != nullptr && signature->variadic;
    site.fixed_count = signature != nullptr ? signature->parameters.size() : site.types.size();
    if (site.fixed_count > site.types.size())
        site.fixed_count = site.types.size();
    if (!site.variadic)
        site.fixed_count = site.types.size();
    site.result = signature != nullptr ? signature->result : expression.type;

    calls_ = true;
    outgoing_ = std::max(outgoing_, machine_.outgoing_bytes(site));
    machine_.call(depth_, site);

    depth_ = base;
    const bool answers = site.result != nullptr && site.result->kind != Type::Kind::Void;
    if (answers) {
        push();
    } else if (wanted) {
        // Nothing came back but something is wanted; a zero keeps the stack
        // honest rather than reading a register that means nothing.
        machine_.push_constant(depth_, 0);
        push();
    }
    if (answers && !wanted)
        pop();
}

void Generator::value(const Expression &expression)
{
    if (failed())
        return;
    switch (expression.kind) {
    case Expression::Kind::IntegerLiteral:
        machine_.push_constant(depth_, expression.integer_value);
        push();
        return;

    case Expression::Kind::FloatLiteral:
        say(expression.where, "floating-point arithmetic is not written yet");
        return;

    case Expression::Kind::InitialiserList:
        say(expression.where, "a braced list of values is not written yet");
        return;

    case Expression::Kind::StringLiteral:
        machine_.push_absolute(depth_, text_address(expression));
        push();
        return;

    case Expression::Kind::SizeOf:
        machine_.push_constant(depth_, TypeStore::size_of(expression.named_type));
        push();
        return;

    case Expression::Kind::Name: {
        if (decays(expression.type)) {
            address(expression);
            return;
        }
        if (expression.type != nullptr && expression.type->kind == Type::Kind::Struct) {
            say(expression.where, "a whole structure cannot be used as a value yet");
            return;
        }
        address(expression);
        if (failed())
            return;
        machine_.load_indirect(depth_, operation_for(expression.type));
        return;
    }

    case Expression::Kind::Index:
    case Expression::Kind::Member: {
        if (decays(expression.type)) {
            address(expression);
            return;
        }
        address(expression);
        if (failed())
            return;
        machine_.load_indirect(depth_, operation_for(expression.type));
        return;
    }

    case Expression::Kind::Call:
        call(expression, true);
        return;

    case Expression::Kind::Assign:
        assignment(expression);
        return;

    case Expression::Kind::Cast: {
        if (expression.named_type != nullptr &&
            expression.named_type->kind == Type::Kind::Floating) {
            say(expression.where, "floating-point conversion is not written yet");
            return;
        }
        value(*expression.left);
        if (failed())
            return;
        machine_.convert(depth_, operation_for(expression.left->type),
                         operation_for(expression.named_type));
        return;
    }

    case Expression::Kind::Conditional:
        conditional(expression);
        return;

    case Expression::Kind::Unary: {
        switch (expression.unary_op) {
        case UnaryOp::AddressOf:
            address(*expression.left);
            return;
        case UnaryOp::Dereference: {
            if (decays(expression.type)) {
                value(*expression.left);
                return;
            }
            value(*expression.left);
            if (failed())
                return;
            machine_.load_indirect(depth_, operation_for(expression.type));
            return;
        }
        case UnaryOp::PreIncrement:
        case UnaryOp::PreDecrement:
        case UnaryOp::PostIncrement:
        case UnaryOp::PostDecrement:
            step_in_place(expression);
            return;
        case UnaryOp::Plus:
            value(*expression.left);
            return;
        case UnaryOp::Not: {
            value(*expression.left);
            if (failed())
                return;
            Operation op = operation_for(expression.left->type);
            op.result_width = 4;
            op.result_signed = true;
            machine_.unary(depth_, UnaryOp::Not, op);
            return;
        }
        default: {
            value(*expression.left);
            if (failed())
                return;
            machine_.unary(depth_, expression.unary_op, operation_for(expression.type));
            return;
        }
        }
    }

    case Expression::Kind::Binary: {
        const BinaryOp what = expression.binary_op;
        if (what == BinaryOp::LogicalAnd || what == BinaryOp::LogicalOr) {
            logical(expression);
            return;
        }
        if (what == BinaryOp::Comma) {
            discard(*expression.left);
            if (failed())
                return;
            value(*expression.right);
            return;
        }

        const TypePtr left_type = expression.left->type;
        const TypePtr right_type = expression.right->type;
        const bool left_is_pointer =
            left_type != nullptr &&
            (left_type->kind == Type::Kind::Pointer || left_type->kind == Type::Kind::Array);
        const bool right_is_pointer =
            right_type != nullptr &&
            (right_type->kind == Type::Kind::Pointer || right_type->kind == Type::Kind::Array);

        // Pointer arithmetic counts in elements, so one side gets scaled.
        if ((what == BinaryOp::Add || what == BinaryOp::Subtract) &&
            left_is_pointer != right_is_pointer) {
            const Expression &pointer = left_is_pointer ? *expression.left : *expression.right;
            const Expression &count = left_is_pointer ? *expression.right : *expression.left;
            if (!left_is_pointer && what == BinaryOp::Subtract) {
                say(expression.where, "a pointer cannot be subtracted from a number");
                return;
            }
            const uint64_t step = step_of(pointer.type);
            value(pointer);
            value(count);
            if (failed())
                return;
            Operation wide;
            wide.width = 8;
            wide.is_signed = true;
            wide.result_width = 8;
            wide.result_signed = false;
            if (step != 1) {
                machine_.push_constant(depth_, step);
                push();
                machine_.binary(depth_, BinaryOp::Multiply, wide);
                pop();
            }
            machine_.binary(depth_, what, wide);
            pop();
            return;
        }
        // The distance between two pointers is in elements too.
        if (what == BinaryOp::Subtract && left_is_pointer && right_is_pointer) {
            const uint64_t step = step_of(left_type);
            value(*expression.left);
            value(*expression.right);
            if (failed())
                return;
            Operation wide;
            wide.width = 8;
            wide.is_signed = true;
            wide.result_width = 8;
            wide.result_signed = true;
            machine_.binary(depth_, BinaryOp::Subtract, wide);
            pop();
            if (step != 1) {
                machine_.push_constant(depth_, step);
                push();
                machine_.binary(depth_, BinaryOp::Divide, wide);
                pop();
            }
            return;
        }

        value(*expression.left);
        value(*expression.right);
        if (failed())
            return;

        Operation op;
        const bool comparing = what == BinaryOp::Less || what == BinaryOp::LessEqual ||
                               what == BinaryOp::Greater || what == BinaryOp::GreaterEqual ||
                               what == BinaryOp::Equal || what == BinaryOp::NotEqual;
        if (what == BinaryOp::ShiftLeft || what == BinaryOp::ShiftRight) {
            // Only the value being shifted decides the width and the sign.
            op = operation_for(left_type);
        } else if (comparing) {
            // Both sides are already widened, so comparing at the wider of the
            // two is the same answer with fewer special cases.
            const Operation left = operation_for(left_type);
            const Operation right = operation_for(right_type);
            op.width = std::max(left.width, right.width);
            op.is_signed = left.is_signed && right.is_signed;
            if (left_is_pointer || right_is_pointer) {
                op.width = 8;
                op.is_signed = false;
            }
            op.result_width = 4;
            op.result_signed = true;
        } else {
            op = operation_for(expression.type);
        }
        machine_.binary(depth_, what, op);
        pop();
        return;
    }
    }
    say(expression.where, "this expression is not something the compiler writes yet");
}

void Generator::discard(const Expression &expression)
{
    if (expression.kind == Expression::Kind::Call) {
        call(expression, false);
        return;
    }
    value(expression);
    if (failed())
        return;
    pop();
}

// ------------------------------------------------------------------ statements

void Generator::collect_labels(const Statement &node)
{
    if (node.kind == Statement::Kind::Label && !node.name.empty())
        if (goto_labels_.find(node.name) == goto_labels_.end())
            goto_labels_[node.name] = out_.fresh("user." + node.name);
    for (const StatementPtr &child : node.body)
        if (child)
            collect_labels(*child);
    if (node.initialiser)
        collect_labels(*node.initialiser);
    if (node.then_branch)
        collect_labels(*node.then_branch);
    if (node.else_branch)
        collect_labels(*node.else_branch);
}

// Finds every `case` belonging to one switch. A nested switch owns its own, so
// the walk stops when it meets one.
void Generator::collect_cases(const Statement &node,
                              std::vector<std::pair<uint64_t, std::string>> &cases,
                              std::string &fallback, bool outermost)
{
    if (!outermost && node.kind == Statement::Kind::Switch)
        return;
    if (node.kind == Statement::Kind::Case) {
        const std::string label = out_.fresh("case");
        case_labels_[&node] = label;
        cases.emplace_back(node.case_value, label);
    } else if (node.kind == Statement::Kind::Default) {
        const std::string label = out_.fresh("default");
        case_labels_[&node] = label;
        fallback = label;
    }
    for (const StatementPtr &child : node.body)
        if (child)
            collect_cases(*child, cases, fallback, false);
    if (node.then_branch)
        collect_cases(*node.then_branch, cases, fallback, false);
    if (node.else_branch)
        collect_cases(*node.else_branch, cases, fallback, false);
}

void Generator::statement(const Statement &node)
{
    if (failed())
        return;
    switch (node.kind) {
    case Statement::Kind::Empty:
        return;

    case Statement::Kind::Compound: {
        open_scope();
        const uint64_t mark = locals_bytes_;
        for (const StatementPtr &child : node.body)
            if (child)
                statement(*child);
        close_scope();
        // Slots are not reused between blocks; keeping the high-water mark is
        // what makes an address handed out inside a block stay valid.
        locals_bytes_ = std::max(locals_bytes_, mark);
        return;
    }

    case Statement::Kind::Declaration: {
        for (const Variable &variable : node.variables) {
            const int64_t at = reserve(variable.type);
            declare(variable.name, variable.type, at);
            if (!variable.initialiser)
                continue;
            if (variable.type != nullptr && variable.type->kind == Type::Kind::Array) {
                say(variable.where, "an array cannot be given a value where it is declared yet");
                return;
            }
            machine_.push_frame_address(depth_, locals_top_ + at);
            push();
            value(*variable.initialiser);
            if (failed())
                return;
            machine_.store_indirect(depth_, operation_for(variable.type));
            pop();
            pop();
        }
        return;
    }

    case Statement::Kind::Expression:
        if (node.value)
            discard(*node.value);
        return;

    case Statement::Kind::If: {
        const std::string otherwise = out_.fresh("if.else");
        const std::string done = out_.fresh("if.done");
        if (!node.value) {
            say(node.where, "an if with no condition cannot be written");
            return;
        }
        value(*node.value);
        if (failed())
            return;
        machine_.branch_if_zero(depth_, node.else_branch ? otherwise : done);
        pop();
        if (node.then_branch)
            statement(*node.then_branch);
        if (node.else_branch) {
            machine_.jump(depth_, done);
            place(otherwise);
            statement(*node.else_branch);
        }
        place(done);
        return;
    }

    case Statement::Kind::While: {
        const std::string again = out_.fresh("while.top");
        const std::string done = out_.fresh("while.done");
        place(again);
        if (node.value) {
            value(*node.value);
            if (failed())
                return;
            machine_.branch_if_zero(depth_, done);
            pop();
        }
        loops_.push_back({done, again, false});
        if (node.then_branch)
            statement(*node.then_branch);
        loops_.pop_back();
        machine_.jump(depth_, again);
        place(done);
        return;
    }

    case Statement::Kind::DoWhile: {
        const std::string again = out_.fresh("do.top");
        const std::string test = out_.fresh("do.test");
        const std::string done = out_.fresh("do.done");
        place(again);
        loops_.push_back({done, test, false});
        if (node.then_branch)
            statement(*node.then_branch);
        loops_.pop_back();
        place(test);
        if (node.value) {
            value(*node.value);
            if (failed())
                return;
            machine_.branch_if_nonzero(depth_, again);
            pop();
        } else {
            machine_.jump(depth_, again);
        }
        place(done);
        return;
    }

    case Statement::Kind::For: {
        open_scope();
        const std::string again = out_.fresh("for.top");
        const std::string next = out_.fresh("for.next");
        const std::string done = out_.fresh("for.done");
        if (node.initialiser)
            statement(*node.initialiser);
        place(again);
        if (node.condition) {
            value(*node.condition);
            if (failed()) {
                close_scope();
                return;
            }
            machine_.branch_if_zero(depth_, done);
            pop();
        }
        loops_.push_back({done, next, false});
        if (node.then_branch)
            statement(*node.then_branch);
        loops_.pop_back();
        place(next);
        if (node.step)
            discard(*node.step);
        machine_.jump(depth_, again);
        place(done);
        close_scope();
        return;
    }

    case Statement::Kind::Switch: {
        if (!node.value) {
            say(node.where, "a switch with nothing to choose on cannot be written");
            return;
        }
        std::vector<std::pair<uint64_t, std::string>> cases;
        std::string fallback;
        if (node.then_branch)
            collect_cases(*node.then_branch, cases, fallback, true);
        const std::string done = out_.fresh("switch.done");
        if (fallback.empty())
            fallback = done;

        value(*node.value);
        if (failed())
            return;
        // A chain of comparisons. It is longer than a jump table but needs no
        // table anywhere in the image, which a patch has no room for.
        for (const std::pair<uint64_t, std::string> &one : cases)
            machine_.branch_if_equal(depth_, one.first, one.second);
        pop();
        machine_.jump(depth_, fallback);

        loops_.push_back({done, std::string(), true});
        if (node.then_branch)
            statement(*node.then_branch);
        loops_.pop_back();
        place(done);
        return;
    }

    case Statement::Kind::Case:
    case Statement::Kind::Default: {
        const auto found = case_labels_.find(&node);
        if (found != case_labels_.end())
            place(found->second);
        if (node.then_branch)
            statement(*node.then_branch);
        return;
    }

    case Statement::Kind::Label: {
        const auto found = goto_labels_.find(node.name);
        if (found != goto_labels_.end())
            place(found->second);
        if (node.then_branch)
            statement(*node.then_branch);
        return;
    }

    case Statement::Kind::Goto: {
        const auto found = goto_labels_.find(node.name);
        if (found == goto_labels_.end()) {
            say(node.where, "there is no label called " + node.name + " in this function");
            return;
        }
        machine_.jump(depth_, found->second);
        return;
    }

    case Statement::Kind::Break: {
        for (auto loop = loops_.rbegin(); loop != loops_.rend(); ++loop) {
            machine_.jump(depth_, loop->leave);
            return;
        }
        say(node.where, "there is no loop or switch here for break to leave");
        return;
    }

    case Statement::Kind::Continue: {
        for (auto loop = loops_.rbegin(); loop != loops_.rend(); ++loop) {
            if (loop->is_switch)
                continue;
            machine_.jump(depth_, loop->again);
            return;
        }
        say(node.where, "there is no loop here for continue to go round");
        return;
    }

    case Statement::Kind::Return: {
        if (node.value) {
            value(*node.value);
            if (failed())
                return;
            machine_.return_value(depth_);
            pop();
        } else {
            machine_.return_nothing();
        }
        return;
    }
    }
}

// ------------------------------------------------------------------ the whole

bool Generator::run(const Function &function, uint64_t frame_size, std::string &error)
{
    result_type_ = function.type != nullptr ? function.type->result : nullptr;
    (void)unit_;
    (void)result_type_;
    leave_ = out_.fresh("leave");
    machine_.set_frame(frame_size, eval_base_, leave_);
    machine_.attach(&out_);

    open_scope();
    // Parameters come first in the frame, so a debugger reading the frame finds
    // them where a compiled function would have put them.
    std::vector<Parameter> parameters;
    for (const Variable &declared : function.parameters) {
        const int64_t at = reserve(declared.type);
        declare(declared.name, declared.type, at);
        Parameter parameter;
        parameter.type = declared.type;
        parameter.frame_offset = locals_top_ + at;
        parameters.push_back(parameter);
    }

    if (function.body)
        collect_labels(*function.body);

    machine_.prologue();
    machine_.spill_parameters(parameters);
    if (function.body)
        statement(*function.body);
    close_scope();

    place(leave_);
    machine_.epilogue();

    if (!error_.empty()) {
        error = error_;
        return false;
    }
    if (!machine_.error().empty()) {
        error = machine_.error();
        return false;
    }
    return true;
}

} // namespace

bool generate_function(Machine &machine, AsmBuffer &out, assembler::Target target, uint64_t address,
                       const Unit &unit, const Function &function, const Environment &environment,
                       const Options &options, std::vector<Result::Datum> &placed,
                       std::vector<Diagnostic> &diagnostics, std::string &error)
{
    if (!function.body) {
        error = function.name + " has no body, so there is nothing to compile";
        return false;
    }

    // Every slot is an offset from the stack pointer, so the frame has to be
    // sized before the body can be written; and what the frame has to hold is
    // only known once the body has been walked. The first round measures and
    // the second writes. Neither measurement depends on the size, so two rounds
    // is always enough, and the third is only there to notice if that ever
    // stops being true.
    uint64_t depth_needed = 0;
    uint64_t outgoing_needed = 0;
    uint64_t locals_needed = 0;
    bool leaf = true;
    for (int round = 0; round < 4; ++round) {
        machine.set_leaf(leaf);
        machine.set_origin(address);
        const uint64_t eval_base = outgoing_needed;
        const uint64_t locals_top = eval_base + depth_needed * machine.slot_size();
        const uint64_t frame_size =
            round_up(locals_top + locals_needed + machine.reserved_bytes(),
                     machine.frame_alignment());

        AsmBuffer attempt(target, address);
        std::vector<Result::Datum> attempt_placed = placed;
        std::vector<Diagnostic> attempt_diagnostics;
        machine.forget_error();
        Generator generator(machine, attempt, unit, environment, options, attempt_placed,
                            attempt_diagnostics);
        generator.set_layout(static_cast<int64_t>(eval_base), static_cast<int64_t>(locals_top));
        std::string why;
        const bool worked = generator.run(function, frame_size, why);

        const uint64_t measured_depth = generator.deepest();
        const uint64_t measured_outgoing = generator.outgoing();
        const uint64_t measured_locals = generator.locals();
        const bool settled = worked && measured_depth <= depth_needed &&
                             measured_outgoing <= outgoing_needed &&
                             measured_locals <= locals_needed &&
                             leaf == !generator.makes_call();

        if (!worked) {
            // A body that will not be written will not be written differently
            // with a bigger frame, so the first complaint is the answer.
            error = why;
            diagnostics.insert(diagnostics.end(), attempt_diagnostics.begin(),
                               attempt_diagnostics.end());
            return false;
        }
        if (settled) {
            out = attempt;
            placed = attempt_placed;
            // What the frame has to hold was counted from the tree, which does
            // not know that most values never leave a register. If none of the
            // room set aside was used, the same body written without it is the
            // same body, and a function that needs no frame at all keeps
            // neither a prologue nor an epilogue.
            const bool frame_untouched =
                !attempt.uses_frame_between(static_cast<int64_t>(eval_base),
                                            static_cast<int64_t>(locals_top + locals_needed));
            if (frame_untouched && (depth_needed > 0 || locals_needed > 0)) {
                const uint64_t bare_frame =
                    round_up(outgoing_needed + machine.reserved_bytes(),
                             machine.frame_alignment());
                AsmBuffer smaller(target, address);
                std::vector<Result::Datum> smaller_placed = placed;
                std::vector<Diagnostic> smaller_diagnostics;
                machine.forget_error();
                Generator lean(machine, smaller, unit, environment, options, smaller_placed,
                               smaller_diagnostics);
                lean.set_layout(static_cast<int64_t>(outgoing_needed),
                                static_cast<int64_t>(outgoing_needed));
                std::string ignored;
                const bool lean_worked = lean.run(function, bare_frame, ignored);
                if (lean_worked && machine.error().empty() &&
                    !smaller.uses_frame_between(static_cast<int64_t>(outgoing_needed),
                                                static_cast<int64_t>(bare_frame)) &&
                    lean.outgoing() <= outgoing_needed && lean.deepest() <= depth_needed &&
                    lean.locals() <= locals_needed && leaf == !lean.makes_call()) {
                    out = smaller;
                    placed = smaller_placed;
                    attempt_diagnostics = smaller_diagnostics;
                }
                machine.forget_error();
            }
            diagnostics.insert(diagnostics.end(), attempt_diagnostics.begin(),
                               attempt_diagnostics.end());
            return true;
        }
        leaf = leaf && !generator.makes_call();
        depth_needed = std::max(depth_needed, measured_depth);
        outgoing_needed = std::max(outgoing_needed, measured_outgoing);
        locals_needed = std::max(locals_needed, measured_locals);
    }
    error = "the frame this function needs will not settle";
    return false;
}

} // namespace compiler
} // namespace astral_internal
