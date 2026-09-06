/// \file printnova.hh
/// \brief The Nova back-end: what the decompiler knows, written as Nova.
///
/// The readable listing prints C's shapes with better names in them. Nova is
/// not C, so this prints Nova's shapes: a declaration names the thing before
/// its type, a function says what it answers with after its parameters, a
/// cast reads left to right, and storage the decompiler recovered is said out
/// loud with `@` rather than left implied.
///
/// Everything about *naming* comes from PrintAstral, which this extends: frame
/// slots, entry values, globals that are really strings, constants named in
/// context. The two never disagree about what something is called, only about
/// how the surrounding syntax is spelled.
#ifndef ASTRAL_PRINTNOVA_HH
#define ASTRAL_PRINTNOVA_HH

#include "printastral.hh"

namespace ghidra {

/// \brief Factory for the Nova back-end
class PrintNovaCapability : public PrintLanguageCapability {
public:
  PrintNovaCapability(void);				///< Take the name "nova"
  virtual void initialize(void);
  virtual PrintLanguage *buildLanguage(Architecture *glb);
};

/// Registers the capability if it is not registered yet, so the name can be
/// handed to Architecture::setPrintLanguage. Cheap to call repeatedly.
void registerNovaPrintLanguage(void);

/// \brief The Nova emitter
class PrintNova : public PrintAstral {
  /// A cast, written after the value it applies to: `value as u32`.
  static OpToken as_cast;

  bool inParameters;			///< Parameters name their type without `var`

  string novaType(const Datatype *ct);			///< A type, spelled the way Nova spells it
  void emitStorage(const Symbol *sym);			///< ` @ 0x1000` for a global that has one
  static string novaWidthName(const string &name);	///< `unk32` written out as `unknown32`
public:
  PrintNova(Architecture *g,const string &nm="nova");
  virtual ~PrintNova(void) {}

  virtual void resetDefaults(void);

  virtual void emitVarDecl(const Symbol *sym);
  virtual void emitPrototypeInputs(const FuncProto *proto);
  virtual void emitFunctionDeclaration(const Funcdata *fd);
  virtual void opTypeCast(const PcodeOp *op);
  virtual void pushConstant(uintb val,const Datatype *ct,tagtype tag,
			    const Varnode *vn,const PcodeOp *op,uint4 displayFormat);
  virtual void opBranchind(const PcodeOp *op);
  virtual void emitBlockSwitch(const BlockSwitch *bl);
  virtual string genericFunctionName(const Address &addr);
  virtual string genericTypeName(const Datatype *ct);
};

} // End namespace ghidra
#endif
