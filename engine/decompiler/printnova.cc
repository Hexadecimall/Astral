// The Nova back-end. See printnova.hh for what separates it from PrintAstral.
#include "printnova.hh"

#include "funcdata.hh"

#include <cctype>
#include <sstream>

namespace ghidra {

// `value as u32`. Binds tighter than every arithmetic operator and looser than
// the postfix ones, which is where C's cast sits too, so an expression means
// the same thing read either way.
OpToken PrintNova::as_cast = { "as", "", 2, 62, false, OpToken::binary, 1, 0, (OpToken *)0 };

PrintNovaCapability::PrintNovaCapability(void)

{
  name = "nova";
  isdefault = false;
}

void PrintNovaCapability::initialize(void)

{
  if (findCapability(name) == (PrintLanguageCapability *)0)
    PrintLanguageCapability::initialize();
}

PrintLanguage *PrintNovaCapability::buildLanguage(Architecture *glb)

{
  return new PrintNova(glb,name);
}

void registerNovaPrintLanguage(void)

{
  // Built the first time it is asked for rather than during static
  // initialization, so the registration happens whether or not the linker kept
  // this translation unit for its own sake.
  static PrintNovaCapability *capability = (PrintNovaCapability *)0;
  if (capability == (PrintNovaCapability *)0) {
    capability = new PrintNovaCapability();
    capability->initialize();
  }
}

PrintNova::PrintNova(Architecture *g,const string &nm)
  : PrintAstral(g,nm)

{
  inParameters = false;
  resetDefaults();
}

void PrintNova::resetDefaults(void)

{
  PrintAstral::resetDefaults();
  // Nova writes `null`, not a cast zero, and not C's macro either. The keyword
  // the language actually has is what the reader should see.
  setNULLPrinting(true);
  // Nova's own keyword, not C's macro.
  nullToken = "null";
  // Every brace in Nova opens on the line that opened the block. A function
  // whose brace sits alone on the next line is C's habit, not Nova's.
  setBraceFormatFunction(Emit::same_line);
  setBraceFormatIfElse(Emit::same_line);
  setBraceFormatLoop(Emit::same_line);
  setBraceFormatSwitch(Emit::same_line);
}

// Nova puts the adornment before the thing it adorns, because that is the
// order the reader needs it in: what this is comes first, what it is made of
// comes after.
string PrintNova::novaType(const Datatype *ct)

{
  if (ct == (const Datatype *)0)
    return "void";
  switch(ct->getMetatype()) {
  case TYPE_PTR: {
    const TypePointer *pt = (const TypePointer *)ct;
    return "*" + novaType(pt->getPtrTo());
  }
  case TYPE_ARRAY: {
    const TypeArray *at = (const TypeArray *)ct;
    ostringstream s;
    s << '[' << novaType(at->getBase());
    if (at->numElements() > 0)
      s << "; " << dec << at->numElements();
    s << ']';
    return s.str();
  }
  case TYPE_VOID:
    return "void";
  case TYPE_CODE: {
    const TypeCode *code = (const TypeCode *)ct;
    const FuncProto *proto = code->getPrototype();
    if (proto == (const FuncProto *)0)
      return "func()";
    ostringstream s;
    s << "func(";
    int4 sz = proto->numParams();
    for(int4 i=0;i<sz;++i) {
      if (i != 0) s << ", ";
      s << novaType(proto->getParam(i)->getType());
    }
    if (proto->isDotdotdot())
      s << (sz == 0 ? "..." : ", ...");
    s << ')';
    Datatype *out = proto->getOutputType();
    if (out != (Datatype *)0 && out->getMetatype() != TYPE_VOID)
      s << ": " << novaType(out);
    return s.str();
  }
  default:
    break;
  }
  if (ct->getName().size() == 0)
    return genericTypeName(ct);
  return novaWidthName(readableTypeName(ct->getDisplayName()));
}

// Nova does not abbreviate. The readable listing says `unk32` because it grew
// out of a notation that was already terse; Nova says what it means.
string PrintNova::novaWidthName(const string &name)

{
  if (name.compare(0,3,"unk") == 0 && name.size() > 3) {
    bool digits = true;
    for(string::size_type i=3;i<name.size();++i)
      if (isdigit((unsigned char)name[i]) == 0) digits = false;
    if (digits)
      return "unknown" + name.substr(3);
  }
  return name;
}

string PrintNova::genericTypeName(const Datatype *ct)

{
  return novaWidthName(PrintAstral::genericTypeName(ct));
}

// A function nobody named is named after where it is. Nova spells that out
// rather than abbreviating it, and drops the leading zeros an address carries
// because they say nothing about which function this is.
string PrintNova::genericFunctionName(const Address &addr)

{
  ostringstream raw;
  addr.printRaw(raw);
  string text = raw.str();
  string::size_type colon = text.rfind(':');
  if (colon != string::npos)
    text = text.substr(colon + 1);
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    text = text.substr(2);
  string::size_type start = 0;
  while (start + 1 < text.size() && text[start] == '0')
    ++start;
  return "function" + text.substr(start);
}

// A global is somewhere in particular, and in Nova that is part of what it is.
// Saying it turns an address from noise in the middle of an expression into
// one fact stated once, where the thing is declared.
void PrintNova::emitStorage(const Symbol *sym)

{
  if (sym == (const Symbol *)0) return;
  if (sym->getScope() == (Scope *)0 || !sym->getScope()->isGlobal()) return;
  const SymbolEntry *entry = sym->getFirstWholeMap();
  if (entry == (const SymbolEntry *)0) return;
  const Address &addr = entry->getAddr();
  if (addr.isInvalid()) return;
  if (addr.getSpace() == (AddrSpace *)0) return;
  if (addr.getSpace()->getType() != IPTR_PROCESSOR &&
      addr.getSpace()->getType() != IPTR_CONSTANT) {
    // Only a real address in the image says anything; a register or a
    // synthetic space is not a place the reader can go and look.
    if (addr.getSpace()->getName() != "ram") return;
  }
  ostringstream s;
  s << " @ 0x" << hex << addr.getOffset();
  emit->print(s.str(),EmitMarkup::const_color);
}

void PrintNova::emitVarDecl(const Symbol *sym)

{
  int4 id = emit->beginVarDecl(sym);

  if (!inParameters) {
    emit->print("var",EmitMarkup::keyword_color);
    emit->spaces(1);
  }
  // The name goes through the readable printer, so a frame slot is localNN
  // and a value the caller left is x29@entry, exactly as in the listing.
  pushSymbol(sym,(Varnode *)0,(PcodeOp *)0);
  recurse();
  emit->print(":");
  emit->spaces(1);
  emit->print(novaType(sym->getType()),EmitMarkup::type_color);
  emitStorage(sym);

  emit->endVarDecl(id);
}

// Nova has no `void` parameter list: a function that takes nothing has empty
// parentheses, which already says it.
void PrintNova::emitPrototypeInputs(const FuncProto *proto)

{
  int4 sz = proto->numParams();
  bool printComma = false;
  for(int4 i=0;i<sz;++i) {
    ProtoParameter *param = proto->getParam(i);
    if (isSet(hide_thisparam) && param->isThisPointer())
      continue;
    if (printComma) {
      emit->print(COMMA);
      emit->spaces(1);
    }
    printComma = true;
    Symbol *sym = param->getSymbol();
    if (sym != (Symbol *)0) {
      inParameters = true;
      emitVarDecl(sym);
      inParameters = false;
    }
    else {
      // No symbol behind it, so there is no name to give: the type alone.
      emit->print(novaType(param->getType()),EmitMarkup::type_color);
    }
  }
  if (proto->isDotdotdot()) {
    if (printComma) {
      emit->print(COMMA);
      emit->spaces(1);
    }
    emit->print(DOTDOTDOT);
  }
}

void PrintNova::emitFunctionDeclaration(const Funcdata *fd)

{
  const FuncProto *proto = &fd->getFuncProto();
  int4 id = emit->beginFuncProto();

  emit->print("func",EmitMarkup::keyword_color);
  emit->spaces(1);

  int4 id1 = emit->openGroup();
  emitSymbolScope(fd->getSymbol());
  emit->tagFuncName(fd->getDisplayName(),EmitMarkup::funcname_color,fd,(PcodeOp *)0);

  emit->spaces(function_call.spacing,function_call.bump);
  int4 id2 = emit->openParen(OPEN_PAREN);
  emit->spaces(0,function_call.bump);
  pushScope(fd->getScopeLocal());	// Enter the function's scope for parameters
  emitPrototypeInputs(proto);
  emit->closeParen(CLOSE_PAREN,id2);
  emit->closeGroup(id1);

  // What it answers with comes after what it takes, because that is the order
  // it happens in.
  Datatype *outtype = proto->getOutputType();
  if (outtype != (Datatype *)0 && outtype->getMetatype() != TYPE_VOID) {
    emit->print(":");
    emit->spaces(1);
    emit->print(novaType(outtype),EmitMarkup::type_color);
  }
  if (option_convention && fd->getFuncProto().printModelInDecl()) {
    emit->spaces(1);
    Emit::syntax_highlight highlight =
      fd->getFuncProto().isModelUnknown() ? Emit::error_color : Emit::keyword_color;
    emit->print(fd->getFuncProto().getModelName(),highlight);
  }

  emit->endFuncProto(id);
}

void PrintNova::opCast(const PcodeOp *op)

{
  // A cast that only takes the address of a frame slot, and one that says
  // nothing a reader needs, are both handled by the readable printer already.
  int8 off;
  if (frameSlot(op->getIn(0),off)) {
    PrintAstral::opCast(op);
    return;
  }
  const Varnode *in = op->getIn(0);
  const Varnode *out = op->getOut();
  if (in == (const Varnode *)0 || out == (const Varnode *)0) {
    PrintAstral::opCast(op);
    return;
  }
  if (castIsSilent(in->getHighTypeReadFacing(op),out->getHighTypeDefFacing())) {
    PrintAstral::opCast(op);
    return;
  }
  if (option_nocasts) {
    pushVn(in,op,mods);
    return;
  }
  // Nova reads left to right: the value, then what it is being read as.
  Datatype *dt = out->getHighTypeDefFacing();
  pushOp(&as_cast,op);
  pushVn(in,op,mods);
  pushAtom(Atom(novaType(dt),typetoken,EmitMarkup::type_color,dt));
}

} // End namespace ghidra
