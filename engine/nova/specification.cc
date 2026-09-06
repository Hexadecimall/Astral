#include "specification.hh"

#include "loadimage.hh"
#include "sleigh.hh"

#include <cstdint>
#include <vector>

#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

namespace astral_internal {
namespace nova {

namespace {

// A load image over nothing.
//
// Reading a specification asks about registers and address spaces, and neither
// question touches the bytes of a program. A processor is described the same
// way whether or not anything has been compiled for it, so this answers with
// zeroes and exists only because an architecture insists on having one.
class NoImage : public ghidra::LoadImage {
public:
    NoImage() : ghidra::LoadImage("<nothing>") {}

    // Bytes to serve, when something is being read back rather than described.
    // Everywhere else is zero, which is what a question about a processor
    // rather than about a program wants.
    std::vector<uint8_t> serving;
    uint64_t serving_at = 0;

    void loadFill(ghidra::uint1 *ptr, ghidra::int4 size, const ghidra::Address &at) override
    {
        for (ghidra::int4 i = 0; i < size; ++i) {
            const uint64_t where = at.getOffset() + static_cast<uint64_t>(i);
            const uint64_t into = where - serving_at;
            ptr[i] = (where >= serving_at && into < serving.size())
                         ? serving[static_cast<size_t>(into)]
                         : 0;
        }
    }
    std::string getArchType(void) const override { return "astral"; }
    void adjustVma(long) override {}
};

// An architecture built to be asked questions rather than to decompile
// anything.
class DescribingArchitecture : public ghidra::SleighArchitecture {
public:
    DescribingArchitecture(const std::string &language_id, std::ostream *errors)
        : ghidra::SleighArchitecture("<nothing>", language_id, errors)
    {
    }

    NoImage *image() { return static_cast<NoImage *>(loader); }

protected:
    void buildLoader(ghidra::DocumentStorage &) override
    {
        collectSpecFiles(*errorstream);
        loader = new NoImage();
    }

    void resolveArchitecture(void) override
    {
        archid = getTarget();
        ghidra::SleighArchitecture::resolveArchitecture();
    }
};

} // namespace

ghidra::SleighArchitecture *specification_for(const std::string &target, std::string &error)
{
    static std::map<std::string, std::unique_ptr<DescribingArchitecture>> kept;
    static std::mutex lock;

    std::lock_guard<std::mutex> holding(lock);
    auto already = kept.find(target);
    if (already != kept.end())
        return already->second.get();

    std::ostringstream complaints;
    try {
        auto made = std::unique_ptr<DescribingArchitecture>(
            new DescribingArchitecture(target, &complaints));
        ghidra::DocumentStorage storage;
        made->init(storage);
        DescribingArchitecture *answer = made.get();
        kept.emplace(target, std::move(made));
        return answer;
    } catch (ghidra::LowlevelError &failure) {
        error = failure.explain;
        return nullptr;
    } catch (ghidra::DecoderError &failure) {
        error = failure.explain;
        return nullptr;
    }
}

std::string reads_as(const std::string &target, const std::vector<uint8_t> &bytes,
                     std::string &error)
{
    static std::mutex lock;
    static uint64_t next = 0x1000;

    ghidra::SleighArchitecture *held = specification_for(target, error);
    if (held == nullptr || bytes.empty())
        return std::string();

    class Written : public ghidra::AssemblyEmit {
    public:
        std::string text;
        void dump(const ghidra::Address &, const std::string &what,
                  const std::string &with) override
        {
            text = with.empty() ? what : what + " " + with;
        }
    };

    std::lock_guard<std::mutex> holding(lock);
    DescribingArchitecture *described = static_cast<DescribingArchitecture *>(held);
    NoImage *image = described->image();
    if (image == nullptr) {
        error = "this processor has nowhere to put the bytes being read";
        return std::string();
    }

    // A fresh address every time, because a reading is kept once made.
    next += 0x100;
    image->serving_at = next;
    image->serving = bytes;

    Written written;
    try {
        held->translate->printAssembly(written,
                                       ghidra::Address(held->getDefaultCodeSpace(), next));
    } catch (ghidra::LowlevelError &failure) {
        error = failure.explain;
        return std::string();
    }
    image->serving.clear();
    return written.text;
}

std::vector<Meaning> means_as(const std::string &target, const std::vector<uint8_t> &bytes,
                              std::string &error)
{
    static std::mutex lock;
    static uint64_t next = 0x40000;

    std::vector<Meaning> meant;
    ghidra::SleighArchitecture *held = specification_for(target, error);
    if (held == nullptr || bytes.empty())
        return meant;

    class Collected : public ghidra::PcodeEmit {
    public:
        std::vector<Meaning> *into = nullptr;
        void dump(const ghidra::Address &, ghidra::OpCode opcode, ghidra::VarnodeData *out,
                  ghidra::VarnodeData *in, ghidra::int4 count) override
        {
            Meaning one;
            one.opcode = opcode;
            if (out != nullptr) {
                one.writes = true;
                one.output = *out;
            }
            for (ghidra::int4 i = 0; i < count; ++i)
                one.inputs.push_back(in[i]);
            into->push_back(std::move(one));
        }
    };

    std::lock_guard<std::mutex> holding(lock);
    DescribingArchitecture *described = static_cast<DescribingArchitecture *>(held);
    NoImage *image = described->image();
    if (image == nullptr) {
        error = "this processor has nowhere to put the bytes being read";
        return meant;
    }

    next += 0x100;
    image->serving_at = next;
    image->serving = bytes;

    Collected collected;
    collected.into = &meant;
    try {
        held->translate->oneInstruction(collected,
                                        ghidra::Address(held->getDefaultCodeSpace(), next));
    } catch (ghidra::LowlevelError &failure) {
        error = failure.explain;
        meant.clear();
    }
    image->serving.clear();
    return meant;
}

} // namespace nova
} // namespace astral_internal
