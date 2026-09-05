// Binds a loaded BinaryImage to Ghidra's decompiler core: a LoadImage over the
// image, a SleighArchitecture that uses it, and the analysis entry points.
#include "session.hh"
#include "format_finish.hh"
#include "macho_sign.hh"

#include <cstdint>
#include <cstring>
#include <sys/stat.h>

#include "assembler.hh"
#include "creal.hh"
#include "pseudo.hh"
#include "listing.hh"
#include "machine.hh"
#include "cxx_idioms.hh"
#include "langmap.hh"
#include "knowledge.hh"
#include "libc_protos.hh"
#include "naming.hh"
#include "paths.hh"

#include "architecture.hh"
#include "block.hh"
#include "funcdata.hh"
#include "libdecomp.hh"
#include "printastral.hh"
#include "printc.hh"
#include "sleigh_arch.hh"
#include "slgh_compile.hh"
#include "types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

namespace astral_internal {
namespace {

bool g_initialized = false;

// A name Astral chose has to be something a C compiler will take. Knowledge
// records are edited by hand and contributed by other people, so a name can
// arrive with a space or a punctuation mark in it; letting that reach the
// output turns a whole file into a syntax error.
bool is_c_identifier(const std::string &name)
{
    if (name.empty())
        return false;
    if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')
        return false;
    for (char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    return true;
}

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
                                         std::string &error, bool note_prototypes)
{
    if (!g_initialized) {
        error = "astral_init has not been called";
        return nullptr;
    }

    std::unique_ptr<Session> session(new Session());
    session->image_ = std::move(image);
    // Give C++ symbols readable names before anything else uses them.
    demangle_symbols(session->image_);
    // What a mangled name says about its own signature is as good as a
    // prototype read from source; make it known before prototypes are applied.
    // Recorded once per image: a second engine over the same program would be
    // writing what the first is already reading.
    if (!note_prototypes)
        ; // the caller has already recorded them
    else
        for (const Symbol &sym : session->image_.symbols)
            if (sym.is_function && !sym.prototype.empty())
                Knowledge::instance().note_prototype(sym.name, sym.prototype);

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
    // A C++ object another image defines (std::cout) is reached through a
    // pointer slot; naming the slot after the object is what makes a call read
    // stdOperatorShl(stdCout, ...) rather than a bare address.
    try {
        ghidra::Scope *global = session->arch_->symboltab->getGlobalScope();
        ghidra::TypeFactory *types = session->arch_->types;
        ghidra::AddrSpace *space = session->arch_->getDefaultCodeSpace();
        ghidra::Datatype *slot = types->getTypePointer(space->getAddrSize(), types->getTypeVoid(),
                                                       space->getWordSize());
        for (const Symbol &sym : session->image_.symbols) {
            if (sym.is_function || !sym.is_import || sym.linkage_name.empty())
                continue;
            ghidra::Address addr(space, sym.address);
            // An empty use point: the symbol is tied to its address everywhere,
            // not scoped from a first use the way a register local is.
            global->addSymbol(sym.name, slot, addr, ghidra::Address());
        }
    } catch (ghidra::LowlevelError &err) {
    }
    // A call to a function that never returns is the end of the flow. Without
    // this the decoder walks off the end of the caller into whatever the
    // linker put next, which merges two unrelated functions into one.
    try {
        ghidra::Scope *global = session->arch_->symboltab->getGlobalScope();
        const Knowledge &knowledge = Knowledge::instance();
        for (const std::string &name : knowledge.noreturn_names()) {
            ghidra::Funcdata *callee = global->queryFunction(name);
            if (callee != nullptr)
                callee->getFuncProto().setNoReturn(true);
        }
    } catch (ghidra::LowlevelError &err) {
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
                             addr, ghidra::Address());
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

emulator::RunResult Session::run(const emulator::RunOptions &options)
{
    return emulator::run(arch_, image_, options);
}

std::unique_ptr<emulator::Debugger> Session::debug(const emulator::RunOptions &options,
                                                   std::string &error)
{
    return emulator::Debugger::create(arch_, image_, options, error);
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

bool Session::disassemble_readable(uint64_t address, int count, std::string &out,
                                   std::string &error)
{
    std::string raw;
    if (!disassemble(address, count, raw, error))
        return false;
    // The last instruction's address bounds what counts as a local branch: a
    // target beyond it belongs to some other function, and is named rather than
    // labelled.
    uint64_t last = address;
    {
        std::istringstream in(raw);
        std::string line;
        while (std::getline(in, line)) {
            const size_t colon = line.find(": ");
            if (colon != std::string::npos)
                last = std::strtoull(line.substr(0, colon).c_str(), nullptr, 16);
        }
    }
    out = readable_listing_text(raw, image_, address, last);
    return true;
}

std::string Session::readable_trace(const std::string &raw)
{
    // The listing needs to know which addresses are inside what it is showing,
    // so a branch within the trace reads as a target rather than a stranger.
    uint64_t first = UINT64_MAX;
    uint64_t last = 0;
    {
        std::istringstream in(raw);
        std::string line;
        while (std::getline(in, line)) {
            const size_t colon = line.find(": ");
            if (colon == std::string::npos)
                continue;
            const uint64_t at = std::strtoull(line.substr(0, colon).c_str(), nullptr, 16);
            first = std::min(first, at);
            last = std::max(last, at);
        }
    }
    if (first == UINT64_MAX)
        return std::string();

    // Both bounds are the same address so nothing is given a label. In a
    // listing a label says where a branch comes back to; in a trace the answer
    // is always the next line, so a label would name what is already there.
    (void)last;

    std::ostringstream out;
    std::string previous;
    for (const ListingLine &line : readable_listing(raw, image_, first, first)) {
        if (line.is_label)
            continue;
        // A call is answered at the stub it branched to, which is a second line
        // saying the name the branch above it already said. Worth keeping when
        // the branch was indirect and said no name at all; noise otherwise.
        if (line.text.compare(0, 5, "call ") == 0 && previous.size() >= line.text.size() - 5 &&
            previous.compare(previous.size() - (line.text.size() - 5), std::string::npos,
                             line.text.substr(5)) == 0)
            continue;
        previous = line.text;
        char address[24];
        std::snprintf(address, sizeof address, "0x%012llx",
                      static_cast<unsigned long long>(line.address));
        out << "  " << address << "  " << line.text;
        if (!line.comment.empty())
            out << "    ; " << line.comment;
        out << '\n';
    }
    return out.str();
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

bool Session::patch_assembly(uint64_t address, const std::string &text, std::string &error)
{
    namespace asmw = astral_internal::assembler;
    const asmw::Target target = asmw::target_for_language(language_id());
    if (target == asmw::Target::Unknown) {
        error = "Astral reads " + language_id() + " but cannot write it yet";
        return false;
    }
    const asmw::Result written = asmw::assemble(target, text, address);
    if (!written.ok) {
        error = written.error;
        return false;
    }
    // The replacement has to fit exactly where the old instruction was. Writing
    // something shorter would leave the tail of the old one behind, and
    // something longer would run into whatever follows.
    const int room = instruction_length(address);
    if (room <= 0) {
        error = "there is no instruction at that address to replace";
        return false;
    }
    std::vector<uint8_t> bytes = written.bytes;
    if (bytes.size() > static_cast<size_t>(room)) {
        // Room can be made by taking in the instructions that follow, but only
        // deliberately: silently overwriting the next one is how a patch turns
        // into a crash somewhere else entirely.
        int covered = room;
        int instructions = 1;
        while (covered < static_cast<int>(bytes.size())) {
            const int next = instruction_length(address + static_cast<uint64_t>(covered));
            if (next <= 0)
                break;
            covered += next;
            ++instructions;
        }
        char message[240];
        if (covered >= static_cast<int>(bytes.size()))
            std::snprintf(message, sizeof message,
                          "that assembles to %zu bytes but only %d are free before the next "
                          "instruction; it would take %d instructions to make room",
                          bytes.size(), room, instructions);
        else
            std::snprintf(message, sizeof message,
                          "that assembles to %zu bytes but the instruction there is only %d",
                          bytes.size(), room);
        error = message;
        return false;
    }
    if (bytes.size() < static_cast<size_t>(room)) {
        if (asmw::is_fixed_width(target)) {
            char message[200];
            std::snprintf(message, sizeof message,
                          "that assembles to %zu bytes but the instruction there is %d, and every "
                          "instruction on this architecture is the same size",
                          bytes.size(), room);
            error = message;
            return false;
        }
        // A shorter instruction leaves a gap. Filling it with no-ops keeps
        // everything after it where it was, which is what the rest of the
        // program expects.
        const asmw::Result padding = asmw::assemble(target, "nop", address + bytes.size());
        if (!padding.ok || padding.bytes.size() != 1) {
            error = "the replacement is shorter and there is no single-byte no-op to fill the gap";
            return false;
        }
        while (bytes.size() < static_cast<size_t>(room))
            bytes.push_back(padding.bytes.front());
    }
    return patch_bytes(address, bytes, PatchTier::Assembled, text, error);
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
    // Changing bytes inside an executable breaks whatever the format records
    // about them: a Mach-O signature no longer covers the file, a PE checksum
    // no longer adds up. An ELF records nothing and needs nothing.
    bool sign_on_disk = false;
    {
        std::string note;
        std::string finish_error;
        if (!finish_patched_file(bytes, note, finish_error) && is_macho(bytes))
            sign_on_disk = true; // the native signer declined; try codesign after writing
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

std::vector<std::pair<ghidra::Address, ghidra::FuncProto *>>
Session::collect_vararg_overrides(void *funcdata)
{
    ghidra::Funcdata *fd = static_cast<ghidra::Funcdata *>(funcdata);
    // `fixed` gives one letter per fixed argument: 'i' an int, 'p' a pointer,
    // 's' the format string itself. scanf is left out because its arguments
    // are pointers to write through, which is a different recovery.
    struct Formatter { const char *fixed; int format_index; bool void_ret; };
    static const std::map<std::string, Formatter> kFormatters = {
        {"printf", {"s", 0, false}},    {"fprintf", {"ps", 1, false}},
        {"sprintf", {"ps", 1, false}},  {"snprintf", {"pps", 2, false}},
        {"dprintf", {"ps", 1, false}},
        // The err/warn family writes the message and returns nothing. Their
        // arguments are recovered the same way, which is what makes a
        // diagnostic print the file it is about.
        {"warn", {"s", 0, true}},       {"warnx", {"s", 0, true}},
        {"err", {"is", 1, true}},       {"errx", {"is", 1, true}},
    };
    const int ptr = arch_->getDefaultCodeSpace()->getAddrSize();
    const int word = arch_->getDefaultCodeSpace()->getWordSize();
    ghidra::TypeFactory *tf = arch_->types;
    ghidra::Datatype *tInt = tf->getBase(4, ghidra::TYPE_INT);
    ghidra::Datatype *tLong = tf->getBase(8, ghidra::TYPE_INT);
    ghidra::Datatype *tUint = tf->getBase(4, ghidra::TYPE_UINT);
    ghidra::Datatype *tDouble = tf->getBase(8, ghidra::TYPE_FLOAT);
    ghidra::Datatype *tCharPtr = tf->getTypePointer(ptr, tf->getTypeChar(1), word);
    ghidra::Datatype *tVoid = tf->getTypeVoid();
    ghidra::Datatype *tVoidPtr = tf->getTypePointer(ptr, tVoid, word);

    std::vector<std::pair<ghidra::Address, ghidra::FuncProto *>> overrides;
    for (int ci = 0; ci < fd->numCalls(); ++ci) {
        ghidra::FuncCallSpecs *spec = fd->getCallSpecs(ci);
        if (spec == nullptr)
            continue;
        ghidra::PcodeOp *op = spec->getOp();
        if (op == nullptr)
            continue;
        auto entry = kFormatters.find(spec->getName());
        if (entry == kFormatters.end())
            continue;
        const std::string fixed_spec = entry->second.fixed;
        const int fixed = static_cast<int>(fixed_spec.size());
        const int format_index = entry->second.format_index;
        const int format_slot = 1 + format_index; // getIn(0) is the call target
        if (op->numInput() <= format_slot)
            continue;
        const ghidra::Varnode *fmtvn = op->getIn(format_slot);
        if (!fmtvn->isConstant())
            continue;
        uint64_t fmtaddr = fmtvn->getOffset();
        std::string fmt;
        for (int k = 0; k < 4096; ++k) {
            uint8_t c = 0;
            if (image_.read(fmtaddr + k, &c, 1) == 0 || c == 0)
                break;
            fmt.push_back(static_cast<char>(c));
        }
        if (fmt.empty())
            continue;

        std::vector<ghidra::Datatype *> varargs;
        for (size_t k = 0; k + 1 < fmt.size(); ++k) {
            if (fmt[k] != '%')
                continue;
            if (fmt[k + 1] == '%') { ++k; continue; }
            size_t j = k + 1;
            bool wide = false;
            while (j < fmt.size() && std::strchr("-+ #0123456789.*hljztL", fmt[j])) {
                if (std::strchr("lLjzt", fmt[j])) wide = true;
                ++j;
            }
            if (j >= fmt.size())
                break;
            switch (fmt[j]) {
            case 'd': case 'i': varargs.push_back(wide ? tLong : tInt); break;
            case 'u': case 'x': case 'X': case 'o': varargs.push_back(wide ? tLong : tUint); break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
                varargs.push_back(tDouble); break;
            case 'c': varargs.push_back(tInt); break;
            case 's': varargs.push_back(tCharPtr); break;
            case 'p': case 'n': varargs.push_back(tVoidPtr); break;
            default: break;
            }
            k = j;
        }
        if (varargs.empty())
            continue;

        ghidra::PrototypePieces pieces;
        pieces.model = arch_->defaultfp;
        pieces.name = spec->getName();
        pieces.outtype = entry->second.void_ret ? tVoid : tInt;
        for (int a = 0; a < fixed; ++a) {
            ghidra::Datatype *t = tVoidPtr;
            if (a == format_index)
                t = tCharPtr;
            else if (fixed_spec[a] == 'i')
                t = tInt;
            pieces.intypes.push_back(t);
            pieces.innames.emplace_back();
        }
        for (ghidra::Datatype *t : varargs) {
            pieces.intypes.push_back(t);
            pieces.innames.emplace_back();
        }
        pieces.firstVarArgSlot = fixed; // args past the fixed ones are variadic
        try {
            // Exactly as Ghidra's own prototype override: internal storage, no
            // input lock, so the surrounding register analysis is not disturbed.
            ghidra::FuncProto *proto = new ghidra::FuncProto();
            proto->setInternal(pieces.model, tVoid);
            (void)0;
            proto->setPieces(pieces);
            // The pieces are laid out as varargs so the model places the
            // arguments past the format on the stack, the way Apple's ABI does.
            // Then this is marked non-variadic, so parameter recovery treats
            // those stack slots as committed arguments and traces their values,
            // rather than skipping straight to the model and dropping them.
            proto->setDotdotdot(false);
            overrides.emplace_back(op->getAddr(), proto);
        } catch (ghidra::LowlevelError &) {
        }
    }
    return overrides;
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

        // main gets its real signature before flow analysis, so argc and argv
        // are recovered as parameters rather than left as raw input registers.
        if (fd->getName() == "main") {
            try {
                ghidra::TypeFactory *tf = arch_->types;
                int psz = arch_->getDefaultCodeSpace()->getAddrSize();
                int wsz = arch_->getDefaultCodeSpace()->getWordSize();
                ghidra::Datatype *tInt = tf->getBase(4, ghidra::TYPE_INT);
                ghidra::Datatype *cptr = tf->getTypePointer(psz, tf->getTypeChar(1), wsz);
                ghidra::Datatype *ccptr = tf->getTypePointer(psz, cptr, wsz);
                ghidra::PrototypePieces pieces;
                pieces.model = arch_->defaultfp;
                pieces.name = "main";
                pieces.outtype = tInt;
                pieces.intypes.push_back(tInt);  pieces.innames.push_back("argc");
                pieces.intypes.push_back(ccptr); pieces.innames.push_back("argv");
                pieces.firstVarArgSlot = -1;
                fd->getFuncProto().setPieces(pieces);
            } catch (ghidra::LowlevelError &) {
            }
        }

        ghidra::AddrSpace *space = addr.getSpace();
        fd->followFlow(ghidra::Address(space, 0), ghidra::Address(space, space->getHighest()));

        arch_->allacts.getCurrent()->reset(*fd);
        arch_->allacts.getCurrent()->perform(*fd);

        // Recover the arguments after a printf-style format string. Overrides
        // are inserted and the function re-analysed with fd->clear(), which
        // keeps overrides but resets analysis - the way the engine's own
        // override does it, so nothing else in the function is disturbed.
        auto vararg_overrides = collect_vararg_overrides(fd);
        if (!vararg_overrides.empty()) {
            for (const auto &ov : vararg_overrides)
                fd->getOverride().insertProtoOverride(ov.first, ov.second);
            fd->clear();
            fd->followFlow(ghidra::Address(space, 0), ghidra::Address(space, space->getHighest()));
            arch_->allacts.getCurrent()->reset(*fd);
            arch_->allacts.getCurrent()->perform(*fd);
        }

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
            std::string chosen = apply_naming(fd, out);
            if (!chosen.empty() && !is_c_identifier(chosen))
                chosen.clear(); // a name that will not compile is no name
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


// Prints one function twice: once with the c-language printer, which is what
// the compilable path is built from, and once with the readable one. Both come
// from the same decompiled form, so the two never disagree about the code.
void Session::print_function(void *funcdata, std::string &listing, std::string &readable)
{
    ghidra::Funcdata *fd = static_cast<ghidra::Funcdata *>(funcdata);
    std::ostringstream code;
    arch_->print->setOutputStream(&code);
    arch_->print->docFunction(fd);
    listing = code.str();

    if (!want_readable_) {
        readable.clear();
        return;
    }

    ghidra::registerAstralPrintLanguage();
    const std::string chosen = arch_->print->getName();
    std::ostringstream pretty;
    try {
        arch_->setPrintLanguage("astral-c");
        arch_->print->setOutputStream(&pretty);
        arch_->print->docFunction(fd);
        readable = readable_listing(pretty.str());
    } catch (ghidra::LowlevelError &) {
        // Nothing readable came out, so the plain listing stands. The listing
        // the rest of the library uses was already taken.
        readable = listing;
    }
    arch_->setPrintLanguage(chosen);
}

// Reads everything Astral reports about a function out of the decompiled form.
void Session::analyse_function(void *funcdata, FunctionResult &out)
{
    ghidra::Funcdata *fd = static_cast<ghidra::Funcdata *>(funcdata);
    ghidra::Scope *global = arch_->symboltab->getGlobalScope();
    out = FunctionResult();
    print_function(funcdata, out.c_code, out.c_code_pseudo);

    out.name = fd->getName();
    out.address = fd->getAddress().getOffset();
    out.size = static_cast<uint64_t>(fd->getSize());

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

    // The program's entry point is main, and that is not inferred. Every other
    // name here comes from what a function calls or says, and main calls
    // everything a program does, so whichever idiom happens to be looked at
    // first finds something to claim it with. Naming it from evidence is how a
    // listing ends up with a main called after the least important thing in it.
    if (knowledge.is_placeholder(out.name) &&
        std::find(image_.entry_points.begin(), image_.entry_points.end(), out.address) !=
            image_.entry_points.end()) {
        naming.function_name = "main";
        naming.function_reason = "it is the program's entry point";
    }

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
        print_function(funcdata, out.c_code, out.c_code_pseudo);
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
// Marks the bytes of `text` that sit inside a string or character literal.
// What is written there is the program's own data, not an identifier.
std::vector<bool> literal_mask(const std::string &text)
{
    std::vector<bool> mask(text.size(), false);
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != 0) {
            mask[i] = true;
            if (c == '\\' && i + 1 < text.size()) {
                mask[i + 1] = true;
                ++i;
                continue;
            }
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            mask[i] = true;
        }
    }
    return mask;
}

// Replaces every occurrence of a whole identifier, leaving literals alone.
void rename_token(std::string &text, const std::string &from, const std::string &to,
                  std::vector<bool> &mask)
{
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at)) {
        const size_t end = at + from.size();
        const bool joined_left =
            at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_');
        const bool joined_right =
            end < text.size() &&
            (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_');
        if (joined_left || joined_right || (at < mask.size() && mask[at])) {
            at = end;
            continue;
        }
        text.replace(at, from.size(), to);
        mask.erase(mask.begin() + static_cast<long>(at),
                   mask.begin() + static_cast<long>(end));
        mask.insert(mask.begin() + static_cast<long>(at), to.size(), false);
        at += to.size();
    }
}

void tidy_names(std::string &out)
{
    // First put the engine's spellings into one shape.
    struct Rule { std::regex re; std::string rep; };
    static const std::vector<Rule> rules = {
        {std::regex(R"(\b[A-Za-z]{1,4}Ram0*([0-9A-Fa-f]+)\b)"), "g$1"},
        {std::regex(R"(\bfunc_0x0*([0-9A-Fa-f]+)\b)"), "sub$1"},
        {std::regex(R"(\b(?:code_r|joined_r)0x0*([0-9A-Fa-f]+)\b)"), "loc$1"},
        {std::regex(R"(\bDAT_0*([0-9A-Fa-f]+)\b)"), "dat$1"},
    };
    for (const Rule &r : rules)
        out = std::regex_replace(out, r.re, r.rep);

    // Then take the addresses out of the names. An address in an identifier is
    // a fact about where something sat in one file, not about what it is, and
    // reading half hex and half C is nobody's idea of source.
    struct Family { const char *prefix; const char *word; };
    static const Family families[] = {
        {"g", "global"}, {"dat", "table"}, {"sub", "sub"}, {"loc", "label"},
    };
    const Knowledge &knowledge = Knowledge::instance();
    std::vector<bool> mask = literal_mask(out);
    std::set<std::string> taken;
    {
        static const std::regex word(R"(\b[A-Za-z_][A-Za-z0-9_]*\b)");
        for (auto it = std::sregex_iterator(out.begin(), out.end(), word);
             it != std::sregex_iterator(); ++it)
            taken.insert(it->str());
    }

    for (const Family &family : families) {
        // In the order they first appear, so the numbering follows the reading.
        const std::string pattern =
            std::string("\\b") + family.prefix + R"([0-9a-f]{3,}\b)";
        std::regex finder(pattern);
        std::vector<std::string> names;
        std::set<std::string> seen;
        for (auto it = std::sregex_iterator(out.begin(), out.end(), finder);
             it != std::sregex_iterator(); ++it)
            if (seen.insert(it->str()).second)
                names.push_back(it->str());

        int next = 1;
        for (const std::string &name : names) {
            std::string chosen;
            // A global that only ever holds the result of one call is named
            // after what that call produces.
            if (std::string(family.prefix) == "g") {
                std::regex assigned("\\b" + name + R"( = ([A-Za-z_][A-Za-z0-9_]*)\()");
                std::set<std::string> producers;
                for (auto it = std::sregex_iterator(out.begin(), out.end(), assigned);
                     it != std::sregex_iterator(); ++it)
                    producers.insert((*it)[1].str());
                if (producers.size() == 1) {
                    const std::string &call = *producers.begin();
                    std::string noun = knowledge.noun_for_call(call);
                    if (noun.empty()) {
                        // The call's own name, in camelCase like every other
                        // name Astral makes: compat_mode gives compatMode.
                        bool boundary = false;
                        for (char c : call) {
                            if (c == '_') { boundary = true; continue; }
                            if (noun.empty())
                                noun.push_back(static_cast<char>(
                                    std::tolower(static_cast<unsigned char>(c))));
                            else if (boundary)
                                noun.push_back(static_cast<char>(
                                    std::toupper(static_cast<unsigned char>(c))));
                            else
                                noun.push_back(c);
                            boundary = false;
                        }
                    }
                    if (!noun.empty() && taken.count(noun) == 0) {
                        chosen = noun;
                    } else if (!noun.empty()) {
                        for (int n = 2; n < 1000; ++n) {
                            const std::string candidate = noun + std::to_string(n);
                            if (taken.count(candidate) == 0) {
                                chosen = candidate;
                                break;
                            }
                        }
                    }
                }
            }
            while (chosen.empty()) {
                const std::string candidate = std::string(family.word) + std::to_string(next++);
                if (taken.count(candidate) == 0)
                    chosen = candidate;
            }
            taken.insert(chosen);
            rename_token(out, name, chosen, mask);
        }
    }
}

// Whether the bytes at the start form a printable, NUL-terminated C string.
bool looks_like_string(const std::vector<uint8_t> &b, size_t &length_out)
{
    size_t i = 0;
    for (; i < b.size(); ++i) {
        uint8_t c = b[i];
        if (c == 0)
            break;
        if (c != '\n' && c != '\t' && c != '\r' && (c < 0x20 || c > 0x7e))
            return false;
    }
    if (i == 0 || i >= b.size() || b[i] != 0)
        return false; // empty, or no terminator within the window
    // A lone terminator is deliberately not called the empty string. It is far
    // more often the front of a table the program fills in later, and calling
    // that a string constant makes every write through it undefined.
    length_out = i;
    return true;
}

// A readable name for a string constant, from its own text: "arg1" -> s_arg1,
// "Crackme EZ\n" -> s_crackme_ez.
std::string string_slug(const std::vector<uint8_t> &b, size_t len)
{
    std::string slug = "s";
    bool boundary = true; // first alnum after the 's' is capitalised
    for (size_t i = 0; i < len && slug.size() < 32; ++i) {
        char c = static_cast<char>(b[i]);
        if (std::isalnum(static_cast<unsigned char>(c))) {
            slug.push_back(boundary ? static_cast<char>(std::toupper((unsigned char)c))
                                    : static_cast<char>(std::tolower((unsigned char)c)));
            boundary = false;
        } else {
            boundary = true;
        }
    }
    return slug.size() > 1 ? slug : std::string();
}

// The C source form of a string literal, escaped.
std::string c_string_literal(const std::vector<uint8_t> &b, size_t len)
{
    std::string out = "\"";
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = b[i];
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        default: out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('\"');
    return out;
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
    auto segment_limit = [&](uint64_t a) {
        for (const Segment &seg : image.segments)
            if (a >= seg.address && a < seg.address + seg.size)
                return seg.address + seg.size;
        return a + 1;
    };
    auto reads_as_string = [&](uint64_t a) {
        const uint64_t peek = std::min<uint64_t>(segment_limit(a) - a, 4096);
        if (peek == 0)
            return false;
        std::vector<uint8_t> bytes(peek, 0);
        image.read(a, bytes.data(), bytes.size());
        size_t length = 0;
        return looks_like_string(bytes, length);
    };
    // Collect the distinct address literals that land in mapped data.
    std::set<uint64_t> addrs;
    static const std::regex hex(R"(\b0x[0-9A-Fa-f]{5,16}\b)");
    for (auto it = std::sregex_iterator(out.begin(), out.end(), hex);
         it != std::sregex_iterator(); ++it) {
        uint64_t a = std::strtoull(it->str().c_str() + 2, nullptr, 16);
        if (!image.contains(a))
            continue;
        // A function's recovered extent can run far past where its instructions
        // actually stop, and on a Mach-O the strings sit in the same segment as
        // the code. So an address whose bytes read as a string is taken for
        // data even when a function claims to cover it: the alternative is
        // leaving every string in that range as a bare number.
        if (in_code(a) && !reads_as_string(a))
            continue;
        addrs.insert(a);
    }
    if (addrs.empty())
        return;

    std::vector<uint64_t> sorted(addrs.begin(), addrs.end());
    std::sort(sorted.begin(), sorted.end());

    // How far past a referenced address to keep, when nothing else says where
    // the data ends. The decompiler often reaches one table through several
    // bases a few bytes apart, so the reach has to cover more than the first
    // byte.
    const uint64_t kWindow = 8192;
    auto segment_end = [&](uint64_t a) {
        for (const Segment &s : image.segments)
            if (a >= s.address && a < s.address + s.size)
                return s.address + s.size;
        return a + 1;
    };

    std::map<std::string, int> slug_counts;
    std::map<std::string, std::string> string_names; // text -> the global holding it
    std::ostringstream defs;
    // Where each referenced address ends up: the array that covers it, and how
    // far into that array it sits.
    std::map<uint64_t, std::pair<std::string, uint64_t>> placement;

    // Every address that is really a C string becomes a named literal, so the
    // source reads setlocale(0, "") rather than an offset into a wall of bytes.
    // This is decided for each address on its own: a string sitting a few bytes
    // after another address is still a string.
    std::vector<uint64_t> remaining;
    for (uint64_t address : sorted) {
        const uint64_t limit = segment_end(address);
        const uint64_t peek = std::min(limit - address, kWindow);
        std::vector<uint8_t> bytes(peek == 0 ? 1 : peek, 0);
        image.read(address, bytes.data(), bytes.size());
        size_t slen = 0;
        std::string slug;
        if (looks_like_string(bytes, slen))
            slug = string_slug(bytes, slen);
        if (slug.empty()) {
            remaining.push_back(address);
            continue;
        }
        // The same text referred to from two places is one constant, not two.
        const std::string text = c_string_literal(bytes, slen);
        auto shared = string_names.find(text);
        std::string sname;
        if (shared != string_names.end()) {
            sname = shared->second;
        } else {
            int &n = slug_counts[slug];
            sname = n == 0 ? slug : slug + std::to_string(n + 1);
            ++n;
            string_names.emplace(text, sname);
            defs << "static const char " << sname << "[] = " << text << ";\n";
        }
        placement.emplace(address, std::make_pair(sname, 0));
    }

    // What is left is data. An address that another address's window already
    // reaches shares that array rather than getting a window of its own, or the
    // same bytes come out once for every base the code happens to use.
    for (size_t i = 0; i < remaining.size();) {
        const uint64_t base = remaining[i];
        const uint64_t limit = segment_end(base);
        uint64_t end = std::min(base + kWindow, limit);
        size_t j = i;
        while (j < remaining.size() && remaining[j] < end) {
            const uint64_t reach = std::min(remaining[j] + kWindow, segment_end(remaining[j]));
            if (reach > end && segment_end(remaining[j]) == limit)
                end = reach;
            ++j;
        }
        if (end <= base)
            end = base + 1;

        char name[40];
        std::snprintf(name, sizeof name, "dat%llx", static_cast<unsigned long long>(base));
        const size_t size = static_cast<size_t>(end - base);
        std::vector<uint8_t> bytes(size, 0);
        image.read(base, bytes.data(), bytes.size());
        defs << "static unsigned char " << name << "[" << size << "] = {";
        for (size_t b = 0; b < bytes.size(); ++b) {
            if (b % 16 == 0)
                defs << "\n  ";
            char byte[8];
            std::snprintf(byte, sizeof byte, "0x%02x,", bytes[b]);
            defs << byte;
        }
        defs << "\n};\n";
        for (size_t k = i; k < j; ++k)
            placement.emplace(remaining[k], std::make_pair(std::string(name), remaining[k] - base));
        i = j;
    }

    // Point every address literal at where its bytes actually live.
    for (const auto &entry : placement) {
        const uint64_t address = entry.first;
        const std::string &name = entry.second.first;
        const uint64_t offset = entry.second.second;
        std::string to = name;
        if (offset != 0) {
            char shifted[80];
            std::snprintf(shifted, sizeof shifted, "(%s + %llu)", name.c_str(),
                          static_cast<unsigned long long>(offset));
            to = shifted;
        }
        // Rewrite every use of the literal to the array (it decays to a pointer).
        char literal[24];
        std::snprintf(literal, sizeof literal, "0x%llx", static_cast<unsigned long long>(address));
        std::string from = literal;
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


// The name a function should be decompiled under: an entry point is main.
std::string Session::current_name(uint64_t address) const
{
    try {
        ghidra::Address addr(arch_->getDefaultCodeSpace(), address);
        ghidra::Funcdata *fd = arch_->symboltab->getGlobalScope()->queryFunction(addr);
        return fd == nullptr ? std::string() : fd->getName();
    } catch (ghidra::LowlevelError &) {
        return std::string();
    }
}

std::string Session::name_for_entry(uint64_t address) const
{
    for (uint64_t entry : image_.entry_points)
        if (entry == address)
            return "main";
    return std::string();
}


std::unique_ptr<Session> Session::clone(std::string &error) const
{
    // The image is copied rather than shared: each engine writes its own
    // symbol table and patches its own copy of the bytes.
    std::unique_ptr<Session> other = Session::create(image_, archid_, error, false);
    if (other)
        other->auto_naming_ = auto_naming_;
    return other;
}

int Session::worker_count(size_t work) const
{
    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0)
        cores = 1;
    int want = threads_ > 0 ? threads_ : static_cast<int>(cores);
    // A way to pin the count without threading an option through every caller,
    // which is what makes measuring one against the other possible.
    if (const char *pinned = std::getenv("ASTRAL_THREADS")) {
        const int asked = std::atoi(pinned);
        if (asked > 0)
            want = asked;
    }
    if (want < 1)
        want = 1;
    // Another engine is only worth having when there is real work for it. Each
    // one re-derives what its own share calls, so on a small program spreading
    // the work costs more than doing it in one place; measuring says the turn
    // comes somewhere in the low hundreds of functions.
    const size_t kMinimumToSpread = 128;
    const size_t kEachThreadWants = 64;
    if (work < kMinimumToSpread)
        return 1;
    const int useful = static_cast<int>(work / kEachThreadWants);
    if (want > useful)
        want = useful;
    if (want < 1)
        want = 1;
    return want;
}


// A set of engines kept alive for the whole job.
//
// Cloning an engine is cheap next to decompiling one function, but not next to
// decompiling a handful, and the call graph is walked in many small layers. So
// the engines are built once, on their own threads, and fed batch after batch.
class Session::Pool {
public:
    Pool(Session &primary, int workers) : primary_(primary)
    {
        if (workers <= 1)
            return;
        ready_.store(0);
        for (int i = 0; i < workers - 1; ++i)
            threads_.emplace_back([this] { serve(); });
        // Wait for each helper to report whether it has an engine, so the first
        // batch is not handed to a pool that is still assembling itself.
        std::unique_lock<std::mutex> guard(lock_);
        started_.wait(guard, [this] {
            return ready_.load() == static_cast<int>(threads_.size());
        });
    }

    ~Pool()
    {
        {
            std::lock_guard<std::mutex> guard(lock_);
            closing_ = true;
        }
        work_ready_.notify_all();
        for (std::thread &thread : threads_)
            thread.join();
    }

    // Decompiles every address in `work`. `apply_names` is adopted by every
    // engine first, when given.
    void run(const std::vector<uint64_t> &work,
             const std::map<uint64_t, std::string> *apply_names,
             std::map<uint64_t, FunctionResult> &results,
             std::map<uint64_t, std::string> &names, std::set<uint64_t> *found,
             std::string &first_error)
    {
        {
            std::lock_guard<std::mutex> guard(lock_);
            work_ = &work;
            names_ = apply_names;
            results_ = &results;
            out_names_ = &names;
            found_ = found;
            error_ = &first_error;
            next_.store(0);
            outstanding_ = static_cast<int>(threads_.size());
            generation_ += 1;
        }
        work_ready_.notify_all();
        adopt(primary_);
        consume(primary_);
        std::unique_lock<std::mutex> guard(lock_);
        done_.wait(guard, [this] { return outstanding_ == 0; });
        work_ = nullptr;
    }

private:
    void serve()
    {
        std::string clone_error;
        // The engine is built here so its translator belongs to this thread.
        std::unique_ptr<Session> engine = primary_.clone(clone_error);
        {
            std::lock_guard<std::mutex> guard(lock_);
            ready_.fetch_add(1);
        }
        started_.notify_all();
        uint64_t seen = 0;
        for (;;) {
            std::unique_lock<std::mutex> guard(lock_);
            work_ready_.wait(guard, [&] { return closing_ || generation_ != seen; });
            if (closing_)
                return;
            seen = generation_;
            guard.unlock();
            if (engine) {
                adopt(*engine);
                consume(*engine);
            }
            guard.lock();
            if (--outstanding_ == 0) {
                guard.unlock();
                done_.notify_all();
            }
        }
    }

    void adopt(Session &engine)
    {
        if (names_ == nullptr)
            return;
        for (const auto &named : *names_) {
            if (named.second.empty())
                continue;
            // Renaming rebuilds the function and every call site that names it,
            // so it is only worth doing where the engine disagrees.
            if (engine.current_name(named.first) == named.second)
                continue;
            std::string ignored;
            engine.rename(named.first, named.second, false, ignored);
        }
    }

    void consume(Session &engine)
    {
        for (;;) {
            const size_t index = next_.fetch_add(1);
            if (work_ == nullptr || index >= work_->size())
                return;
            const uint64_t address = (*work_)[index];
            FunctionResult result;
            std::string error;
            const bool ok = engine.decompile(address, engine.name_for_entry(address), result, error);
            std::lock_guard<std::mutex> guard(out_lock_);
            if (!ok) {
                if (error_->empty())
                    *error_ = error;
                continue;
            }
            if (found_ != nullptr)
                for (uint64_t callee : result.callees)
                    found_->insert(callee);
            (*out_names_)[address] = result.name;
            (*results_)[address] = std::move(result);
        }
    }

    Session &primary_;
    std::vector<std::thread> threads_;
    std::mutex lock_;
    std::mutex out_lock_;
    std::condition_variable work_ready_;
    std::condition_variable done_;
    std::condition_variable started_;
    std::atomic<int> ready_{0};
    std::atomic<size_t> next_{0};
    bool closing_ = false;
    uint64_t generation_ = 0;
    int outstanding_ = 0;
    const std::vector<uint64_t> *work_ = nullptr;
    const std::map<uint64_t, std::string> *names_ = nullptr;
    std::map<uint64_t, FunctionResult> *results_ = nullptr;
    std::map<uint64_t, std::string> *out_names_ = nullptr;
    std::set<uint64_t> *found_ = nullptr;
    std::string *error_ = nullptr;
};

bool Session::emit_c(const std::vector<uint64_t> &addresses, bool self_contained, bool comments,
                     bool explain, std::string &out, std::string &error)
{
    if (addresses.empty()) {
        error = "no functions to emit";
        return false;
    }
    // Emitting compilable C walks everything the named functions reach, and
    // none of it is ever read in its readable form. Producing that form for
    // each one doubles the printing for nothing, which is felt on a whole
    // program. It is restored however this returns.
    struct ReadableOff {
        bool *flag;
        bool was;
        ~ReadableOff() { *flag = was; }
    } readable_off{&want_readable_, want_readable_};
    want_readable_ = false;
    // Addresses that name a library import, so a call to one is left as an
    // external declaration rather than decompiled into a stub body.
    std::set<uint64_t> imports;
    for (const Symbol &sym : image_.symbols)
        if (sym.is_import)
            imports.insert(sym.address);
    std::set<uint64_t> entries(image_.entry_points.begin(), image_.entry_points.end());
    auto name_for = [&](uint64_t addr) { return entries.count(addr) ? std::string("main")
                                                                    : std::string(); };
    // A call into the C++ standard library or the language runtime is a library
    // call, not the program's own code. It is left as an external declaration
    // rather than decompiled, the way a call to printf is - otherwise a small
    // program drags in thousands of lines of std::string and iostream.
    std::map<uint64_t, std::string> sym_names;
    for (const Symbol &sym : image_.symbols)
        if (!sym.name.empty())
            sym_names.emplace(sym.address, sym.name);
    auto is_library = [&](uint64_t addr) {
        auto it = sym_names.find(addr);
        if (it == sym_names.end())
            return false;
        const std::string &n = it->second;
        return n.rfind("std", 0) == 0 || n.rfind("__", 0) == 0 ||
               n.rfind("operator", 0) == 0 || n.rfind("_GLOBAL", 0) == 0 ||
               n.find("cxx") != std::string::npos;
    };
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
    auto wanted = [&](uint64_t callee) {
        return callee != 0 && !imports.count(callee) && in_code(callee) && !is_library(callee);
    };

    // The first pass walks the call graph a layer at a time. Everything in a
    // layer is independent, so the layer is decompiled across as many engines
    // as there are threads; what those functions call becomes the next layer.
    // The call graph is walked a layer at a time. Everything in a layer is
    // independent, so the layer goes out across the worker threads and what
    // those functions call becomes the next layer. Taking it in layers rather
    // than from one shared queue keeps the result the same on every run, and
    // costs less than spreading a handful of functions over every core: each
    // engine re-derives what its own slice calls, so more engines on the same
    // work means more of that repeated.
    // Where the time went, for anyone measuring the effect of a thread count.
    const bool timing = std::getenv("ASTRAL_TIMING") != nullptr;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto since = [&](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now() - t).count();
    };
    auto t0 = now();
    std::set<uint64_t> discovered;
    std::map<uint64_t, FunctionResult> found_results;
    std::map<uint64_t, std::string> settled_names;
    const int workers = worker_count(addresses.size());
    if (workers <= 1) {
        // One engine walks the graph depth first: a function is decompiled
        // right after the one that calls it, while everything that call needed
        // is still analysed. Taking it in layers instead throws that away.
        std::vector<uint64_t> worklist(addresses.begin(), addresses.end());
        while (!worklist.empty() && discovered.size() < kFunctionLimit) {
            uint64_t address = worklist.back();
            worklist.pop_back();
            if (!discovered.insert(address).second)
                continue;
            FunctionResult probe;
            std::string one_error;
            if (!decompile(address, name_for_entry(address), probe, one_error)) {
                if (first_error.empty())
                    first_error = one_error;
                discovered.erase(address);
                continue;
            }
            settled_names[address] = probe.name;
            found_results[address] = std::move(probe);
            for (uint64_t callee : found_results[address].callees)
                if (wanted(callee) && !discovered.count(callee))
                    worklist.push_back(callee);
        }
    }
    Pool pool(*this, workers);
    if (timing)
        std::fprintf(stderr, "pool ready %lldms\n", (long long)since(t0));
    if (workers > 1) {
        std::vector<uint64_t> layer;
        for (uint64_t address : addresses)
            if (discovered.insert(address).second)
                layer.push_back(address);
        while (!layer.empty() && discovered.size() < kFunctionLimit) {
            std::set<uint64_t> callees;
            pool.run(layer, nullptr, found_results, settled_names, &callees, first_error);
            layer.clear();
            for (uint64_t callee : callees)
                if (wanted(callee) && discovered.size() < kFunctionLimit &&
                    discovered.insert(callee).second)
                    layer.push_back(callee);
        }
    }
    if (timing)
        std::fprintf(stderr, "pass1 %lldms (%zu functions)\n", (long long)since(t0), discovered.size());
    auto t1 = now();
    for (auto it = discovered.begin(); it != discovered.end();) {
        if (found_results.count(*it) == 0)
            it = discovered.erase(it);
        else
            ++it;
    }
    if (discovered.empty()) {
        error = first_error.empty() ? "nothing could be decompiled" : first_error;
        return false;
    }

    // Every engine has to agree about what each function is called, or a call
    // site prints one name and the body it reaches carries another. The
    // helpers adopt these names as they start; this one adopts them here.
    for (const auto &named : settled_names) {
        if (named.second.empty())
            continue;
        std::string ignored;
        rename(named.first, named.second, false, ignored);
    }

    // The second pass decompiles everything again now that the names are
    // settled, so callers and callees agree.
    std::vector<uint64_t> ordered(discovered.begin(), discovered.end());
    std::sort(ordered.begin(), ordered.end());
    std::map<uint64_t, FunctionResult> final_results;
    std::map<uint64_t, std::string> final_names;
    pool.run(ordered, &settled_names, final_results, final_names, nullptr, first_error);
    if (timing)
        std::fprintf(stderr, "pass2 %lldms\n", (long long)since(t1));
    auto t2 = now();
    std::vector<FunctionResult> results;
    results.reserve(final_results.size());
    for (uint64_t address : ordered) {
        auto it = final_results.find(address);
        if (it != final_results.end())
            results.push_back(std::move(it->second));
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
    const std::string prefix = image_.format == ASTRAL_FORMAT_MACHO ? "_" : "";
    // The names this image imports as code. A data slot carrying one of these
    // names is the pointer the call goes through.
    std::set<std::string> imported_functions;
    for (const Symbol &sym : image_.symbols)
        if (sym.is_function && sym.is_import && !sym.name.empty())
            imported_functions.insert(sym.name);
    for (const Symbol &sym : image_.symbols) {
        if (sym.is_function) {
            if (!sym.linkage_name.empty())
                options.function_linkage.emplace(sym.name, prefix + sym.linkage_name);
            continue;
        }
        if (sym.name.empty())
            continue;
        options.data_names.emplace(sym.address, sym.name);
        if (sym.is_import && imported_functions.count(sym.name))
            options.data_functions.emplace(sym.address, sym.name);
        // Any other import slot holds the address of an object another image
        // defines. Its linker-level name reaches that object from C, so the
        // slot points at the real thing rather than at the file's fixup value.
        else if (sym.is_import && sym.linkage_name.empty())
            options.data_linkage.emplace(sym.address, prefix + sym.name);
        // The linker-level name of a C++ object, so a slot for it can point at
        // the real thing. Mach-O spells C symbols with a leading underscore.
        if (!sym.linkage_name.empty())
            options.data_linkage.emplace(sym.address, prefix + sym.linkage_name);
    }
    if (timing)
        std::fprintf(stderr, "merge %lldms\n", (long long)since(t2));
    auto t3 = now();
    out = emit_c_unit(results, options);
    if (timing)
        std::fprintf(stderr, "emit %lldms\n", (long long)since(t3));
    // Define and re-point the absolute data addresses the code reads, so the
    // rebuilt program touches real arrays instead of stale image addresses.
    std::vector<std::pair<uint64_t, uint64_t>> code_ranges;
    for (const FunctionResult &r : results)
        code_ranges.emplace_back(r.address, r.address + (r.size == 0 ? 1 : r.size));
    emit_absolute_data(image_, code_ranges, out);
    // C++ stream output written the way C writes it.
    rewrite_stream_idioms(out, image_);
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
