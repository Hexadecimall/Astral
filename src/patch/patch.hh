#ifndef ASTRAL_PATCH_HH
#define ASTRAL_PATCH_HH

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {

// Which stage of the patch engine produced a change. Recorded so a patch set
// reads back honestly and the UI can say how each edit was made.
enum class PatchTier {
    ByteRewrite, // a recognised structural edit mapped to exact bytes
    Assembled,   // instruction text turned into bytes by the assembler
    Compiled,    // compiled C, reached through a trampoline
    Manual,      // bytes supplied directly
};

const char *tier_name(PatchTier tier);
bool tier_from_name(const std::string &name, PatchTier &out);

// One contiguous byte edit against a file. `original` is kept so a set applies
// only to the bytes it was cut from, and so an undo restores exactly what was
// there.
struct Patch {
    uint64_t address = 0;             // virtual address of the change
    uint64_t file_offset = 0;         // byte position in the file
    std::vector<uint8_t> original;    // bytes before
    std::vector<uint8_t> replacement; // bytes after
    PatchTier tier = PatchTier::Manual;
    std::string note;                 // why, in a few words
};

// An ordered, undoable set of edits against one binary. Serialises to a readable
// patches.astral and can be replayed onto another copy of the same file.
class PatchSet {
public:
    void add(Patch patch) { patches_.push_back(std::move(patch)); }
    bool empty() const { return patches_.empty(); }
    size_t size() const { return patches_.size(); }
    const std::vector<Patch> &patches() const { return patches_; }
    void undo_last()
    {
        if (!patches_.empty())
            patches_.pop_back();
    }
    void clear() { patches_.clear(); }

    // Overlays every patch onto a copy of `file_bytes`. Fails, leaving the
    // buffer untouched, when a patch runs off the end or its recorded original
    // no longer matches the file underneath it.
    bool apply_to(std::vector<uint8_t> &file_bytes, std::string &error) const;

    std::string serialize() const;
    static bool parse(const std::string &text, PatchSet &out, std::string &error);

private:
    std::vector<Patch> patches_;
};

// Reads / writes whole files. Return false and set `error` on failure.
bool write_file(const std::string &path, const std::vector<uint8_t> &bytes, std::string &error);
bool make_file_executable(const std::string &path);

} // namespace astral_internal

#endif
