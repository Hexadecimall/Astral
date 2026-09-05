# Source partition of Astral's decompiler engine, derived from Ghidra's
# decompiler (Apache-2.0, see engine/NOTICE) and modified in tree.


set(ASTRAL_DECOMP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine)

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

# The engine is kept in folders that say what each part is for; the sets above
# say which folder each name lives in.
set(ASTRAL_ENGINE_DIRS core decompiler sleigh runtime assembler upstream)

# Every folder is on the include path, so a header is included by its own name
# whichever part of the engine reaches for it.
set(ASTRAL_ENGINE_INCLUDE_DIRS "")
foreach(_dir ${ASTRAL_ENGINE_DIRS})
    list(APPEND ASTRAL_ENGINE_INCLUDE_DIRS ${ASTRAL_DECOMP_DIR}/${_dir})
endforeach()

# Finds each named unit wherever it sits.
function(astral_expand out_var)
    set(_acc "")
    foreach(_n ${ARGN})
        set(_found "")
        foreach(_dir ${ASTRAL_ENGINE_DIRS})
            if(EXISTS ${ASTRAL_DECOMP_DIR}/${_dir}/${_n}.cc)
                set(_found ${ASTRAL_DECOMP_DIR}/${_dir}/${_n}.cc)
                break()
            endif()
        endforeach()
        if(_found STREQUAL "")
            message(FATAL_ERROR "engine source ${_n}.cc is not in any of ${ASTRAL_ENGINE_DIRS}")
        endif()
        list(APPEND _acc ${_found})
    endforeach()
    set(${out_var} ${_acc} PARENT_SCOPE)
endfunction()
