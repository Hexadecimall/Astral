cmake_minimum_required(VERSION 3.20)
project(Astral VERSION 0.1.0 LANGUAGES CXX)

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

include(cmake/GhidraSources.cmake)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ghidra/VERSION ASTRAL_GHIDRA_VERSION)
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
    src/contribute.cc
    src/source_learn.cc
    src/creal.cc
    src/cpp_api.cc
    src/error.cc
    src/image.cc
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
target_compile_definitions(astral_objs PRIVATE
    ASTRAL_VERSION="${PROJECT_VERSION}"
    ASTRAL_GHIDRA_VERSION="${ASTRAL_GHIDRA_VERSION}"
    ASTRAL_DEFAULT_SPEC_ROOT="${CMAKE_INSTALL_PREFIX}/share/astral/specs")
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

# ---------------------------------------------------------------- tools
add_executable(astral-sleigh tools/sleighc_main.cc)
target_link_libraries(astral-sleigh PRIVATE astral_static)
target_include_directories(astral-sleigh PRIVATE ${ASTRAL_DECOMP_DIR})

add_executable(astral tools/astral_main.cc)
target_link_libraries(astral PRIVATE astral_static)
target_compile_definitions(astral PRIVATE
    ASTRAL_VERSION="${PROJECT_VERSION}"
    ASTRAL_GHIDRA_VERSION="${ASTRAL_GHIDRA_VERSION}")

add_executable(astral-update tools/update_main.cc)
target_compile_definitions(astral-update PRIVATE
    ASTRAL_VERSION="${PROJECT_VERSION}"
    ASTRAL_GHIDRA_VERSION="${ASTRAL_GHIDRA_VERSION}"
    ASTRAL_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
    ASTRAL_INSTALL_ROOT="${CMAKE_INSTALL_PREFIX}")

# ------------------------------------------------------------------- interface
#
# The terminal interface is a Rust program built on the same library, so the
# library itself never gains a terminal dependency. It is optional: without
# cargo the rest of Astral still builds.
find_program(CARGO_EXECUTABLE cargo)
option(ASTRAL_BUILD_TUI "Build the terminal interface (needs cargo)" ON)

if(ASTRAL_BUILD_TUI AND CARGO_EXECUTABLE)
    set(TUI_BINARY ${CMAKE_CURRENT_SOURCE_DIR}/tui/target/release/astral-tui)
    add_custom_target(astral-tui ALL
        COMMAND ${CMAKE_COMMAND} -E env ASTRAL_LIB_DIR=$<TARGET_FILE_DIR:astral_static>
                ${CARGO_EXECUTABLE} build --release --quiet
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tui
        DEPENDS astral_static
        COMMENT "building the terminal interface")
    install(PROGRAMS ${TUI_BINARY} DESTINATION bin)
elseif(ASTRAL_BUILD_TUI)
    message(STATUS "cargo not found: the terminal interface will not be built")
endif()

# ---------------------------------------------------------------- sleigh specs
set(ASTRAL_LANGUAGES "x86;AARCH64;ARM;MIPS;PowerPC;RISCV;JVM;Sparc;68000;Z80;PIC;8051;SuperH4;Xtensa;Toy;DATA"
    CACHE STRING "Processor directories whose SLEIGH specs get compiled (ALL for every one)")

set(SPEC_STAGE ${CMAKE_BINARY_DIR}/specs)
set(PROC_SRC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ghidra/processors)

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
    foreach(_ss ${_slaspecs})
        string(REPLACE ".slaspec" ".sla" _sla ${_ss})
        add_custom_command(OUTPUT ${_dst}/${_sla}
            COMMAND astral-sleigh ${_dst}/${_ss} ${_dst}/${_sla}
            DEPENDS ${_stamp} astral-sleigh
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
install(TARGETS astral-sleigh astral astral-update RUNTIME DESTINATION bin)
install(DIRECTORY include/astral DESTINATION include)
install(DIRECTORY ${SPEC_STAGE}/ DESTINATION share/astral/specs
        PATTERN ".*" EXCLUDE)
install(DIRECTORY bindings/rust/ DESTINATION share/astral/rust)
install(FILES third_party/ghidra/LICENSE third_party/ghidra/NOTICE
        DESTINATION share/astral)
