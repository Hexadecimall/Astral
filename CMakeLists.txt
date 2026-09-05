cmake_minimum_required(VERSION 3.20)
project(Astral VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Default to a per-user location, but never override a prefix the caller asked
# for. CMake sets this variable only when the prefix is its own default, which
# is what distinguishes "unspecified" from "someone typed /usr/local".
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.astral" CACHE PATH "Install location" FORCE)
endif()

include(cmake/EngineSources.cmake)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/engine/GHIDRA_VERSION ASTRAL_GHIDRA_VERSION)
string(STRIP "${ASTRAL_GHIDRA_VERSION}" ASTRAL_GHIDRA_VERSION)

find_package(ZLIB REQUIRED)

# ---------------------------------------------------------------- vendored core
astral_expand(_core_src ${ASTRAL_CORE} ${ASTRAL_DECCORE} ${ASTRAL_SLEIGH} ${ASTRAL_EXTRA})
astral_expand(_slacomp_src ${ASTRAL_SLACOMP})

add_library(astral_vendor OBJECT ${_core_src} ${_slacomp_src})
target_include_directories(astral_vendor PUBLIC ${ASTRAL_DECOMP_DIR})
target_link_libraries(astral_vendor PUBLIC ZLIB::ZLIB)
# slgh_compile.cc carries the stand-alone `sleigh` tool entry point; rename it away
# so the same unmodified source can live inside the library.
set_source_files_properties(${ASTRAL_DECOMP_DIR}/slgh_compile.cc
    PROPERTIES COMPILE_DEFINITIONS "main=astral_sleigh_tool_main_unused")
target_compile_options(astral_vendor PRIVATE -w)

# ---------------------------------------------------------------- library proper
set(KNOWLEDGE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/data/knowledge.astral)
set(KNOWLEDGE_CONTRIB ${CMAKE_CURRENT_SOURCE_DIR}/contrib/databases)
set(KNOWLEDGE_COMBINED ${CMAKE_BINARY_DIR}/generated/knowledge.astral)
set(KNOWLEDGE_EMBEDDED ${CMAKE_BINARY_DIR}/generated/knowledge_seed.cc)

file(GLOB KNOWLEDGE_CONTRIBUTED ${KNOWLEDGE_CONTRIB}/*.astral)
add_custom_command(OUTPUT ${KNOWLEDGE_COMBINED}
    COMMAND ${CMAKE_COMMAND} -DSEED=${KNOWLEDGE_FILE} -DCONTRIB=${KNOWLEDGE_CONTRIB}
            -DOUTPUT=${KNOWLEDGE_COMBINED}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CombineKnowledge.cmake
    DEPENDS ${KNOWLEDGE_FILE} ${KNOWLEDGE_CONTRIBUTED}
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CombineKnowledge.cmake
    COMMENT "joining the knowledge base with contributed databases")

add_custom_command(OUTPUT ${KNOWLEDGE_EMBEDDED}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${KNOWLEDGE_COMBINED} -DOUTPUT=${KNOWLEDGE_EMBEDDED}
            -DSYMBOL=SEED_KNOWLEDGE_TEXT -DTAG=ASTRAL_KNOWLEDGE
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${KNOWLEDGE_COMBINED} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
    COMMENT "embedding the knowledge base")

set(RUNTIME_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/astral/decompiled.h)
set(RUNTIME_EMBEDDED ${CMAKE_BINARY_DIR}/generated/runtime_header.cc)
add_custom_command(OUTPUT ${RUNTIME_EMBEDDED}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${RUNTIME_HEADER} -DOUTPUT=${RUNTIME_EMBEDDED}
            -DSYMBOL=RUNTIME_HEADER_TEXT -DTAG=ASTRAL_RUNTIME
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${RUNTIME_HEADER} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
    COMMENT "embedding the decompiled-C runtime header")

set(ASTRAL_LIB_SRC
    ${RUNTIME_EMBEDDED}
    ${KNOWLEDGE_EMBEDDED}
    src/knowledge.cc
    src/naming.cc
    src/paths.cc
    src/contribute.cc
    src/source_learn.cc
    src/creal.cc
    src/cpp_api.cc
    src/error.cc
    src/image.cc
    src/demangle.cc
    src/cxx_idioms.cc
    src/patch.cc
    src/sha256.cc
    src/macho_sign.cc
    src/loader_elf.cc
    src/loader_pe.cc
    src/loader_macho.cc
    src/langmap.cc
    src/libc_protos.cc
    src/session.cc
    src/capi.cc)

add_library(astral_objs OBJECT ${ASTRAL_LIB_SRC})
target_include_directories(astral_objs
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${ASTRAL_DECOMP_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
# No path is written into the library. Where things are is worked out while
# running, so a binary never names the machine that built it and never breaks
# when an install is moved.
target_compile_definitions(astral_objs PRIVATE
    ASTRAL_VERSION="${PROJECT_VERSION}"
    ASTRAL_GHIDRA_VERSION="${ASTRAL_GHIDRA_VERSION}")
target_link_libraries(astral_objs PUBLIC ZLIB::ZLIB)

add_library(astral_static STATIC
    $<TARGET_OBJECTS:astral_vendor> $<TARGET_OBJECTS:astral_objs>)
set_target_properties(astral_static PROPERTIES OUTPUT_NAME Astral)
target_include_directories(astral_static INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(astral_static PUBLIC ZLIB::ZLIB)

add_library(astral_shared SHARED
    $<TARGET_OBJECTS:astral_vendor> $<TARGET_OBJECTS:astral_objs>)
set_target_properties(astral_shared PROPERTIES
    OUTPUT_NAME Astral
    VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
    # Consumers link against an absolute path, so no rpath juggling is needed.
    INSTALL_NAME_DIR "${CMAKE_INSTALL_PREFIX}/lib/astral/dynamic-libs"
    BUILD_WITH_INSTALL_NAME_DIR ON
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib/astral/dynamic-libs")
target_include_directories(astral_shared INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(astral_shared PUBLIC ZLIB::ZLIB)

# ---------------------------------------------------------------- the command
#
# One program. The command line, the terminal interface, the updater and the
# specification compiler are all modules of a single Rust binary that links the
# library in; letting the Rust toolchain perform the final link is what makes
# that possible, since the platform linker cannot read the objects it produces.
find_program(CARGO_EXECUTABLE cargo)
option(ASTRAL_BUILD_TUI "Build the astral command (needs cargo)" ON)

set(ASTRAL_CLI_BUILT ${CMAKE_CURRENT_SOURCE_DIR}/cli/target/release/astral)
set(ASTRAL_CLI ${CMAKE_BINARY_DIR}/astral)

# Rust records the file each panic could come from, which would otherwise write
# the build machine's home directory into every shipped binary. These rewrite
# those roots to neutral names; the separator is the unit character cargo reads
# CARGO_ENCODED_RUSTFLAGS with, so a directory containing a space still works.
string(ASCII 31 _rustflag_separator)
set(_remap "--remap-path-prefix=${CMAKE_CURRENT_SOURCE_DIR}=/astral")
foreach(_root CARGO_HOME RUSTUP_HOME HOME)
    if(DEFINED ENV{${_root}})
        string(TOLOWER ${_root} _name)
        string(APPEND _remap
               "${_rustflag_separator}--remap-path-prefix=$ENV{${_root}}=/${_name}")
    endif()
endforeach()

if(ASTRAL_BUILD_TUI AND CARGO_EXECUTABLE)
    file(GLOB_RECURSE CLI_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/cli/src/*.rs
        ${CMAKE_CURRENT_SOURCE_DIR}/tui/src/*.rs
        ${CMAKE_CURRENT_SOURCE_DIR}/bindings/rust/src/*.rs)
    add_custom_command(OUTPUT ${ASTRAL_CLI}
        COMMAND ${CMAKE_COMMAND} -E env ASTRAL_LIB_DIR=$<TARGET_FILE_DIR:astral_static>
                "CARGO_ENCODED_RUSTFLAGS=${_remap}"
                ${CARGO_EXECUTABLE} build --release --quiet
        # Beside the libraries as well, so a build tree can be used in place.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ASTRAL_CLI_BUILT} ${ASTRAL_CLI}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/cli
        DEPENDS astral_static ${CLI_SOURCES}
                ${CMAKE_CURRENT_SOURCE_DIR}/cli/Cargo.toml
                ${CMAKE_CURRENT_SOURCE_DIR}/tui/Cargo.toml
                ${CMAKE_CURRENT_SOURCE_DIR}/bindings/rust/Cargo.toml
                ${CMAKE_CURRENT_SOURCE_DIR}/bindings/rust/build.rs
        COMMENT "building the astral command")
    add_custom_target(astral_cli ALL DEPENDS ${ASTRAL_CLI})
elseif(NOT CARGO_EXECUTABLE)
    message(STATUS "cargo was not found: the astral command cannot be built, "
                   "so this build produces only the libraries and headers")
else()
    message(STATUS "ASTRAL_BUILD_TUI is off: the astral command will not be built, "
                   "so this build produces only the libraries and headers")
endif()

# ---------------------------------------------------------------- the application
option(ASTRAL_BUILD_GUI "Build the Astral desktop application (needs Qt 6)" ON)
if(ASTRAL_BUILD_GUI)
    add_subdirectory(gui)
endif()

# ---------------------------------------------------------------- sleigh specs
set(ASTRAL_LANGUAGES "x86;AARCH64;ARM;MIPS;PowerPC;RISCV;JVM;Sparc;68000;Z80;PIC;8051;SuperH4;Xtensa;Toy;DATA"
    CACHE STRING "Processor directories whose SLEIGH specs get compiled (ALL for every one)")

set(SPEC_STAGE ${CMAKE_BINARY_DIR}/specs)
set(PROC_SRC ${CMAKE_CURRENT_SOURCE_DIR}/sleigh)

if(ASTRAL_LANGUAGES STREQUAL "ALL")
    file(GLOB _proc_dirs RELATIVE ${PROC_SRC} ${PROC_SRC}/*)
    set(ASTRAL_LANGUAGES "")
    foreach(_d ${_proc_dirs})
        if(IS_DIRECTORY ${PROC_SRC}/${_d})
            list(APPEND ASTRAL_LANGUAGES ${_d})
        endif()
    endforeach()
endif()

set(_all_sla "")
foreach(_proc ${ASTRAL_LANGUAGES})
    set(_src ${PROC_SRC}/${_proc}/data/languages)
    if(NOT IS_DIRECTORY ${_src})
        message(WARNING "no such processor directory: ${_proc}")
        continue()
    endif()
    set(_dst ${SPEC_STAGE}/${_proc}/data/languages)
    file(GLOB _spec_files ${_src}/*)
    file(GLOB _slaspecs RELATIVE ${_src} ${_src}/*.slaspec)
    set(_stamp ${SPEC_STAGE}/.${_proc}.copied)
    add_custom_command(OUTPUT ${_stamp}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_dst}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${_src} ${_dst}
        COMMAND ${CMAKE_COMMAND} -E touch ${_stamp}
        DEPENDS ${_spec_files}
        COMMENT "staging ${_proc} language files")
    if(NOT TARGET astral_cli)
        continue()
    endif()
    foreach(_ss ${_slaspecs})
        string(REPLACE ".slaspec" ".sla" _sla ${_ss})
        add_custom_command(OUTPUT ${_dst}/${_sla}
            COMMAND ${ASTRAL_CLI} sleigh ${_dst}/${_ss} ${_dst}/${_sla}
            DEPENDS ${_stamp} astral_cli
            COMMENT "sleigh ${_proc}/${_ss}")
        list(APPEND _all_sla ${_dst}/${_sla})
    endforeach()
endforeach()

add_custom_target(specs ALL DEPENDS ${_all_sla})

# ---------------------------------------------------------------- install
include(GNUInstallDirs)

# Libraries live under their own directory rather than loose in lib, so an
# install can be identified and removed as a unit, and so the dynamic and static
# forms are told apart at a glance.
set(ASTRAL_DYNAMIC_LIB_DIR lib/astral/dynamic-libs CACHE STRING "where shared libraries go")
set(ASTRAL_STATIC_LIB_DIR lib/astral/static-libs CACHE STRING "where static libraries go")

install(TARGETS astral_static ARCHIVE DESTINATION ${ASTRAL_STATIC_LIB_DIR})
install(TARGETS astral_shared LIBRARY DESTINATION ${ASTRAL_DYNAMIC_LIB_DIR})
if(ASTRAL_BUILD_TUI AND CARGO_EXECUTABLE)
    install(PROGRAMS ${ASTRAL_CLI_BUILT} DESTINATION bin)
endif()
install(DIRECTORY include/astral DESTINATION include)
install(DIRECTORY ${SPEC_STAGE}/ DESTINATION share/astral/specs
        PATTERN ".*" EXCLUDE)
install(DIRECTORY bindings/rust/ DESTINATION share/astral/rust)
install(FILES engine/LICENSE engine/NOTICE
        DESTINATION share/astral)
