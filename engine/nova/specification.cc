#include "specification.hh"

#include "loadimage.hh"

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
    void loadFill(ghidra::uint1 *ptr, ghidra::int4 size, const ghidra::Address &) override
    {
        for (ghidra::int4 i = 0; i < size; ++i)
            ptr[i] = 0;
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

} // namespace nova
} // namespace astral_internal
