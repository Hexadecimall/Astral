// Binds a loaded BinaryImage to Ghidra's decompiler core: a LoadImage over the
// image, a SleighArchitecture that uses it, and the analysis entry points.
#include "session.hh"
#include "macho_sign.hh"

#include <sys/stat.h>

#include "creal.hh"
#include "langmap.hh"
#include "knowledge.hh"
#include "libc_protos.hh"
#include "naming.hh"
#include "paths.hh"

#include "architecture.hh"
#include "block.hh"
#include "funcdata.hh"
#include "libdecomp.hh"
#include "printc.hh"
#include "sleigh_arch.hh"
#include "slgh_compile.hh"
#include "types.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

namespace astral_internal {
namespace {

bool g_initialized = false;

// A LoadImage backed by an in-memory BinaryImage.
class ImageLoadImage : public ghidra::LoadImage {
public:
    ImageLoadImage(const BinaryImage &image, const std::string &archtype)
        : ghidra::LoadImage(image.path.empty() ? std::string("<memory>") : image.path),
          image_(image), archtype_(archtype)
    {
    }

    void loadFill(ghidra::uint1 *ptr, ghidra::int4 size, const ghidra::Address &addr) override
    {
        uint64_t off = addr.getOffset() + vma_adjust_;
        size_t covered = image_.read(off, ptr, static_cast<size_t>(size));
        if (covered == 0)
            throw ghidra::DataUnavailError("Address not in image: 0x" +
                                           to_hex(addr.getOffset()));
    }

    std::string getArchType(void) const override { return archtype_; }

    void adjustVma(long adjust) override { vma_adjust_ -= adjust; }

    void openSymbols(void) const override { symbol_cursor_ = 0; }

    bool getNextSymbol(ghidra::LoadImageFunc &record) const override
    {
        while (symbol_cursor_ < image_.symbols.size()) {
            const Symbol &sym = image_.symbols[symbol_cursor_++];
            if (!sym.is_function)
                continue;
            record.address = ghidra::Address(space_, sym.address);
            record.name = sym.name;
            return true;
        }
        return false;
    }

    void openSectionInfo(void) const override { section_cursor_ = 0; }

    bool getNextSection(ghidra::LoadImageSection &sec) const override
    {
        if (section_cursor_ >= image_.segments.size())
            return false;
        const Segment &seg = image_.segments[section_cursor_++];
        sec.address = ghidra::Address(space_, seg.address);
        sec.size = seg.size;
        sec.flags = 0;
        if (seg.executable)
            sec.flags |= ghidra::LoadImageSection::code;
        else
            sec.flags |= ghidra::LoadImageSection::data;
        if (!seg.writable)
            sec.flags |= ghidra::LoadImageSection::readonly;
        return true;
    }

    void getReadonly(ghidra::RangeList &list) const override
    {
        for (const Segment &seg : image_.segments) {
            if (seg.writable || seg.size == 0)
                continue;
            list.insertRange(space_, seg.address, seg.address + seg.size - 1);
        }
    }

    void attachToSpace(ghidra::AddrSpace *space) { space_ = space; }

private:
    static std::string to_hex(uint64_t v)
    {
        std::ostringstream s;
        s << std::hex << v;
        return s.str();
    }

    const BinaryImage &image_;
    std::string archtype_;
    ghidra::AddrSpace *space_ = nullptr;
    uint64_t vma_adjust_ = 0;
    mutable size_t symbol_cursor_ = 0;
    mutable size_t section_cursor_ = 0;
};

// A SleighArchitecture whose load image is the one supplied by this library.
class StandaloneArchitecture : public ghidra::SleighArchitecture {
public:
    StandaloneArchitecture(const BinaryImage &image, const std::string &archid, std::ostream *errs)
        : ghidra::SleighArchitecture(image.path.empty() ? std::string("<memory>") : image.path,
                                     archid, errs),
          image_(image)
    {
    }

protected:
    void buildLoader(ghidra::DocumentStorage &store) override
    {
        collectSpecFiles(*errorstream);
        loader = new ImageLoadImage(image_, getTarget());
    }

    void resolveArchitecture(void) override
    {
        archid = getTarget();
        ghidra::SleighArchitecture::resolveArchitecture();
    }

    void postSpecFile(void) override
    {
        ghidra::Architecture::postSpecFile();
        static_cast<ImageLoadImage *>(loader)->attachToSpace(getDefaultCodeSpace());
    }

private:
    const BinaryImage &image_;
};

class StringAssemblyEmit : public ghidra::AssemblyEmit {
public:
    explicit StringAssemblyEmit(std::ostringstream &out) : out_(out) {}
    void dump(const ghidra::Address &addr, const std::string &mnem, const std::string &body) override
    {
        addr.printRaw(out_);
        out_ << ": " << mnem << ' ' << body << '\n';
    }

private:
    std::ostringstream &out_;
};

class StringPcodeEmit : public ghidra::PcodeEmit {
public:
    StringPcodeEmit(std::ostringstream &out, const ghidra::Translate *trans)
        : out_(out), trans_(trans)
    {
    }

    void dump(const ghidra::Address &addr, ghidra::OpCode opc, ghidra::VarnodeData *outvar,
              ghidra::VarnodeData *vars, ghidra::int4 isize) override
    {
        addr.printRaw(out_);
        out_ << ": ";
        if (outvar != nullptr) {
            print_varnode(*outvar);
            out_ << " = ";
        }
        out_ << ghidra::get_opname(opc);
        for (ghidra::int4 i = 0; i < isize; ++i) {
            out_ << ' ';
            print_varnode(vars[i]);
        }
        out_ << '\n';
    }

private:
    void print_varnode(const ghidra::VarnodeData &v)
    {
        const std::string name = trans_->getRegisterName(v.space, v.offset, v.size);
        if (!name.empty()) {
            out_ << name;
            return;
        }
        out_ << '(' << v.space->getName() << ", 0x" << std::hex << v.offset << std::dec << ", "
             << v.size << ')';
    }

    std::ostringstream &out_;
    const ghidra::Translate *trans_;
};

// Collects every directory under `root` that holds at least one .ldefs file.
void collect_spec_dirs(const std::string &root, int depth, std::vector<std::string> &out)
{
    if (depth > 6)
        return;
    DIR *dir = opendir(root.c_str());
    if (dir == nullptr)
        return;
    bool has_ldefs = false;
    std::vector<std::string> subdirs;
    while (struct dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        const std::string full = root + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            subdirs.push_back(full);
        else if (name.size() > 6 && name.compare(name.size() - 6, 6, ".ldefs") == 0)
            has_ldefs = true;
    }
    closedir(dir);
    if (has_ldefs)
        out.push_back(root);
    for (const std::string &sub : subdirs)
        collect_spec_dirs(sub, depth + 1, out);
}

std::string type_text(ghidra::Datatype *type)
{
    if (type == nullptr)
        return "void";
    std::ostringstream s;
    type->printRaw(s);
    return s.str();
}

// An integer type wide enough for a varnode of this many bytes.
const char *width_type(int bytes)
{
    if (bytes <= 1)
        return "uint8_t";
    if (bytes <= 2)
        return "uint16_t";
    if (bytes <= 4)
        return "uint32_t";
    return "uint64_t";
}

// The C type of the expression printed for one argument of a call. Using the
// varnode's own type keeps the prototype agreeing with the call site; an array
// is written as a pointer, which is what it decays to when passed.
std::string argument_type(const ghidra::PcodeOp *call_op, int slot)
{
    const ghidra::Varnode *vn = call_op->getIn(slot);
    ghidra::Datatype *type = vn->getHighTypeReadFacing(call_op);
    if (type == nullptr || type->getMetatype() == ghidra::TYPE_VOID)
        return width_type(vn->getSize());
    if (type->getMetatype() == ghidra::TYPE_ARRAY) {
        const ghidra::TypeArray *array = static_cast<const ghidra::TypeArray *>(type);
        return type_text(array->getBase()) + " *";
    }
    return type_text(type);
}

// The C type of the value a call produces. As with arguments, the prototype has
// to agree with what the call site does: a result the printed code assigns
// cannot come from a function declared void.
std::string result_type(const ghidra::FuncProto &proto, const ghidra::PcodeOp *call_op)
{
    if (call_op != nullptr && !proto.isOutputLocked()) {
        const ghidra::Varnode *out = call_op->getOut();
        if (out != nullptr) {
            ghidra::Datatype *type = out->getHighTypeDefFacing();
            if (type != nullptr && type->getMetatype() != ghidra::TYPE_VOID)
                return type_text(type);
            return width_type(out->getSize());
        }
    }
    return type_text(proto.getOutputType());
}

// Renders a callable's C prototype.
//
// Where the decompiler locked down a prototype, that is what the call site
// prints and what gets emitted. Where it did not, the printed call still passes
// whatever the CALL operation carries, so the arguments are taken from there
// instead; a prototype that disagreed with its own call sites would not compile.
std::string prototype_text(const ghidra::FuncProto &proto, const std::string &name,
                           const ghidra::PcodeOp *call_op)
{
    int actual = -1;
    if (call_op != nullptr && !proto.isInputLocked())
        actual = static_cast<int>(call_op->numInput()) - 1;

    std::ostringstream s;
    s << "extern " << result_type(proto, call_op) << ' ' << name << '(';
    if (actual > proto.numParams()) {
        for (int i = 0; i < actual; ++i) {
            if (i != 0)
                s << ", ";
            s << argument_type(call_op, i + 1);
        }
        s << ");";
        return s.str();
    }

    const int count = proto.numParams();
    for (int i = 0; i < count; ++i) {
        if (i != 0)
            s << ", ";
        s << type_text(proto.getParam(i)->getType());
    }
    if (proto.isDotdotdot())
        s << (count == 0 ? "..." : ", ...");
    else if (count == 0)
        s << "void";
    s << ");";
    return s.str();
}

} // namespace

// ------------------------------------------------------------------ global

astral_status initialize(const char *spec_root)
{
    const std::string root =
        spec_root != nullptr && spec_root[0] != '\0' ? spec_root : default_spec_root();
    if (root.empty())
        return ASTRAL_ERR_SPECS_MISSING;

    // Ghidra's own scan expects a full installation tree. This library ships a
    // flat <root>/<Processor>/data/languages layout, so the directories holding
    // .ldefs files are collected directly.
    std::vector<std::string> spec_dirs;
    collect_spec_dirs(root, 0, spec_dirs);
    if (spec_dirs.empty())
        return ASTRAL_ERR_SPECS_MISSING;

    try {
        ghidra::startDecompilerLibrary(spec_dirs);
    } catch (ghidra::LowlevelError &err) {
        return ASTRAL_ERR_SPECS_MISSING;
    }
    g_initialized = true;
    if (ghidra::SleighArchitecture::getDescriptions().empty())
        return ASTRAL_ERR_SPECS_MISSING;
    return ASTRAL_OK;
}

void terminate()
{
    if (!g_initialized)
        return;
    ghidra::SleighArchitecture::shutdown();
    ghidra::shutdownDecompilerLibrary();
    g_initialized = false;
}

bool is_initialized() { return g_initialized; }

int language_count()
{
    if (!g_initialized)
        return 0;
    return static_cast<int>(ghidra::SleighArchitecture::getDescriptions().size());
}

const char *language_id_at(int index)
{
    const auto &all = ghidra::SleighArchitecture::getDescriptions();
    if (index < 0 || static_cast<size_t>(index) >= all.size())
        return nullptr;
    return all[index].getId().c_str();
}

const char *language_description_at(int index)
{
    const auto &all = ghidra::SleighArchitecture::getDescriptions();
    if (index < 0 || static_cast<size_t>(index) >= all.size())
        return nullptr;
    return all[index].getDescription().c_str();
}

bool compile_sleigh(const std::string &slaspec, const std::string &sla, std::string &error)
{
    try {
        ghidra::AttributeId::initialize();
        ghidra::ElementId::initialize();
        ghidra::SleighCompile compiler;
        std::map<std::string, std::string> defines;
        compiler.setAllOptions(defines, false, true, false, false, false, false, false, false);
        if (compiler.run_compilation(slaspec, sla) != 0) {
            error = "sleigh compilation of " + slaspec + " failed";
            return false;
        }
        return true;
    } catch (ghidra::LowlevelError &err) {
        error = err.explain;
        return false;
    }
}

// ----------------------------------------------------------------- session

Session::~Session()
{
    delete arch_;
}

std::unique_ptr<Session> Session::create(BinaryImage image, const std::string &language_override,
                                         std::string &error)
{
    if (!g_initialized) {
        error = "astral_init has not been called";
        return nullptr;
    }

    std::unique_ptr<Session> session(new Session());
    session->image_ = std::move(image);

    if (!language_override.empty())
        session->archid_ = complete_architecture(language_override, session->image_.arch.abi, error);
    else
        session->archid_ = choose_architecture(session->image_.arch, error);
    if (session->archid_.empty())
        return nullptr;

    try {
        StandaloneArchitecture *arch =
            new StandaloneArchitecture(session->image_, session->archid_, &session->messages_);
        ghidra::DocumentStorage store;
        arch->init(store);
        session->arch_ = arch;
    } catch (ghidra::LowlevelError &err) {
        error = err.explain;
        const std::string messages = session->messages_.str();
        if (!messages.empty())
            error += " (" + messages + ")";
        return nullptr;
    } catch (ghidra::DecoderError &err) {
        error = err.explain;
        return nullptr;
    }

    // Register the symbols the loader recovered so calls print by name.
    try {
        session->arch_->readLoaderSymbols("::");
    } catch (ghidra::LowlevelError &err) {
        // Symbol import is best-effort; a bad entry must not sink the session.
    }
    // Typing the library functions it imports is what lets string arguments
    // print as strings rather than as addresses.
    try {
        apply_library_prototypes(session->arch_);
    } catch (ghidra::LowlevelError &err) {
    }
    // A body the user has named before comes back with that name.
    try {
        session->apply_learned_names();
    } catch (ghidra::LowlevelError &err) {
    }
    return session;
}

std::string Session::language_id() const
{
    size_t last = archid_.rfind(':');
    return last == std::string::npos ? archid_ : archid_.substr(0, last);
}

std::string Session::compiler_spec() const
{
    size_t last = archid_.rfind(':');
    return last == std::string::npos ? std::string() : archid_.substr(last + 1);
}

bool Session::big_endian() const
{
    return arch_ != nullptr && arch_->translate != nullptr && arch_->translate->isBigEndian();
}

int Session::pointer_size() const
{
    if (arch_ == nullptr)
        return 0;
    return arch_->getDefaultCodeSpace()->getAddrSize();
}

bool Session::add_symbol(uint64_t address, const std::string &name, bool is_function,
                         std::string &error)
{
    try {
        ghidra::Address addr(arch_->getDefaultCodeSpace(), address);
        ghidra::Scope *scope = arch_->symboltab->getGlobalScope();
        if (is_function)
            scope->addFunction(addr, name);
        else
            scope->addSymbol(name, arch_->types->getBase(pointer_size(), ghidra::TYPE_UNKNOWN),
                             addr, addr);
        Symbol sym;
        sym.address = address;
        sym.name = name;
        sym.is_function = is_function;
        image_.symbols.push_back(std::move(sym));
        image_.sort_and_dedup_symbols();
        globals_valid_ = false;
        return true;
    } catch (ghidra::LowlevelError &err) {
        error = err.explain;
        return false;
    }
}

bool Session::set_option(const std::string &name, const std::string &value, std::string &error)
{
    try {
        ghidra::uint4 id = ghidra::ElementId::find(name, 0);
        if (id == 0) {
            error = "no decompiler option named '" + name + "'";
            return false;
        }
        std::string message = arch_->options->set(id, value, "", "");
        (void)message;
        return true;
    } catch (ghidra::ParseError &err) {
        error = err.explain;
        return false;
    } catch (ghidra::LowlevelError &err) {
        error = err.explain;
        return false;
    }
}

bool Session::disassemble(uint64_t address, int count, std::string &out, std::string &error)
{
    if (count <= 0) {
        error = "instruction count must be positive";
        return false;
    }
    std::ostringstream s;
    StringAssemblyEmit emit(s);
    ghidra::Address addr(arch_->getDefaultCodeSpace(), address);
    // An undecodable byte ends the listing rather than discarding what decoded
    // before it, which is what a caller walking unknown bytes needs.
    for (int i = 0; i < count; ++i) {
        try {
            int len = arch_->translate->printAssembly(emit, addr);
            if (len <= 0)
                break;
            addr = addr + len;
        } catch (ghidra::LowlevelError &err) {
            if (i == 0) {
                error = err.explain;
                return false;
            }
            break;
        }
    }
    out = s.str();
    return true;
}

bool Session::pcode(uint64_t address, int count, std::string &out, std::string &error)
{
    if (count <= 0) {
        error = "instruction count must be positive";
        return false;
    }
    std::ostringstream s;
    StringPcodeEmit emit(s, arch_->translate);
    ghidra::Address addr(arch_->getDefaultCodeSpace(), address);
    for (int i = 0; i < count; ++i) {
        try {
            int len = arch_->translate->oneInstruction(emit, addr);
            if (len <= 0)
                break;
            addr = addr + len;
        } catch (ghidra::LowlevelError &err) {
            if (i == 0) {
                error = err.explain;
                return false;
            }
            break;
        }
    }
    out = s.str();
    return true;
}

namespace {

// The no-op encoding for an architecture, repeated to fill `total` bytes. Only
// the encodings whose patch tier is exact live here; anything else asks the
// caller to supply bytes rather than guessing.
bool arch_nop_fill(const std::string &archid, bool big_endian, int total,
                   std::vector<uint8_t> &out, std::string &error)
{
    auto has = [&](const char *needle) { return archid.find(needle) != std::string::npos; };
    if (has("x86")) {
        // 0x90 is a one-byte no-op, so any run of them fills exactly.
        out.assign(static_cast<size_t>(total), 0x90);
        return true;
    }
    if (has("AARCH64")) {
        if (total % 4 != 0) {
            error = "arm64 no-op must cover whole 4-byte instructions";
            return false;
        }
        uint8_t nop[4] = {0x1f, 0x20, 0x03, 0xd5}; // little-endian D503201F
        if (big_endian)
            std::swap(nop[0], nop[3]), std::swap(nop[1], nop[2]);
        out.clear();
        for (int i = 0; i < total; i += 4)
            out.insert(out.end(), nop, nop + 4);
        return true;
    }
    error = "no built-in no-op for this architecture yet; supply raw bytes instead";
    return false;
}

} // namespace

int Session::instruction_length(uint64_t address) const
{
    if (arch_ == nullptr || arch_->translate == nullptr)
        return 0;
    try {
        ghidra::Address addr(arch_->getDefaultCodeSpace(), address);
        return arch_->translate->instructionLength(addr);
    } catch (ghidra::LowlevelError &) {
        return 0;
    }
}

bool Session::patch_bytes(uint64_t address, const std::vector<uint8_t> &bytes, PatchTier tier,
                          const std::string &note, std::string &error)
{
    if (bytes.empty()) {
        error = "nothing to write";
        return false;
    }
    uint64_t file_off = 0;
    if (!image_.file_offset_for(address, file_off)) {
        error = "address 0x" + [&] {
            char b[24];
            std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(address));
            return std::string(b);
        }() + " has no file bytes to patch";
        return false;
    }
    // Read the bytes there now, both to record the original and to confirm the
    // whole span is file-backed.
    std::vector<uint8_t> original(bytes.size());
    size_t covered = image_.read(address, original.data(), original.size());
    if (covered < original.size()) {
        error = "patch runs past mapped file bytes";
        return false;
    }
    for (size_t i = 1; i < bytes.size(); ++i) {
        uint64_t probe = 0;
        if (!image_.file_offset_for(address + i, probe) || probe != file_off + i) {
            error = "patch would cross a segment boundary";
            return false;
        }
    }
    Patch p;
    p.address = address;
    p.file_offset = file_off;
    p.original = std::move(original);
    p.replacement = bytes;
    p.tier = tier;
    p.note = note;
    patches_.add(std::move(p));
    // Reflect the edit in the in-memory image so a re-decompile or a fresh
    // disassembly shows the patched bytes at once, not only after write-back.
    image_.write(address, bytes.data(), bytes.size());
    return true;
}

bool Session::undo_patch()
{
    if (patches_.empty())
        return false;
    const Patch &last = patches_.patches().back();
    image_.write(last.address, last.original.data(), last.original.size());
    patches_.undo_last();
    return true;
}

bool Session::patch_nop(uint64_t address, int instruction_count, std::string &error)
{
    if (instruction_count <= 0) {
        error = "instruction count must be positive";
        return false;
    }
    uint64_t at = address;
    int total = 0;
    for (int i = 0; i < instruction_count; ++i) {
        int len = instruction_length(at);
        if (len <= 0) {
            error = "cannot decode the instruction to replace";
            return false;
        }
        total += len;
        at += static_cast<uint64_t>(len);
    }
    std::vector<uint8_t> fill;
    if (!arch_nop_fill(archid_, big_endian(), total, fill, error))
        return false;
    return patch_bytes(address, fill, PatchTier::ByteRewrite,
                       instruction_count == 1 ? "no-op one instruction"
                                              : "no-op instructions", error);
}

bool Session::patch_invert_branch(uint64_t address, std::string &error)
{
    std::vector<uint8_t> insn(4);
    if (image_.read(address, insn.data(), 4) < 4) {
        error = "cannot read the branch instruction";
        return false;
    }
    bool arm = archid_.find("AARCH64") != std::string::npos;
    bool x86 = archid_.find("x86") != std::string::npos;
    if (arm) {
        uint32_t w = uint32_t(insn[0]) | (uint32_t(insn[1]) << 8) | (uint32_t(insn[2]) << 16) |
                     (uint32_t(insn[3]) << 24);
        // B.cond: 0101010 0 imm19 0 cond -> flip the low bit of the condition.
        if ((w & 0xff000010u) == 0x54000000u) {
            w ^= 1u;
        }
        // CBZ/CBNZ: sf 011010 op imm19 Rt -> flip bit 24 (op).
        else if ((w & 0x7e000000u) == 0x34000000u) {
            w ^= (1u << 24);
        }
        // TBZ/TBNZ: b5 011011 op b40 imm14 Rt -> flip bit 24 (op).
        else if ((w & 0x7e000000u) == 0x36000000u) {
            w ^= (1u << 24);
        } else {
            error = "not a conditional branch at that address";
            return false;
        }
        std::vector<uint8_t> out = {uint8_t(w), uint8_t(w >> 8), uint8_t(w >> 16), uint8_t(w >> 24)};
        return patch_bytes(address, out, PatchTier::ByteRewrite, "invert branch", error);
    }
    if (x86) {
        // Short Jcc: 0x70-0x7f, invert by flipping bit 0 of the opcode.
        if (insn[0] >= 0x70 && insn[0] <= 0x7f) {
            std::vector<uint8_t> out = {uint8_t(insn[0] ^ 1u)};
            return patch_bytes(address, out, PatchTier::ByteRewrite, "invert branch", error);
        }
        // Near Jcc: 0x0f 0x80-0x8f.
        if (insn[0] == 0x0f && insn[1] >= 0x80 && insn[1] <= 0x8f) {
            std::vector<uint8_t> out = {0x0f, uint8_t(insn[1] ^ 1u)};
            return patch_bytes(address, out, PatchTier::ByteRewrite, "invert branch", error);
        }
        error = "not a conditional jump at that address";
        return false;
    }
    error = "inverting branches is not built in for this architecture yet";
    return false;
}

bool Session::patch_return(uint64_t address, uint64_t value, std::string &error)
{
    bool arm = archid_.find("AARCH64") != std::string::npos;
    bool x86 = archid_.find("x86") != std::string::npos;
    if (arm) {
        if (value > 0xffff) {
            error = "arm64 return shortcut supports values up to 0xffff";
            return false;
        }
        // movz w0, #value ; ret
        uint32_t movz = 0x52800000u | (uint32_t(value & 0xffff) << 5);
        uint32_t ret = 0xd65f03c0u;
        std::vector<uint8_t> out = {
            uint8_t(movz), uint8_t(movz >> 8), uint8_t(movz >> 16), uint8_t(movz >> 24),
            uint8_t(ret),  uint8_t(ret >> 8),  uint8_t(ret >> 16),  uint8_t(ret >> 24)};
        return patch_bytes(address, out, PatchTier::ByteRewrite, "return a constant", error);
    }
    if (x86) {
        // mov eax, imm32 ; ret
        uint32_t v = uint32_t(value);
        std::vector<uint8_t> out = {0xb8, uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16),
                                    uint8_t(v >> 24), 0xc3};
        return patch_bytes(address, out, PatchTier::ByteRewrite, "return a constant", error);
    }
    error = "the return shortcut is not built in for this architecture yet";
    return false;
}

bool Session::write_patched(const std::string &out_path, std::string &error) const
{
    if (image_.path.empty()) {
        error = "no source file on disk to patch";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!read_file(image_.path, bytes, error))
        return false;
    if (!patches_.apply_to(bytes, error))
        return false;
    // A patched arm64 Mach-O will not run until it is re-signed: Apple Silicon
    // refuses a binary whose signature no longer covers its bytes. Re-sign in
    // memory when possible, and fall back to the platform tool otherwise.
    bool sign_on_disk = false;
    if (is_macho(bytes)) {
        std::string sign_error;
        if (!macho_adhoc_sign(bytes, sign_error))
            sign_on_disk = true; // native path declined; try codesign after write
    }
    if (!write_file(out_path, bytes, error))
        return false;
    // A patched executable should stay executable.
    {
        struct stat st;
        if (stat(image_.path.c_str(), &st) == 0)
            chmod(out_path.c_str(), st.st_mode);
    }
    if (sign_on_disk) {
        std::string sign_error;
        if (!codesign_adhoc(out_path, sign_error)) {
            // The bytes are written; only the signature is missing. Say so
            // rather than pretend the patch produced a runnable binary.
            error = "patched, but could not re-sign: " + sign_error;
            return false;
        }
    }
    return true;
}

bool Session::decompile(uint64_t address, const std::string &name, FunctionResult &out,
                        std::string &error)
{
    if (!image_.contains(address)) {
        error = "address is not mapped by the image";
        return false;
    }
    try {
        ghidra::Address addr(arch_->getDefaultCodeSpace(), address);
        ghidra::Scope *global = arch_->symboltab->getGlobalScope();
        ghidra::Funcdata *fd = global->queryFunction(addr);
        if (fd == nullptr) {
            std::string funcname = name;
            if (funcname.empty())
                arch_->nameFunction(addr, funcname);
            fd = global->addFunction(addr, funcname)->getFunction();
        } else if (!name.empty() && fd->getName() != name) {
            // A caller-supplied name wins over whatever the loader recorded.
            arch_->symboltab->getGlobalScope()->renameSymbol(fd->getSymbol(), name);
        }

        if (fd->hasNoCode()) {
            error = "no code at that address";
            return false;
        }
        if (fd->isProcStarted())
            arch_->clearAnalysis(fd);

        ghidra::AddrSpace *space = addr.getSpace();
        fd->followFlow(ghidra::Address(space, 0), ghidra::Address(space, space->getHighest()));

        arch_->allacts.getCurrent()->reset(*fd);
        arch_->allacts.getCurrent()->perform(*fd);

        analyse_function(fd, out);

        // Following flow can create function symbols for call targets.
        globals_valid_ = false;

        // A function named on an earlier pass carries no fresh reason on this
        // one; hand back the reason recorded when the name was chosen.
        if (out.naming_reason.empty()) {
            auto cached = naming_reasons_.find(address);
            if (cached != naming_reasons_.end())
                out.naming_reason = cached->second;
        }

        if (auto_naming_) {
            const std::string chosen = apply_naming(fd, out);
            if (!chosen.empty()) {
                // The recovered name is cached in the function and in every
                // call site that refers to it, so the function is rebuilt under
                // its new name rather than relabelled after the fact.
                const std::string reason = out.naming_reason;
                global->removeSymbol(fd->getSymbol());
                fd = global->addFunction(addr, chosen)->getFunction();
                // Now that it has a name, anything known about a function of
                // that name applies: the prototype read from the source that
                // built it gives real argument types and argument names.
                apply_known_prototype(chosen);
                ghidra::AddrSpace *again = addr.getSpace();
                fd->followFlow(ghidra::Address(again, 0),
                               ghidra::Address(again, again->getHighest()));
                arch_->allacts.getCurrent()->reset(*fd);
                arch_->allacts.getCurrent()->perform(*fd);
                analyse_function(fd, out);
                out.naming_reason = reason;
                // Name its values again, now that the body is final.
                apply_naming(fd, out);
            }
        }

        realize_c(out);
        collect_externals(out, fd);
        return true;
    } catch (ghidra::LowlevelError &err) {
        error = err.explain;
        return false;
    } catch (ghidra::DecoderError &err) {
        error = err.explain;
        return false;
    }
}


const std::map<std::string, Session::GlobalSymbol> &Session::globals() const
{
    if (globals_valid_)
        return globals_;
    globals_.clear();
    const ghidra::Scope *scope = arch_->symboltab->getGlobalScope();
    for (ghidra::MapIterator it = scope->begin(); it != scope->end(); ++it) {
        const ghidra::SymbolEntry *entry = *it;
        if (entry == nullptr || entry->isPiece())
            continue;
        ghidra::Symbol *symbol = entry->getSymbol();
        if (symbol == nullptr || symbol->getName().empty())
            continue;
        GlobalSymbol record;
        record.address = entry->getAddr().getOffset();
        record.is_function = dynamic_cast<ghidra::FunctionSymbol *>(symbol) != nullptr;
        record.type_text = type_text(symbol->getType());
        globals_.emplace(symbol->getName(), std::move(record));
    }
    globals_valid_ = true;
    return globals_;
}

// Works out what the emitted body references without declaring, and renders a C
// declaration for each: an exact prototype for a call, the recovered type for a
// global, and a width-appropriate guess for an unnamed memory location.
void Session::collect_externals(FunctionResult &result, const void *funcdata) const
{
    const ghidra::Funcdata *fd = static_cast<const ghidra::Funcdata *>(funcdata);

    std::set<std::string> declared;
    declared.insert(result.name);
    for (const std::string &name : result.parameter_names)
        declared.insert(name);
    for (const std::string &name : result.local_names)
        declared.insert(name);

    // Prototypes for the call sites, which know the exact argument types. A
    // target with no symbol is printed under a name derived from its address,
    // so register the prototype under that spelling as well.
    std::map<std::string, std::string> call_prototypes;
    for (int i = 0; i < fd->numCalls(); ++i) {
        const ghidra::FuncCallSpecs *call = fd->getCallSpecs(i);
        std::string names[2];
        names[0] = result.callee_names[static_cast<size_t>(i)];
        if (!call->getEntryAddress().isInvalid())
            arch_->nameFunction(call->getEntryAddress(), names[1]);
        for (const std::string &name : names) {
            if (name.empty() || name == result.name)
                continue;
            const std::string prototype = prototype_text(*call, name, call->getOp());
            auto existing = call_prototypes.find(name);
            if (existing == call_prototypes.end()) {
                call_prototypes.emplace(name, prototype);
            } else if (existing->second != prototype) {
                // Two call sites disagree about the same function, which
                // happens where the decompiler recovered different argument
                // counts. A prototype that contradicts a call site is worse
                // than none, so neither is stated.
                existing->second = "extern long long " + name + "();";
            }
        }
    }

    const std::map<std::string, GlobalSymbol> &known = globals();
    std::set<std::string> seen;
    for (const Identifier &identifier : scan_identifiers(result.c_code_real)) {
        const std::string &name = identifier.name;
        if (declared.count(name) != 0 || seen.count(name) != 0)
            continue;
        if (is_runtime_identifier(name))
            continue;
        seen.insert(name);

        Declaration declaration;
        declaration.name = name;

        auto prototype = call_prototypes.find(name);
        if (prototype != call_prototypes.end()) {
            declaration.text = prototype->second;
            declaration.is_function = true;
            auto known_entry = known.find(name);
            if (known_entry != known.end())
                declaration.address = known_entry->second.address;
            result.externals.push_back(std::move(declaration));
            continue;
        }

        auto known_entry = known.find(name);
        if (known_entry != known.end()) {
            declaration.address = known_entry->second.address;
            declaration.is_function = known_entry->second.is_function;
            if (known_entry->second.is_function) {
                ghidra::Funcdata *callee =
                    arch_->symboltab->getGlobalScope()->queryFunction(name);
                declaration.text = callee != nullptr
                    ? prototype_text(callee->getFuncProto(), name, nullptr)
                    : "extern long long " + name + "();";
            } else {
                declaration.text =
                    "extern " + format_declaration(known_entry->second.type_text, name) + ";";
            }
            result.externals.push_back(std::move(declaration));
            continue;
        }

        // Not a symbol: an unnamed location the printer named after its type and
        // address, such as iRam000000010000c068 or a call target.
        declaration.is_function = identifier.called;
        if (identifier.called) {
            declaration.text = "extern long long " + name + "();";
        } else {
            declaration.text = "extern " + unnamed_location_type(name) + " " + name + ";";
        }
        result.externals.push_back(std::move(declaration));
    }
}


// Reads everything Astral reports about a function out of the decompiled form.
void Session::analyse_function(void *funcdata, FunctionResult &out)
{
    ghidra::Funcdata *fd = static_cast<ghidra::Funcdata *>(funcdata);
    ghidra::Scope *global = arch_->symboltab->getGlobalScope();
    out = FunctionResult();
    std::ostringstream code;
    arch_->print->setOutputStream(&code);
    arch_->print->docFunction(fd);

    out.name = fd->getName();
    out.address = fd->getAddress().getOffset();
    out.size = static_cast<uint64_t>(fd->getSize());
    out.c_code = code.str();

    const ghidra::FuncProto &proto = fd->getFuncProto();
    out.calling_convention = proto.getModelName();
    out.return_type = type_text(proto.getOutputType());

    std::ostringstream sig;
    sig << out.return_type << ' ' << out.name << '(';
    for (int i = 0; i < proto.numParams(); ++i) {
        ghidra::ProtoParameter *param = proto.getParam(i);
        std::string ptype = type_text(param->getType());
        out.parameter_types.push_back(ptype);
        out.parameter_names.push_back(param->getName());
        if (i != 0)
            sig << ", ";
        sig << ptype << ' ' << param->getName();
    }
    if (proto.numParams() == 0)
        sig << "void";
    sig << ')';
    out.signature = sig.str();

    const ghidra::Scope *locals = fd->getScopeLocal();
    for (ghidra::MapIterator it = locals->begin(); it != locals->end(); ++it) {
        const ghidra::SymbolEntry *entry = *it;
        if (entry->isPiece())
            continue;
        ghidra::Symbol *sym = entry->getSymbol();
        if (sym == nullptr || sym->getName().empty())
            continue;
        if (dynamic_cast<ghidra::FunctionSymbol *>(sym) != nullptr)
            continue;
        if (sym->getCategory() == 0) // formal parameters, already reported
            continue;
        out.local_names.push_back(sym->getName());
        out.local_types.push_back(type_text(sym->getType()));
    }

    for (int i = 0; i < fd->numCalls(); ++i) {
        const ghidra::FuncCallSpecs *call = fd->getCallSpecs(i);
        const ghidra::Address &entry = call->getEntryAddress();
        // An indirect call has no static target, and its address carries no
        // space; asking anything of it would dereference null.
        if (entry.isInvalid()) {
            out.callees.push_back(0);
            out.callee_names.push_back(std::string());
            continue;
        }
        out.callees.push_back(entry.getOffset());
        ghidra::Funcdata *callee = global->queryFunction(entry);
        out.callee_names.push_back(callee != nullptr ? callee->getName() : std::string());
    }

    const ghidra::BlockGraph &blocks = fd->getBasicBlocks();
    for (int i = 0; i < blocks.getSize(); ++i)
        out.block_addresses.push_back(blocks.getBlock(i)->getStart().getOffset());

}

// Names what the decompiler could only number, using the evidence the body
// still carries, then prints it again so the names appear throughout.
std::string Session::apply_naming(void *funcdata, FunctionResult &out)
{
    ghidra::Funcdata *fd = static_cast<ghidra::Funcdata *>(funcdata);
    const Knowledge &knowledge = Knowledge::instance();

    // A name the user chose for this same body, in this or any other program,
    // outranks anything Astral could infer: it is the one piece of evidence
    // that came from someone who knew.
    std::string learned;
    if (knowledge.is_placeholder(out.name))
        learned = learned_name_for(out.address, out.size);

    NamingResult naming = analyse(out.c_code, out.name, out.callee_names, out.local_names,
                                  out.parameter_names, knowledge);
    if (!learned.empty()) {
        naming.function_name = learned;
        naming.function_reason = "you named this body before";
    }
    if (naming.empty())
        return std::string();

    bool renamed_function = false;

    // Local variables first: renaming these changes nothing else, so it is safe
    // even when the evidence is only suggestive.
    if (!naming.variables.empty()) {
        ghidra::Scope *locals = fd->getScopeLocal();
        for (const auto &rename : naming.variables) {
            ghidra::Symbol *symbol = nullptr;
            for (ghidra::MapIterator it = locals->begin(); it != locals->end(); ++it) {
                const ghidra::SymbolEntry *entry = *it;
                if (entry == nullptr || entry->isPiece())
                    continue;
                if (entry->getSymbol() != nullptr && entry->getSymbol()->getName() == rename.first) {
                    symbol = entry->getSymbol();
                    break;
                }
            }
            if (symbol == nullptr)
                continue;
            try {
                locals->renameSymbol(symbol, rename.second);
                out.applied_renames.push_back(rename);
                // The recorded names have to follow, or a value Astral just
                // named reads as undeclared and gets an extern of its own.
                for (std::string &name : out.local_names)
                    if (name == rename.first)
                        name = rename.second;
                for (std::string &name : out.parameter_names)
                    if (name == rename.first)
                        name = rename.second;
            } catch (ghidra::LowlevelError &) {
                // A name the scope will not accept is simply not applied.
            }
        }
    }

    // The function's own name is a stronger claim, so it is only made when the
    // current one carries no information. The caller performs it, because a
    // function has to be rebuilt under its name for that name to propagate.
    std::string proposed_name;
    if (!naming.function_name.empty() && knowledge.is_placeholder(out.name)) {
        ghidra::Scope *global = arch_->symboltab->getGlobalScope();
        proposed_name = naming.function_name;
        for (int suffix = 2; global->queryFunction(proposed_name) != nullptr && suffix < 100;
             ++suffix)
            proposed_name = naming.function_name + std::to_string(suffix);
        out.naming_reason = proposed_name + ": " + naming.function_reason;
        naming_reasons_[out.address] = out.naming_reason;
        renamed_function = true;
    }

    out.comments = naming.comments;

    if (!renamed_function && !out.applied_renames.empty()) {
        // Renaming locals changes nothing the analysis depends on, so printing
        // again is enough to make the new names appear throughout.
        std::ostringstream code;
        arch_->print->setOutputStream(&code);
        arch_->print->docFunction(fd);
        out.c_code = code.str();
    }
    return proposed_name;
}

// The name the knowledge base holds for the body at this address, if any.
//
// Where a function ends is rarely known exactly: the decompiler's recovered
// size and the size a symbol table reported are usually different numbers for
// the same code. So rather than trusting one length, every length the database
// holds is tried, and the longest match wins because it is the most specific.
std::string Session::learned_name_for(uint64_t address, uint64_t size) const
{
    const Knowledge &knowledge = Knowledge::instance();
    const std::set<uint32_t> &lengths = knowledge.signature_lengths();
    if (lengths.empty())
        return std::string();

    const uint32_t longest = *lengths.rbegin();
    size_t window = std::min<size_t>(longest, 1u << 18);
    if (size > 0)
        window = std::max<size_t>(window, static_cast<size_t>(std::min<uint64_t>(size, 1u << 18)));

    std::vector<uint8_t> body(window);
    const size_t got = image_.read(address, body.data(), body.size());
    if (got < 8)
        return std::string();
    body.resize(got);

    const std::string processor = language_id().substr(0, language_id().find(':'));
    std::string best;
    uint32_t best_length = 0;
    fingerprint_prefixes(body.data(), body.size(), processor, lengths,
                         [&](uint32_t length, uint64_t hash) {
                             const std::string name = knowledge.signature_name(hash, length);
                             if (!name.empty() && length > best_length) {
                                 best_length = length;
                                 best = name;
                             }
                         });
    return best;
}

// Gives a named function the prototype the knowledge base holds for that name.
void Session::apply_known_prototype(const std::string &name)
{
    const std::string declaration = Knowledge::instance().prototype_for(name);
    if (declaration.empty())
        return;
    try {
        std::istringstream stream(size_types_for(declaration));
        ghidra::parse_C(arch_, stream);
    } catch (ghidra::ParseError &) {
    } catch (ghidra::LowlevelError &) {
    }
}

int Session::learn_symbols(std::string &error)
{
    Knowledge &knowledge = Knowledge::instance();
    const std::string processor = language_id().substr(0, language_id().find(':'));
    int learned = 0;

    for (const Symbol &symbol : image_.symbols) {
        // Only bodies this program actually contains, under names someone
        // meant. An import stub is another image's code, and a placeholder
        // name teaches nothing.
        if (!symbol.is_function || symbol.is_import)
            continue;
        if (symbol.name.empty() || knowledge.is_placeholder(symbol.name))
            continue;
        if (symbol.size < 16 || symbol.size >= (1u << 20))
            continue;

        std::vector<uint8_t> body(static_cast<size_t>(symbol.size));
        if (image_.read(symbol.address, body.data(), body.size()) != body.size())
            continue;
        uint64_t hash = 0;
        if (!fingerprint(body.data(), body.size(), processor, hash))
            continue;
        if (knowledge.signature_name(hash, static_cast<uint32_t>(symbol.size)) == symbol.name)
            continue;
        std::string learn_error;
        if (knowledge.learn_signature(hash, static_cast<uint32_t>(symbol.size), symbol.name,
                                      learn_error)) {
            ++learned;
        } else if (error.empty()) {
            error = learn_error;
        }
    }
    return learned;
}

bool Session::rename(uint64_t address, const std::string &name, bool learn, std::string &error)
{
    if (name.empty()) {
        error = "a rename needs a name";
        return false;
    }
    try {
        ghidra::Address addr(arch_->getDefaultCodeSpace(), address);
        ghidra::Scope *global = arch_->symboltab->getGlobalScope();
        ghidra::Funcdata *fd = global->queryFunction(addr);
        if (fd != nullptr) {
            // A function caches its own name, and so does every call site that
            // refers to it, so the old one is discarded and rebuilt rather than
            // relabelled. The next decompilation analyses it under its name.
            global->removeSymbol(fd->getSymbol());
        }
        global->addFunction(addr, name);
        globals_valid_ = false;

        for (Symbol &symbol : image_.symbols)
            if (symbol.address == address)
                symbol.name = name;

        if (learn) {
            // Remember the body, not the address: the same code in another
            // program should come back with the name the user chose.
            uint64_t size = fd != nullptr ? static_cast<uint64_t>(fd->getSize()) : 0;
            for (const Symbol &symbol : image_.symbols)
                if (symbol.address == address && symbol.size != 0)
                    size = symbol.size;
            if (size >= 8 && size < (1u << 20)) {
                std::vector<uint8_t> body(static_cast<size_t>(size));
                if (image_.read(address, body.data(), body.size()) == body.size()) {
                    uint64_t hash = 0;
                    const std::string processor = language_id().substr(0, language_id().find(':'));
                    if (fingerprint(body.data(), body.size(), processor, hash)) {
                        std::string learn_error;
                        Knowledge::instance().learn_signature(
                            hash, static_cast<uint32_t>(size), name, learn_error);
                    }
                }
            }
        }
        return true;
    } catch (ghidra::LowlevelError &err) {
        error = err.explain;
        return false;
    }
}

// Gives a function the name the user chose for the same body in an earlier
// program, if the knowledge base has seen it.
void Session::apply_learned_names()
{
    const Knowledge &knowledge = Knowledge::instance();
    const std::string processor = language_id().substr(0, language_id().find(':'));
    ghidra::Scope *global = arch_->symboltab->getGlobalScope();

    for (Symbol &symbol : image_.symbols) {
        if (!symbol.is_function || symbol.is_import || symbol.size < 8)
            continue;
        if (!knowledge.is_placeholder(symbol.name) && !symbol.name.empty())
            continue;
        std::vector<uint8_t> body(static_cast<size_t>(symbol.size));
        if (image_.read(symbol.address, body.data(), body.size()) != body.size())
            continue;
        uint64_t hash = 0;
        if (!fingerprint(body.data(), body.size(), processor, hash))
            continue;
        const std::string learned =
            knowledge.signature_name(hash, static_cast<uint32_t>(symbol.size));
        if (learned.empty())
            continue;
        symbol.name = learned;
        try {
            ghidra::Address addr(arch_->getDefaultCodeSpace(), symbol.address);
            global->addFunction(addr, learned);
        } catch (ghidra::LowlevelError &) {
        }
    }
    globals_valid_ = false;
}

namespace {

// The recovered code sometimes reaches memory by its absolute address in the
// original image - a lookup table at 0x1000006c8, a .bss buffer it builds at
// 0x100008000. Those addresses mean nothing in a standalone rebuild, so the
// program links but faults. This defines each referenced data address as a real
// C array (the bytes for read-only data, zero for .bss the code fills itself)
// and rewrites the address to point at that array, so the rebuilt program runs.
// Replaces the decompiler's long address-encoded names with short, conventional
// ones, so the source reads the way a person writes it: pxRam0000000100004130
// becomes g_100004130, func_0x000100000e5c becomes sub_e5c, and a goto label
// code_r0x000100000758 becomes L_758. Whole-token, so every use stays in step.
void tidy_names(std::string &out)
{
    struct Rule { std::regex re; std::string rep; };
    static const std::vector<Rule> rules = {
        {std::regex(R"(\b[A-Za-z]{1,4}Ram0*([0-9A-Fa-f]+)\b)"), "g_$1"},
        {std::regex(R"(\bfunc_0x0*([0-9A-Fa-f]+)\b)"), "sub_$1"},
        {std::regex(R"(\b(?:code_r|joined_r)0x0*([0-9A-Fa-f]+)\b)"), "L_$1"},
    };
    for (const Rule &r : rules)
        out = std::regex_replace(out, r.re, r.rep);
}

void emit_absolute_data(BinaryImage &image, const std::vector<std::pair<uint64_t, uint64_t>> &code,
                        std::string &out)
{
    auto in_code = [&](uint64_t a) {
        for (const auto &r : code)
            if (a >= r.first && a < r.second)
                return true;
        return false;
    };
    // Collect the distinct address literals that land in mapped data.
    std::set<uint64_t> addrs;
    static const std::regex hex(R"(\b0x[0-9A-Fa-f]{5,16}\b)");
    for (auto it = std::sregex_iterator(out.begin(), out.end(), hex);
         it != std::sregex_iterator(); ++it) {
        uint64_t a = std::strtoull(it->str().c_str() + 2, nullptr, 16);
        if (!image.contains(a) || in_code(a))
            continue;
        addrs.insert(a);
    }
    if (addrs.empty())
        return;

    // Size each array up to the next referenced address in the same segment, or
    // the segment's end, capped so an over-read stays bounded.
    std::vector<uint64_t> sorted(addrs.begin(), addrs.end());
    std::sort(sorted.begin(), sorted.end());
    // A fixed window rather than the distance to the next referenced address:
    // the decompiler often reaches one array through several bases a few bytes
    // apart (opcodes at N, operands at N+1), and truncating the first at the
    // second would leave it a byte long. Overlapping windows are harmless -
    // each holds the real bytes at its own base - and reading past the segment
    // just zero-fills.
    const uint64_t kWindow = 8192;
    std::ostringstream defs;
    for (size_t i = 0; i < sorted.size(); ++i) {
        uint64_t a = sorted[i];
        uint64_t seg_end = a;
        for (const Segment &s : image.segments)
            if (a >= s.address && a < s.address + s.size)
                seg_end = s.address + s.size;
        uint64_t size = seg_end - a;
        if (size > kWindow)
            size = kWindow;
        if (size == 0)
            size = 1;
        std::vector<uint8_t> bytes(size, 0);
        image.read(a, bytes.data(), bytes.size());
        char name[32];
        std::snprintf(name, sizeof name, "DAT_%llx", static_cast<unsigned long long>(a));
        defs << "static unsigned char " << name << "[" << size << "] = {";
        for (size_t b = 0; b < bytes.size(); ++b) {
            if (b % 16 == 0)
                defs << "\n  ";
            char byte[8];
            std::snprintf(byte, sizeof byte, "0x%02x,", bytes[b]);
            defs << byte;
        }
        defs << "\n};\n";
        // Rewrite every use of the literal to the array (it decays to a pointer).
        char literal[24];
        std::snprintf(literal, sizeof literal, "0x%llx", static_cast<unsigned long long>(a));
        std::string from = literal, to = name;
        for (size_t at = out.find(from); at != std::string::npos; at = out.find(from, at)) {
            // Only a whole token, so 0x100 inside 0x1008 is never touched.
            bool left = at > 0 && (std::isalnum((unsigned char)out[at - 1]) || out[at - 1] == '_');
            size_t end = at + from.size();
            bool right = end < out.size() &&
                         (std::isalnum((unsigned char)out[end]) || out[end] == '_');
            if (left || right) {
                at = end;
                continue;
            }
            out.replace(at, from.size(), to);
            at += to.size();
        }
    }
    // Place the definitions after the last #include so they precede all code.
    size_t inc = out.rfind("#include");
    size_t at = inc == std::string::npos ? 0 : out.find('\n', inc) + 1;
    out.insert(at, "\n" + defs.str());
}

} // namespace

bool Session::emit_c(const std::vector<uint64_t> &addresses, bool self_contained, bool comments,
                     bool explain, std::string &out, std::string &error)
{
    if (addresses.empty()) {
        error = "no functions to emit";
        return false;
    }
    // Addresses that name a library import, so a call to one is left as an
    // external declaration rather than decompiled into a stub body.
    std::set<uint64_t> imports;
    for (const Symbol &sym : image_.symbols)
        if (sym.is_import)
            imports.insert(sym.address);
    std::set<uint64_t> entries(image_.entry_points.begin(), image_.entry_points.end());
    auto name_for = [&](uint64_t addr) { return entries.count(addr) ? std::string("main")
                                                                    : std::string(); };
    auto in_code = [&](uint64_t addr) {
        for (const Segment &seg : image_.segments)
            if (seg.executable && addr >= seg.address && addr < seg.address + seg.size)
                return true;
        return false;
    };

    // Emit the whole reachable call graph, not just the requested functions, so
    // the result has no calls to bodies it never defined - otherwise a
    // decompiled program compiles but cannot link: every internal helper it
    // calls is an undefined symbol.
    //
    // Two passes. The first walks the graph and lets the decompiler name each
    // function; the second re-decompiles every one it found, so a call site
    // prints the name its target settled on rather than the placeholder it had
    // when the caller was first seen. Without the second pass a named callee
    // and its callers disagree, and the disagreement is another link failure.
    const size_t kFunctionLimit = 4000;
    std::string first_error;
    std::set<uint64_t> discovered;
    std::vector<uint64_t> worklist(addresses.begin(), addresses.end());
    while (!worklist.empty() && discovered.size() < kFunctionLimit) {
        uint64_t address = worklist.back();
        worklist.pop_back();
        if (!discovered.insert(address).second)
            continue;
        FunctionResult probe;
        std::string one_error;
        if (!decompile(address, name_for(address), probe, one_error)) {
            if (first_error.empty())
                first_error = one_error;
            discovered.erase(address);
            continue;
        }
        for (uint64_t callee : probe.callees)
            if (callee != 0 && !discovered.count(callee) && !imports.count(callee) && in_code(callee))
                worklist.push_back(callee);
    }
    if (discovered.empty()) {
        error = first_error.empty() ? "nothing could be decompiled" : first_error;
        return false;
    }
    std::vector<uint64_t> ordered(discovered.begin(), discovered.end());
    std::sort(ordered.begin(), ordered.end());
    std::vector<FunctionResult> results;
    for (uint64_t address : ordered) {
        FunctionResult result;
        std::string one_error;
        if (decompile(address, name_for(address), result, one_error))
            results.push_back(std::move(result));
    }
    if (results.empty()) {
        error = first_error.empty() ? "nothing could be decompiled" : first_error;
        return false;
    }
    CEmitOptions options;
    options.self_contained = self_contained;
    options.comments = comments;
    options.explain = explain;
    // Read the initial value of every data global the code refers to, so the
    // emitter can define it rather than leave an undefined extern.
    for (const FunctionResult &result : results) {
        for (const Declaration &decl : result.externals) {
            if (decl.is_function || decl.address == 0)
                continue;
            if (options.data_init.count(decl.address))
                continue;
            uint8_t bytes[8] = {0};
            size_t got = image_.read(decl.address, bytes, sizeof bytes);
            // A stack-frame pseudo-symbol the decompiler leaked is not real
            // memory, but it still has to be defined for the unit to link.
            bool artifact = decl.name.compare(0, 5, "stack") == 0;
            if (got == 0 && !artifact)
                continue; // genuinely unmapped: leave it as an extern
            uint64_t value = 0;
            for (int i = 7; i >= 0; --i)
                value = (value << 8) | bytes[i];
            options.data_init.emplace(decl.address, value);
        }
    }
    // The loader's name for each referenced data address, so an import slot for
    // a real libc global can be pointed at the true symbol.
    for (const Symbol &sym : image_.symbols)
        if (!sym.is_function && !sym.name.empty())
            options.data_names.emplace(sym.address, sym.name);
    out = emit_c_unit(results, options);
    // Define and re-point the absolute data addresses the code reads, so the
    // rebuilt program touches real arrays instead of stale image addresses.
    std::vector<std::pair<uint64_t, uint64_t>> code_ranges;
    for (const FunctionResult &r : results)
        code_ranges.emplace_back(r.address, r.address + (r.size == 0 ? 1 : r.size));
    emit_absolute_data(image_, code_ranges, out);
    tidy_names(out);
    return true;
}

std::vector<uint64_t> Session::function_addresses() const
{
    std::vector<uint64_t> addresses;
    // Imported stubs are named so calls read well, but their bodies belong to
    // another image and are not this program's code.
    for (const Symbol &symbol : image_.symbols) {
        if (!symbol.is_function || symbol.is_import)
            continue;
        // A name that is not a C identifier cannot be emitted as one, and a
        // symbol carrying such a name is not a function this program defines.
        bool usable = !symbol.name.empty();
        for (char c : symbol.name)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                usable = false;
        if (usable)
            addresses.push_back(symbol.address);
    }
    return addresses;
}

} // namespace astral_internal
