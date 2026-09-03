# Source partition of the vendored Ghidra decompiler.
# Mirrors the groupings in the upstream Makefile so the vendored tree stays unmodified.

set(ASTRAL_DECOMP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ghidra/decompile)

set(ASTRAL_CORE
    xml marshal space float address pcoderaw translate opcodes globalcontext)

set(ASTRAL_DECCORE
    capability architecture options graph cover block cast typeop database cpool
    comment stringmanage modelrules fspec action loadimage grammar varnode op type
    variable varmap jumptable emulate emulateutil flow userop expression multiprecision
    funcdata funcdata_block funcdata_op funcdata_varnode unionresolve pcodeinject
    heritage prefersplit rangeutil ruleaction subflow blockaction merge double
    transform constseq bitfield coreaction condexe override dynamic crc32 prettyprint
    printlanguage printc printjava memstate opbehavior paramid signature)

set(ASTRAL_SLEIGH
    sleigh pcodeparse pcodecompile sleighbase slghsymbol slghpatexpress slghpattern
    semantics context slaformat compression filemanage)

# Extra units needed for a standalone (non-Ghidra-process) build. The upstream
# "EXTRA" set minus the libbfd-dependent, console-interface and rule-compiler units.
set(ASTRAL_EXTRA
    inject_sleigh libdecomp loadimage_xml raw_arch sleigh_arch xml_arch)

# The SLEIGH specification compiler.
set(ASTRAL_SLACOMP slgh_compile slghparse slghscan)

function(astral_expand out_var)
    set(_acc "")
    foreach(_n ${ARGN})
        list(APPEND _acc ${ASTRAL_DECOMP_DIR}/${_n}.cc)
    endforeach()
    set(${out_var} ${_acc} PARENT_SCOPE)
endfunction()
