# The desktop application. A pure client of the public library API, so it
# links Astral the way any other program would.
cmake_minimum_required(VERSION 3.20)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Homebrew keeps Qt out of the default search path; ask it where, so a plain
# configure finds Qt without the caller passing CMAKE_PREFIX_PATH.
find_program(HOMEBREW_EXECUTABLE brew)
if(HOMEBREW_EXECUTABLE)
    execute_process(COMMAND ${HOMEBREW_EXECUTABLE} --prefix qt
                    OUTPUT_VARIABLE _brew_qt OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _brew_result)
    if(_brew_result EQUAL 0 AND IS_DIRECTORY "${_brew_qt}")
        list(APPEND CMAKE_PREFIX_PATH "${_brew_qt}")
        # A Qt6_DIR remembered from a component-only tree (qtbase) cannot
        # resolve the add-on modules; the umbrella prefix can.
        if(Qt6_DIR AND NOT Qt6_DIR MATCHES "^${_brew_qt}/")
            unset(Qt6_DIR CACHE)
        endif()
    endif()
endif()

find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Widgets Svg Test)
if(NOT Qt6_FOUND)
    message(STATUS "Qt6 was not found (${Qt6_NOT_FOUND_MESSAGE}): the Astral application will not be built")
    return()
endif()

# ---------------------------------------------------------------- vendored sqlite
# The project store. Compiled from the amalgamation into the application, so
# nothing has to be installed and no dynamic library is looked for at runtime.
enable_language(C)
add_library(astral_gui_sqlite STATIC vendor/sqlite/sqlite3.c)
set_target_properties(astral_gui_sqlite PROPERTIES AUTOMOC OFF AUTORCC OFF C_STANDARD 11)
target_include_directories(astral_gui_sqlite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(astral_gui_sqlite PRIVATE
    SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1 SQLITE_DQS=0)
# Vendored third-party code, warned about the way the rest of it is.
target_compile_options(astral_gui_sqlite PRIVATE -w)

set(GUI_SOURCES
    app/main.cc
    app/mainwindow.cc
    app/titlebar.cc
    app/welcomepage.cc
    app/functionspane.cc
    app/tablepane.cc
    app/searchresults.cc
    app/programtab.cc
    app/projectcontroller.cc
    model/programdocument.cc
    model/project.cc
    model/functionlistmodel.cc
    model/settings.cc
    model/debugsession.cc
    model/patchbuilder.cc
    model/literalspace.cc
    model/sourcepatcher.cc
    model/assembler.cc
    views/codeview.cc
    views/decompilerview.cc
    views/listingview.cc
    views/hexview.cc
    views/debuggerpane.cc
    views/hexpane.cc
    views/listingpane.cc
    theme/theme.cc
    resources.qrc)
list(APPEND GUI_SOURCES platform/window.cc)
if(APPLE)
    enable_language(OBJCXX)
    list(APPEND GUI_SOURCES platform/window_mac.mm)
endif()

# The sources compile once, into an object library that both the bundle and
# the plain binary link.
add_library(astral_gui_objs OBJECT ${GUI_SOURCES})
set_target_properties(astral_gui_objs PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
# The engine's own headers, so the application reaches the compiler and the
# assembler built into it rather than running anything else.
target_include_directories(astral_gui_objs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${ASTRAL_ENGINE_INCLUDE_DIRS})
target_compile_definitions(astral_gui_objs PRIVATE
    ASTRAL_GUI_VERSION="${PROJECT_VERSION}"
    QT_NO_KEYWORDS
    QT_NO_CAST_FROM_ASCII)
target_link_libraries(astral_gui_objs PRIVATE Qt6::Widgets Qt6::Svg astral_shared astral_gui_sqlite)
target_compile_options(astral_gui_objs PRIVATE -Wall -Wextra)

add_executable(astral_gui MACOSX_BUNDLE WIN32 $<TARGET_OBJECTS:astral_gui_objs>)

# The same program as one file: gui/astral-gui. The engine is linked in
# statically and the theme and icons are embedded, so the file is the whole
# application apart from Qt, which comes from the system.
add_executable(astral_gui_bin $<TARGET_OBJECTS:astral_gui_objs>)
set_target_properties(astral_gui_bin PROPERTIES
    OUTPUT_NAME astral-gui
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_link_libraries(astral_gui_bin PRIVATE Qt6::Widgets Qt6::Svg astral_static astral_gui_sqlite)

# The single file is what gets handed to someone else, so it is built for
# speed and carries nothing it does not need to run: no debugging tables, no
# symbol names beyond the ones the dynamic linker reads.
target_compile_options(astral_gui_bin PRIVATE -O3)
if(APPLE)
    target_link_options(astral_gui_bin PRIVATE -Wl,-dead_strip)
    # -x -S and an ad-hoc signature are both Apple's: the linker there refuses
    # an unsigned binary, and the flags mean nothing to GNU strip.
    add_custom_command(TARGET astral_gui_bin POST_BUILD
        COMMAND strip -x -S $<TARGET_FILE:astral_gui_bin>
        COMMAND codesign --force --sign - $<TARGET_FILE:astral_gui_bin>
        COMMENT "stripping the single-file application"
        VERBATIM)
else()
    target_link_options(astral_gui_bin PRIVATE -Wl,--gc-sections)
    add_custom_command(TARGET astral_gui_bin POST_BUILD
        COMMAND strip --strip-unneeded $<TARGET_FILE:astral_gui_bin>
        COMMENT "stripping the single-file application"
        VERBATIM)
endif()
# gui/specs for the plain binary: the library looks beside the executable.
add_custom_command(TARGET astral_gui_bin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/specs
            $<TARGET_FILE_DIR:astral_gui_bin>/specs
    COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/include
            $<TARGET_FILE_DIR:astral_gui_bin>/include
    VERBATIM)
set_target_properties(astral_gui PROPERTIES
    OUTPUT_NAME Astral
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    MACOSX_BUNDLE_BUNDLE_NAME Astral
    MACOSX_BUNDLE_GUI_IDENTIFIER dev.astral.app
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
target_include_directories(astral_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(astral_gui PRIVATE
    ASTRAL_GUI_VERSION="${PROJECT_VERSION}"
    QT_NO_KEYWORDS
    QT_NO_CAST_FROM_ASCII)
target_link_libraries(astral_gui PRIVATE Qt6::Widgets Qt6::Svg astral_shared astral_gui_sqlite)
if(APPLE)
    target_link_libraries(astral_gui PRIVATE "-framework AppKit")
    set_source_files_properties(platform/window_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

target_compile_options(astral_gui PRIVATE -Wall -Wextra)

# The application carries its own copy of the library, so a build tree runs
# in place and an installed bundle needs nothing beside it. The specifications
# are linked in from the build tree; an installer copies them for real.
if(APPLE)
    set(_contents $<TARGET_BUNDLE_CONTENT_DIR:astral_gui>)
    add_custom_command(TARGET astral_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_contents}/Frameworks
        # Older copies of the library go, or a version bump leaves stale ones beside the new.
        COMMAND sh -c "rm -f '${_contents}/Frameworks/'libAstral*.dylib"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:astral_shared>
                ${_contents}/Frameworks/$<TARGET_FILE_NAME:astral_shared>
        COMMAND install_name_tool -change
                $<TARGET_PROPERTY:astral_shared,INSTALL_NAME_DIR>/$<TARGET_SONAME_FILE_NAME:astral_shared>
                @executable_path/../Frameworks/$<TARGET_FILE_NAME:astral_shared>
                $<TARGET_FILE:astral_gui>
        COMMAND codesign --force --sign - $<TARGET_FILE:astral_gui>
        COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/specs
                ${_contents}/Frameworks/specs
        COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/include
                ${_contents}/Frameworks/include
        COMMENT "bundling the library into Astral.app"
        VERBATIM)

    # The application icon, rendered from the SVG mark at build time.
    set(_icns ${CMAKE_CURRENT_BINARY_DIR}/Astral.icns)
    add_custom_command(OUTPUT ${_icns}
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/packaging/make_icns.sh
                ${CMAKE_CURRENT_SOURCE_DIR}/icons/logo.svg ${_icns}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/icons/logo.svg ${CMAKE_CURRENT_SOURCE_DIR}/packaging/make_icns.sh
        COMMENT "rendering the application icon")
    target_sources(astral_gui PRIVATE ${_icns})
    set_source_files_properties(${_icns} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(astral_gui PROPERTIES MACOSX_BUNDLE_ICON_FILE Astral.icns)

    # `make astral_gui_dist` produces gui/dist/Astral.app: the Qt frameworks and
    # plugins copied in by macdeployqt, the specifications and headers copied
    # rather than linked, and the whole bundle signed ad hoc. It runs on a
    # machine with neither Homebrew nor a Qt install.
    find_program(MACDEPLOYQT_EXECUTABLE macdeployqt HINTS ${_brew_qt}/bin)
    set(_dist ${CMAKE_CURRENT_BINARY_DIR}/dist)
    add_custom_target(astral_gui_dist
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_dist}/Astral.app
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_dist}
        COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_BUNDLE_DIR:astral_gui> ${_dist}/Astral.app
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_dist}/Astral.app/Contents/Frameworks/specs
                ${_dist}/Astral.app/Contents/Frameworks/include
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/specs
                ${_dist}/Astral.app/Contents/Frameworks/specs
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/include
                ${_dist}/Astral.app/Contents/Frameworks/include
        COMMAND ${MACDEPLOYQT_EXECUTABLE} ${_dist}/Astral.app -always-overwrite -no-strip
        COMMAND codesign --force --deep --sign - ${_dist}/Astral.app
        DEPENDS astral_gui
        COMMENT "building a self-contained Astral.app in gui/dist"
        VERBATIM)
endif()

# ---------------------------------------------------------------- tests
enable_testing()
add_executable(astral_gui_theme_test tests/theme_test.cc theme/theme.cc)
set_target_properties(astral_gui_theme_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(astral_gui_theme_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(astral_gui_theme_test PRIVATE QT_NO_KEYWORDS)
target_link_libraries(astral_gui_theme_test PRIVATE Qt6::Widgets Qt6::Test)
add_test(NAME gui_theme COMMAND astral_gui_theme_test -platform offscreen)

# The document is the window's whole view of the engine, so it is checked
# against a real program rather than a stub. Any small system binary will do.
if(NOT DEFINED ASTRAL_GUI_TEST_BINARY)
    if(APPLE)
        set(ASTRAL_GUI_TEST_BINARY /bin/echo)
    else()
        set(ASTRAL_GUI_TEST_BINARY /bin/true)
    endif()
endif()
add_executable(astral_gui_document_test tests/document_test.cc
               model/programdocument.cc model/functionlistmodel.cc)
set_target_properties(astral_gui_document_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(astral_gui_document_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(astral_gui_document_test PRIVATE QT_NO_KEYWORDS
    ASTRAL_TEST_BINARY="${ASTRAL_GUI_TEST_BINARY}")
target_link_libraries(astral_gui_document_test PRIVATE Qt6::Widgets Qt6::Test astral_static)
add_test(NAME gui_document COMMAND astral_gui_document_test -platform offscreen)

# The project store is checked without the engine: a manifest, a database and
# the records that go through them are all it touches.
add_executable(astral_gui_project_test tests/project_test.cc model/project.cc)
set_target_properties(astral_gui_project_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(astral_gui_project_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(astral_gui_project_test PRIVATE QT_NO_KEYWORDS)
target_link_libraries(astral_gui_project_test PRIVATE Qt6::Widgets Qt6::Test astral_gui_sqlite)
add_test(NAME gui_project COMMAND astral_gui_project_test -platform offscreen)

# The assembler's text-to-bytes path: rewriting a listing, running the system
# assembler, and refusing a block that does not fit.

# The hex view's byte model: what typing does to it and what it refuses.
add_executable(astral_gui_hexedit_test tests/hexedit_test.cc views/hexview.cc
               views/codeview.cc theme/theme.cc)
set_target_properties(astral_gui_hexedit_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(astral_gui_hexedit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(astral_gui_hexedit_test PRIVATE QT_NO_KEYWORDS)
target_link_libraries(astral_gui_hexedit_test PRIVATE Qt6::Widgets Qt6::Test)
add_test(NAME gui_hexedit COMMAND astral_gui_hexedit_test -platform offscreen)

add_executable(astral_gui_patch_test tests/patch_test.cc model/patchbuilder.cc model/literalspace.cc model/programdocument.cc)
set_target_properties(astral_gui_patch_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(astral_gui_patch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(astral_gui_patch_test PRIVATE QT_NO_KEYWORDS)
# The static library, so the test never picks up an installed copy.
target_link_libraries(astral_gui_patch_test PRIVATE Qt6::Widgets Qt6::Test astral_static)
add_test(NAME gui_patch COMMAND astral_gui_patch_test -platform offscreen)

# The project store is checked without the engine: a manifest, a database and
# the records that go through them are all it touches.

add_executable(astral_gui_assemble_test tests/assemble_test.cc model/assembler.cc
               model/patchbuilder.cc model/literalspace.cc model/programdocument.cc)
set_target_properties(astral_gui_assemble_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(astral_gui_assemble_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(astral_gui_assemble_test PRIVATE QT_NO_KEYWORDS)
target_link_libraries(astral_gui_assemble_test PRIVATE Qt6::Widgets Qt6::Test astral_static)
add_test(NAME gui_assemble COMMAND astral_gui_assemble_test -platform offscreen)

# The hex view's byte model: what typing does to it and what it refuses.

add_executable(astral_gui_patchflow tests/patchflow_test.cc model/patchbuilder.cc model/literalspace.cc model/programdocument.cc)
set_target_properties(astral_gui_patchflow PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(astral_gui_patchflow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(astral_gui_patchflow PRIVATE QT_NO_KEYWORDS)
target_link_libraries(astral_gui_patchflow PRIVATE Qt6::Core astral_static)

add_executable(astral_gui_sourcepatch_test tests/sourcepatch_test.cc
    model/sourcepatcher.cc model/literalspace.cc model/programdocument.cc)
set_target_properties(astral_gui_sourcepatch_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF AUTOMOC ON)
target_include_directories(astral_gui_sourcepatch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${ASTRAL_ENGINE_INCLUDE_DIRS})
target_compile_definitions(astral_gui_sourcepatch_test PRIVATE QT_NO_KEYWORDS)
target_link_libraries(astral_gui_sourcepatch_test PRIVATE Qt6::Widgets Qt6::Test astral_static)
add_test(NAME gui_sourcepatch COMMAND astral_gui_sourcepatch_test -platform offscreen)

# The whole loop, run for real: recover C, edit it, patch, write, run.
add_executable(astral_gui_sourcepatchflow tests/sourcepatchflow_test.cc
    model/sourcepatcher.cc model/literalspace.cc model/programdocument.cc)
set_target_properties(astral_gui_sourcepatchflow PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF AUTOMOC ON)
target_include_directories(astral_gui_sourcepatchflow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${ASTRAL_ENGINE_INCLUDE_DIRS})
target_compile_definitions(astral_gui_sourcepatchflow PRIVATE QT_NO_KEYWORDS)
target_link_libraries(astral_gui_sourcepatchflow PRIVATE Qt6::Core astral_static)
add_test(NAME gui_sourcepatchflow COMMAND astral_gui_sourcepatchflow)

add_executable(astral_gui_asmflow tests/asmflow_test.cc model/assembler.cc
               model/patchbuilder.cc model/literalspace.cc model/programdocument.cc)
set_target_properties(astral_gui_asmflow PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(astral_gui_asmflow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(astral_gui_asmflow PRIVATE QT_NO_KEYWORDS)
target_link_libraries(astral_gui_asmflow PRIVATE Qt6::Core astral_static)

# ---------------------------------------------------------------- install
#
# The single file is what someone else runs, so that is what goes into bin. The
# bundle goes beside it on macOS, where an application that is not a bundle
# cannot be opened from the Finder.
install(TARGETS astral_gui_bin RUNTIME DESTINATION bin)
if(APPLE)
    install(TARGETS astral_gui BUNDLE DESTINATION share/astral/applications)
endif()
