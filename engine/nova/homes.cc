#include "homes.hh"

#include <algorithm>
#include <map>
#include <set>

namespace astral_internal {
namespace nova {
namespace homes {

namespace {

// A value with no home, told apart from every other by where it was put and how
// wide it is - which is all p-code says about one.
struct Homeless {
    uint64_t offset = 0;
    int size = 0;

    bool operator<(const Homeless &other) const
    {
        return offset != other.offset ? offset < other.offset : size < other.size;
    }
};

// When a value is first written and last read, counted in operations from the
// start. Two values whose lives do not overlap can share one register, which is
// the whole of what makes this fit at all.
struct Life {
    size_t born = 0;
    size_t died = 0;
};

// The registers a function may use for values of its own.
//
// Not every register on a processor: the stack pointer is not free, and neither
// is anything this function was already told to use, since a value pinned to a
// register is pinned precisely so that nothing else goes there.
std::vector<std::string> free_registers(const ir::Target &target, int size,
                                        const std::set<uint64_t> &taken)
{
    ir::Target::Frame frame;
    std::string unused;
    target.frame(frame, unused);

    // The registers a call puts things in are registers for holding things.
    //
    // A processor has many registers and most are for something in particular -
    // a thread pointer, a status word, a floating-point control setting. Taking
    // whichever came first by address picked those, and an instruction written
    // over one of them is not an instruction anybody meant. The calling
    // convention already names the ones that carry ordinary values, since that
    // is what it is for, so those are asked for and used.
    std::vector<std::string> ordinary;
    {
        std::vector<int> asking(8, size);
        std::vector<Storage> places;
        Storage answer;
        std::string trouble;
        if (target.calling_convention(asking, size, places, answer, trouble)) {
            if (answer.kind == Storage::Kind::Register)
                ordinary.push_back(answer.register_name);
            for (const Storage &where : places) {
                if (where.kind == Storage::Kind::Register)
                    ordinary.push_back(where.register_name);
            }
        }
    }

    std::vector<std::string> free;
    std::set<uint64_t> already;
    for (const std::string &name : ordinary) {
        const ir::Target::RegisterPlace *place = target.register_place(name);
        if (place == nullptr || place->width != size)
            continue;
        if (taken.count(place->offset) != 0)
            continue;
        if (frame.known && name == frame.pointer)
            continue;
        if (!already.insert(place->offset).second)
            continue;
        free.push_back(name);
    }
    return free;
}

} // namespace

bool give(pcode::Sequence &sequence, const ir::Target &target,
          std::vector<std::string> &problems)
{
    const size_t before = problems.size();

    // Where each value with no home lives and dies, and which places are
    // already spoken for.
    std::map<Homeless, Life> lives;
    std::set<uint64_t> taken;
    size_t at = 0;

    auto note = [&](const pcode::Varnode &node) {
        if (node.where == pcode::Where::Register) {
            taken.insert(node.offset);
            return;
        }
        if (node.where != pcode::Where::Unique)
            return;
        Homeless which;
        which.offset = node.offset;
        which.size = node.size;
        auto found = lives.find(which);
        if (found == lives.end()) {
            Life life;
            life.born = at;
            life.died = at;
            lives.emplace(which, life);
        } else {
            found->second.died = at;
        }
    };

    for (const pcode::Block &block : sequence.blocks) {
        for (const pcode::Operation &operation : block.operations) {
            if (operation.writes)
                note(operation.output);
            for (const pcode::Varnode &input : operation.inputs)
                note(input);
            ++at;
        }
    }

    if (lives.empty())
        return true;

    // A value written in one block and read in another is alive across
    // everything between, because control may arrive at the reader by any path.
    // Counting only the operations between them would be wrong wherever a loop
    // sends control backwards, so anything crossing a block is alive from where
    // it starts to where the function ends.
    if (sequence.blocks.size() > 1) {
        std::map<Homeless, size_t> first_block;
        std::map<Homeless, size_t> last_block;
        size_t which_block = 0;
        for (const pcode::Block &block : sequence.blocks) {
            for (const pcode::Operation &operation : block.operations) {
                auto seen = [&](const pcode::Varnode &node) {
                    if (node.where != pcode::Where::Unique)
                        return;
                    Homeless which;
                    which.offset = node.offset;
                    which.size = node.size;
                    first_block.emplace(which, which_block);
                    last_block[which] = which_block;
                };
                if (operation.writes)
                    seen(operation.output);
                for (const pcode::Varnode &input : operation.inputs)
                    seen(input);
            }
            ++which_block;
        }
        for (auto &one : lives) {
            auto first = first_block.find(one.first);
            auto last = last_block.find(one.first);
            if (first != first_block.end() && last != last_block.end() &&
                first->second != last->second)
                one.second.died = at;  // to the end
        }
    }

    // Oldest first, so that what is given out is given in the order things
    // began - which is what makes a register free again as soon as the value in
    // it is finished with.
    std::vector<std::pair<Homeless, Life>> order(lives.begin(), lives.end());
    std::sort(order.begin(), order.end(),
              [](const std::pair<Homeless, Life> &one, const std::pair<Homeless, Life> &two) {
                  return one.second.born < two.second.born;
              });

    // A register per width, since a processor's registers come in sizes and a
    // four-byte value does not go in a two-byte place.
    std::map<int, std::vector<std::string>> free_by_size;
    std::map<Homeless, std::string> given;
    std::vector<std::pair<std::string, size_t>> busy;  // register, until when

    for (const auto &one : order) {
        const Homeless &value = one.first;
        const Life &life = one.second;

        // Anything finished with is free again.
        for (auto held = busy.begin(); held != busy.end();) {
            if (held->second < life.born) {
                free_by_size[value.size].push_back(held->first);
                held = busy.erase(held);
            } else {
                ++held;
            }
        }

        auto &free = free_by_size[value.size];
        if (free.empty()) {
            free = free_registers(target, value.size, taken);
            for (const auto &already : given) {
                auto used = std::find(free.begin(), free.end(), already.second);
                if (used != free.end())
                    free.erase(used);
            }
        }
        if (free.empty()) {
            problems.push_back(
                "this function wants more values at once than this processor has registers of " +
                std::to_string(value.size) + " bytes, and putting some in the frame instead is "
                "not done here");
            return false;
        }

        const std::string where = free.back();
        free.pop_back();
        given.emplace(value, where);
        busy.emplace_back(where, life.died);
    }

    // And now they have somewhere, so say so.
    for (pcode::Block &block : sequence.blocks) {
        for (pcode::Operation &operation : block.operations) {
            auto house = [&](pcode::Varnode &node) {
                if (node.where != pcode::Where::Unique)
                    return;
                Homeless which;
                which.offset = node.offset;
                which.size = node.size;
                auto found = given.find(which);
                if (found == given.end())
                    return;
                const ir::Target::RegisterPlace *place = target.register_place(found->second);
                if (place == nullptr)
                    return;
                node.where = pcode::Where::Register;
                node.offset = place->offset;
                node.size = place->width;
            };
            if (operation.writes)
                house(operation.output);
            for (pcode::Varnode &input : operation.inputs)
                house(input);
        }
    }

    return problems.size() == before;
}

} // namespace homes
} // namespace nova
} // namespace astral_internal
