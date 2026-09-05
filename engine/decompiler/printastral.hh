/// \file printastral.hh
/// \brief The readable listing: Astral's own back-end for pseudo-C.
///
/// The C back-end prints what the decompiler knows in the notation the
/// decompiler thinks in: types named by byte width, frame slots left as
/// arithmetic on a register, globals spelled as their addresses. That listing
/// is what the compilable-C path is built from and it stays exactly as it is.
///
/// This one prints the same analysis for a person to read. It is free to use
/// notation no C compiler would take, because nothing downstream compiles it.
#ifndef ASTRAL_PRINTASTRAL_HH
#define ASTRAL_PRINTASTRAL_HH

#include "printc.hh"

#include <set>

namespace ghidra {

/// \brief Factory for the readable listing back-end
///
/// Registered under a name of its own, so selecting it never disturbs the
/// c-language printer the compilable path uses.
class PrintAstralCapability : public PrintLanguageCapability {
public:
  PrintAstralCapability(void);				///< Take the name "astral-c"
  virtual void initialize(void);
  virtual PrintLanguage *buildLanguage(Architecture *glb);
};

/// Registers the capability if it is not registered yet, so the name can be
/// handed to Architecture::setPrintLanguage. Cheap to call repeatedly.
void registerAstralPrintLanguage(void);

/// \brief The readable listing emitter
class PrintAstral : public PrintC {
  std::set<string> frameRegisters;	///< Registers whose entry value addresses the frame
  Datatype *charType;			///< Character type used to probe an address for text
  bool triedCharType;			///< Whether the character type has been looked up

  Datatype *getCharType(void);				///< The character type, looked up once
  bool isFrameBase(const Varnode *vn) const;		///< Is this the caller's frame register
  bool frameSlot(const Varnode *vn,int8 &off) const;	///< Does this value address a frame slot
  bool frameSlotOfOp(const PcodeOp *op,int8 &off) const;	///< Does this op form a frame address
  bool pushStringAt(uintb val,const Varnode *vn,const PcodeOp *op);	///< Push the text at an address
  string callContext(const PcodeOp *op,const Varnode *vn) const;	///< "ioctl:1" for a call argument
public:
  PrintAstral(Architecture *g,const string &nm="astral-c");
  virtual ~PrintAstral(void) {}

  virtual void resetDefaults(void);
  virtual void initializeFromArchitecture(void);

  virtual void pushTypeStart(const Datatype *ct,bool noident);
  virtual string genericTypeName(const Datatype *ct);
  virtual void pushSymbol(const Symbol *sym,const Varnode *vn,const PcodeOp *op);
  virtual void pushMismatchSymbol(const Symbol *sym,int4 off,int4 sz,
				  const Varnode *vn,const PcodeOp *op);
  virtual void pushUnnamedLocation(const Address &addr,const Varnode *vn,const PcodeOp *op);
  virtual void pushConstant(uintb val,const Datatype *ct,tagtype tag,
			    const Varnode *vn,const PcodeOp *op,uint4 displayFormat);
  virtual void opLoad(const PcodeOp *op);
  virtual void opStore(const PcodeOp *op);
  virtual void opIntAdd(const PcodeOp *op);
  virtual void opIntSub(const PcodeOp *op);
  virtual void opPtrsub(const PcodeOp *op);
  virtual void opCast(const PcodeOp *op);
};

} // End namespace ghidra
#endif
