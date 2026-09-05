// The readable listing. See printastral.hh for what separates it from PrintC.
#include "printastral.hh"

#include "funcdata.hh"
#include "knowledge.hh"

#include <cctype>

namespace ghidra {

namespace {

// The frame is addressed through one of these when the decompiler did not
// recover it, and the register keeps the value the caller left in it. Naming
// them is the only way to tell a frame slot from a field reached through some
// other callee-saved register, which must keep printing as arithmetic.
const char *const FRAME_REGISTERS[] = {
  "x29", "w29", "fp", "sp", "rbp", "ebp", "bp", "rsp", "esp", "r11", "r31", "s8"
};

// What a width-named type is called in the listing. The type factory keeps the
// byte-width names, because the compilable path maps those exact spellings onto
// real C types; only the reader sees these.
struct TypeSpelling {
  const char *from;
  const char *to;
};

const TypeSpelling TYPE_SPELLINGS[] = {
  {"int1", "i8"},        {"int2", "i16"},       {"int4", "i32"},       {"int8", "i64"},
  {"int16", "i128"},
  {"uint1", "u8"},       {"uint2", "u16"},      {"uint4", "u32"},      {"uint8", "u64"},
  {"uint16", "u128"},
  {"byte", "u8"},        {"word", "u16"},       {"dword", "u32"},      {"qword", "u64"},
  {"sbyte", "i8"},       {"sword", "i16"},      {"sdword", "i32"},     {"sqword", "i64"},
  {"xunknown1", "unk8"}, {"xunknown2", "unk16"},{"xunknown4", "unk32"},{"xunknown8", "unk64"},
  {"xunknown3", "unk24"},{"xunknown5", "unk40"},{"xunknown6", "unk48"},{"xunknown7", "unk56"},
  {"undefined", "unk8"}, {"undefined1", "unk8"},{"undefined2", "unk16"},
  {"undefined3", "unk24"},{"undefined4", "unk32"},{"undefined5", "unk40"},
  {"undefined6", "unk48"},{"undefined7", "unk56"},{"undefined8", "unk64"},
  {"float4", "f32"},     {"float8", "f64"},     {"float10", "f80"},    {"float16", "f128"},
  {"wchar2", "wchar16"}, {"wchar4", "wchar32"}
};

// The listing spelling of a type name, or the name itself when it already says
// what it is (char, bool, void, code, and every type read from a program's own
// debug information).
string readableTypeName(const string &name)

{
  for(int4 i=0;i<(int4)(sizeof(TYPE_SPELLINGS)/sizeof(TypeSpelling));++i) {
    if (name == TYPE_SPELLINGS[i].from)
      return string(TYPE_SPELLINGS[i].to);
  }
  return name;
}

bool isHexDigits(const string &text,string::size_type from)

{
  if (from >= text.size()) return false;
  for(string::size_type i=from;i<text.size();++i) {
    if (isxdigit((unsigned char)text[i]) == 0) return false;
  }
  return true;
}

// An address written as hex, with the leading zeros and any 0x dropped.
string shortHex(const string &text)

{
  string::size_type start = 0;
  if (text.size() > 2 && text[0]=='0' && (text[1]=='x'||text[1]=='X'))
    start = 2;
  while (start + 1 < text.size() && text[start]=='0')
    ++start;
  return text.substr(start);
}

// A frame slot's name. The offset goes in the name so two slots never collide,
// and its sign says which side of the frame pointer it sits on: below is this
// function's own storage, above is what the caller left there.
string slotName(int8 off)

{
  ostringstream s;
  if (off < 0)
    s << "local" << hex << (uint8)(-off);
  else if (off > 0)
    s << "stack" << hex << (uint8)off;
  else
    s << "frame";
  return s.str();
}

// The listing name for a value the decompiler never worked out, or the empty
// string when the name already says something. The suffix marks these as the
// caller's business rather than this function's: they are read here and never
// written, so no declaration of them is ever true.
string entryValueName(const string &name)

{
  // unaffX29: the register the caller owned, untouched by this body.
  if (name.compare(0,5,"unaff") == 0 && name.size() > 5) {
    string tail = name.substr(5);
    tail[0] = (char)tolower((unsigned char)tail[0]);
    if (isHexDigits(tail,0))
      tail = "at" + shortHex(tail);
    return tail + "@entry";
  }
  // inStack00000010: storage the caller wrote that this body only reads.
  if (name.size() > 2 && name.compare(0,2,"in") == 0 && isupper((unsigned char)name[2])) {
    string tail = name.substr(2);
    tail[0] = (char)tolower((unsigned char)tail[0]);
    string::size_type digits = tail.find_first_of("0123456789");
    if (digits != string::npos && isHexDigits(tail,digits))
      tail = tail.substr(0,digits) + shortHex(tail.substr(digits));
    return tail + "@entry";
  }
  return string();
}

// The listing name for a global the decompiler could only call by its address:
// uRam000000010000c0d0 and its kind. The address itself is what the reader
// does not want to see, so only enough of it survives to keep two globals
// apart.
string globalPlaceName(const string &name)

{
  string::size_type at = name.find("Ram");
  if (at == string::npos || !isHexDigits(name,at+3))
    return string();
  for(string::size_type i=0;i<at;++i) {
    if (islower((unsigned char)name[i]) == 0) return string();
  }
  string digits = shortHex(name.substr(at+3));
  if (digits.size() > 4)
    digits = digits.substr(digits.size()-4);
  digits[0] = (char)toupper((unsigned char)digits[0]);
  return "g" + digits;
}

// The listing name for a slot the decompiler did place in the frame and then
// named after its offset: xStack_1e and its kind. The offset stays, because it
// is what tells two slots apart; the type letter and the spelling of the space
// go, because neither says anything the declaration does not.
string stackSlotName(const string &name)

{
  string::size_type at = name.find("Stack");
  if (at == string::npos) return string();
  for(string::size_type i=0;i<at;++i) {
    if (islower((unsigned char)name[i]) == 0) return string();
  }
  string::size_type rest = at + 5;
  // X marks storage the caller allocated, which is the other side of the frame.
  bool caller = rest < name.size() && (name[rest]=='X');
  if (rest < name.size() && (name[rest]=='X' || name[rest]=='Y'))
    ++rest;
  if (rest >= name.size() || name[rest] != '_') return string();
  ++rest;
  if (!isHexDigits(name,rest)) return string();
  return (caller ? "stack" : "local") + shortHex(name.substr(rest));
}

// True when a cast between these two says nothing a reader needs: same width,
// and both sides are plain integers whose only difference is how the bits are
// read.
bool castIsSilent(const Datatype *from,const Datatype *to)

{
  if (from == (const Datatype *)0 || to == (const Datatype *)0) return false;
  if (from->getSize() != to->getSize()) return false;
  type_metatype a = from->getMetatype();
  type_metatype b = to->getMetatype();
  bool aPlain = (a==TYPE_INT || a==TYPE_UINT || a==TYPE_UNKNOWN);
  bool bPlain = (b==TYPE_INT || b==TYPE_UINT || b==TYPE_UNKNOWN);
  if (!aPlain || !bPlain) return false;
  // A character keeps its cast: the quotes it prints in are the point.
  if (from->isCharPrint() || to->isCharPrint()) return false;
  if (from->isEnumType() || to->isEnumType()) return false;
  return true;
}

} // namespace

PrintAstralCapability::PrintAstralCapability(void)

{
  name = "astral-c";
  isdefault = false;
}

void PrintAstralCapability::initialize(void)

{
  if (findCapability(name) == (PrintLanguageCapability *)0)
    PrintLanguageCapability::initialize();
}

PrintLanguage *PrintAstralCapability::buildLanguage(Architecture *glb)

{
  return new PrintAstral(glb,name);
}

void registerAstralPrintLanguage(void)

{
  // Built the first time it is asked for rather than during static
  // initialization, so the registration happens whether or not the linker kept
  // this translation unit for its own sake.
  static PrintAstralCapability *capability = (PrintAstralCapability *)0;
  if (capability == (PrintAstralCapability *)0) {
    capability = new PrintAstralCapability();
    capability->initialize();
  }
}

PrintAstral::PrintAstral(Architecture *g,const string &nm)
  : PrintC(g,nm)

{
  charType = (Datatype *)0;
  triedCharType = false;
  for(int4 i=0;i<(int4)(sizeof(FRAME_REGISTERS)/sizeof(const char *));++i)
    frameRegisters.insert(string(FRAME_REGISTERS[i]));
  resetDefaults();
}

void PrintAstral::resetDefaults(void)

{
  PrintC::resetDefaults();
  // A null pointer reads as NULL rather than as a cast zero.
  setNULLPrinting(true);
  setHideImpliedExts(true);
}

void PrintAstral::initializeFromArchitecture(void)

{
  PrintC::initializeFromArchitecture();
  charType = (Datatype *)0;
  triedCharType = false;
}

Datatype *PrintAstral::getCharType(void)

{
  if (!triedCharType) {
    triedCharType = true;
    charType = glb->types->getTypeChar(glb->types->getSizeOfChar());
  }
  return charType;
}

bool PrintAstral::isFrameBase(const Varnode *vn) const

{
  if (vn == (const Varnode *)0) return false;
  if (!vn->isInput()) return false;
  if (vn->getSpace()->getType() != IPTR_PROCESSOR) return false;
  string reg = glb->translate->getRegisterName(vn->getSpace(),vn->getOffset(),vn->getSize());
  if (reg.empty()) return false;
  for(string::size_type i=0;i<reg.size();++i)
    reg[i] = (char)tolower((unsigned char)reg[i]);
  return frameRegisters.find(reg) != frameRegisters.end();
}

bool PrintAstral::frameSlot(const Varnode *vn,int8 &off) const

{
  // Casts and copies sit between the arithmetic and its use without changing
  // the address, so they are stepped over rather than printed.
  for(int4 guard=0;guard<8 && vn != (const Varnode *)0;++guard) {
    if (isFrameBase(vn)) { off = 0; return true; }
    const PcodeOp *def = vn->getDef();
    if (def == (const PcodeOp *)0) return false;
    OpCode code = def->code();
    if (code == CPUI_CAST || code == CPUI_COPY) {
      vn = def->getIn(0);
      continue;
    }
    return frameSlotOfOp(def,off);
  }
  return false;
}

bool PrintAstral::frameSlotOfOp(const PcodeOp *op,int8 &off) const

{
  if (op == (const PcodeOp *)0) return false;
  OpCode code = op->code();
  if (code != CPUI_INT_ADD && code != CPUI_INT_SUB && code != CPUI_PTRSUB)
    return false;
  const Varnode *shift = op->getIn(1);
  if (!shift->isConstant()) return false;
  int8 amount;
  if (!frameSlot(op->getIn(0),amount)) return false;
  uintb raw = shift->getOffset();
  int4 bits = shift->getSize() * 8;
  int8 value = (int8)raw;
  if (bits > 0 && bits < 64)
    value = ((int8)(raw << (64-bits))) >> (64-bits);
  off = amount + (code == CPUI_INT_SUB ? -value : value);
  return true;
}

// The text at a constant address, when the address is one the program can only
// read and the bytes there really are text. A message says far more about a
// call than the number that points at it.
bool PrintAstral::pushStringAt(uintb val,const Varnode *vn,const PcodeOp *op)

{
  if (val == 0) return false;
  Datatype *ct = getCharType();
  if (ct == (Datatype *)0) return false;
  AddrSpace *spc = glb->getDefaultDataSpace();
  if (spc == (AddrSpace *)0) return false;
  if (val > spc->getHighest()) return false;
  Address point;
  if (op != (const PcodeOp *)0)
    point = op->getAddr();
  uintb fullEncoding;
  Address addr = glb->resolveConstant(spc,val,spc->getAddrSize(),point,fullEncoding);
  if (addr.isInvalid()) return false;
  if (!glb->symboltab->getGlobalScope()->isReadOnly(addr,1,Address()))
    return false;
  ostringstream str;
  if (!printCharacterConstant(str,addr,ct))
    return false;
  pushAtom(Atom(str.str(),vartoken,EmitMarkup::const_color,op,vn));
  return true;
}

// Which slot of which call a constant sits in, written the way the knowledge
// base keys its records. Empty when the value is not a direct call argument,
// which is what keeps a request number from being named everywhere the same
// number happens to appear.
string PrintAstral::callContext(const PcodeOp *op,const Varnode *vn) const

{
  if (op == (const PcodeOp *)0 || vn == (const Varnode *)0) return string();
  if (op->code() != CPUI_CALL) return string();
  const Varnode *callpoint = op->getIn(0);
  if (callpoint->getSpace()->getType() != IPTR_FSPEC) return string();
  FuncCallSpecs *fc = FuncCallSpecs::getFspecFromConst(callpoint->getAddr());
  if (fc == (FuncCallSpecs *)0 || fc->getName().size() == 0) return string();
  int4 slot = op->getSlot(vn);
  if (slot < 1) return string();
  ostringstream s;
  s << fc->getName() << ':' << dec << (slot - 1);
  return s.str();
}

void PrintAstral::pushTypeStart(const Datatype *ct,bool noident)

{
  vector<const Datatype *> typestack;
  buildTypeStack(ct,typestack);

  ct = typestack.back();
  OpToken *tok = (noident && (typestack.size()==1)) ? &type_expr_nospace : &type_expr_space;

  string nm = ct->getName().size()==0 ? genericTypeName(ct) : readableTypeName(ct->getDisplayName());
  pushOp(tok,(const PcodeOp *)0);
  pushAtom(Atom(nm,typetoken,EmitMarkup::type_color,ct));

  for(int4 i=typestack.size()-2;i>=0;--i) {
    ct = typestack[i];
    if (ct->getMetatype() == TYPE_PTR)
      pushOp(&ptr_expr,(const PcodeOp *)0);
    else if (ct->getMetatype() == TYPE_ARRAY)
      pushOp(&array_expr,(const PcodeOp *)0);
    else if (ct->getMetatype() == TYPE_CODE)
      pushOp(&function_call,(const PcodeOp *)0);
    else {
      clear();
      throw LowlevelError("Bad type expression");
    }
  }
}

string PrintAstral::genericTypeName(const Datatype *ct)

{
  return readableTypeName(PrintC::genericTypeName(ct));
}

void PrintAstral::pushSymbol(const Symbol *sym,const Varnode *vn,const PcodeOp *op)

{
  const string &name(sym->getName());

  string entry = entryValueName(name);
  if (!entry.empty()) {
    pushAtom(Atom(entry,vartoken,EmitMarkup::special_color,op,vn));
    return;
  }

  string slot = stackSlotName(name);
  if (!slot.empty()) {
    pushAtom(Atom(slot,vartoken,EmitMarkup::var_color,op,vn));
    return;
  }

  string place = globalPlaceName(name);
  if (!place.empty()) {
    // Text beats a name every time: if the address holds a message, say it.
    const SymbolEntry *mapped = sym->getFirstWholeMap();
    if (mapped != (const SymbolEntry *)0 && getCharType() != (Datatype *)0 &&
	glb->symboltab->getGlobalScope()->isReadOnly(mapped->getAddr(),1,Address())) {
      ostringstream str;
      if (printCharacterConstant(str,mapped->getAddr(),getCharType())) {
	pushAtom(Atom(str.str(),vartoken,EmitMarkup::const_color,op,vn));
	return;
      }
    }
    pushAtom(Atom(place,vartoken,EmitMarkup::global_color,op,vn));
    return;
  }

  PrintC::pushSymbol(sym,vn,op);
}

void PrintAstral::pushUnnamedLocation(const Address &addr,const Varnode *vn,const PcodeOp *op)

{
  const string &spacename(addr.getSpace()->getName());
  if (spacename == "stack") {
    // The decompiler placed this in the frame but never named it. Its offset is
    // what identifies it, so that is the name.
    pushAtom(Atom(slotName((int8)addr.getOffset()),vartoken,EmitMarkup::var_color,op,vn));
    return;
  }
  if (spacename == "ram") {
    ostringstream raw;
    raw << "xRam";
    addr.printRaw(raw);
    string place = globalPlaceName(raw.str());
    if (!place.empty()) {
      pushAtom(Atom(place,vartoken,EmitMarkup::global_color,op,vn));
      return;
    }
  }
  PrintC::pushUnnamedLocation(addr,vn,op);
}

void PrintAstral::pushConstant(uintb val,const Datatype *ct,tagtype tag,
			       const Varnode *vn,const PcodeOp *op,uint4 displayFormat)

{
  type_metatype meta = ct->getMetatype();
  if ((meta==TYPE_INT || meta==TYPE_UINT || meta==TYPE_UNKNOWN) &&
      !ct->isCharPrint() && !ct->isEnumType()) {
    // A number in a call slot the knowledge base recognises reads as the name
    // the header gives it. The slot is part of the key, so a request number
    // stays a number wherever it is not a request.
    const astral_internal::Knowledge &knowledge = astral_internal::Knowledge::instance();
    string known = knowledge.constant_name(callContext(op,vn),val);
    if (!known.empty()) {
      pushAtom(Atom(known,vartoken,EmitMarkup::const_color,op,vn));
      return;
    }
    if (ct->getSize() >= 4 && pushStringAt(val,vn,op))
      return;
  }
  PrintC::pushConstant(val,ct,tag,vn,op,displayFormat);
}

void PrintAstral::opLoad(const PcodeOp *op)

{
  int8 off;
  if (frameSlot(op->getIn(1),off)) {
    pushAtom(Atom(slotName(off),vartoken,EmitMarkup::var_color,op,op->getOut()));
    return;
  }
  PrintC::opLoad(op);
}

void PrintAstral::opStore(const PcodeOp *op)

{
  int8 off;
  if (frameSlot(op->getIn(1),off)) {
    pushOp(&assignment,op);
    // Operands are pushed in reverse, the same order PrintC::opStore uses.
    pushVn(op->getIn(2),op,mods);
    pushAtom(Atom(slotName(off),vartoken,EmitMarkup::var_color,op,op->getIn(1)));
    return;
  }
  PrintC::opStore(op);
}

void PrintAstral::opIntAdd(const PcodeOp *op)

{
  int8 off;
  if (frameSlotOfOp(op,off)) {
    pushAtom(Atom("&" + slotName(off),vartoken,EmitMarkup::var_color,op,op->getOut()));
    return;
  }
  PrintC::opIntAdd(op);
}

void PrintAstral::opIntSub(const PcodeOp *op)

{
  int8 off;
  if (frameSlotOfOp(op,off)) {
    pushAtom(Atom("&" + slotName(off),vartoken,EmitMarkup::var_color,op,op->getOut()));
    return;
  }
  PrintC::opIntSub(op);
}

void PrintAstral::opPtrsub(const PcodeOp *op)

{
  int8 off;
  if (frameSlotOfOp(op,off)) {
    pushAtom(Atom("&" + slotName(off),vartoken,EmitMarkup::var_color,op,op->getOut()));
    return;
  }
  PrintC::opPtrsub(op);
}

void PrintAstral::opCast(const PcodeOp *op)

{
  int8 off;
  if (frameSlot(op->getIn(0),off)) {
    pushAtom(Atom("&" + slotName(off),vartoken,EmitMarkup::var_color,op,op->getOut()));
    return;
  }
  const Varnode *in = op->getIn(0);
  const Varnode *out = op->getOut();
  if (in != (const Varnode *)0 && out != (const Varnode *)0 &&
      castIsSilent(in->getHighTypeReadFacing(op),out->getHighTypeDefFacing())) {
    pushVn(in,op,mods);
    return;
  }
  PrintC::opCast(op);
}

} // End namespace ghidra
