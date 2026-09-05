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

set(GUI_SOURCES
    app/main.cc
    app/mainwindow.cc
    app/titlebar.cc
    app/welcomepage.cc
    app/functionspane.cc
    app/tablepane.cc
    app/programtab.cc
    model/programdocument.cc
    model/functionlistmodel.cc
    model/patchbuilder.cc
    views/codeview.cc
    views/decompilerview.cc
    views/listingview.cc
    views/hexview.cc
    theme/theme.cc
    resources.qrc)
list(APPEND GUI_SOURCES platform/window.cc)
if(APPLE)
    enable_language(OBJCXX)
    list(APPEND GUI_SOURCES platform/window_mac.mm)
endif()

add_executable(astral_gui MACOSX_BUNDLE WIN32 ${GUI_SOURCES})
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
target_link_libraries(astral_gui PRIVATE Qt6::Widgets Qt6::Svg astral_shared)
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
target_link_libraries(astral_gui_document_test PRIVATE Qt6::Widgets Qt6::Test astral_shared)
add_test(NAME gui_document COMMAND astral_gui_document_test -platform offscreen)

add_executable(astral_gui_patch_test tests/patch_test.cc model/patchbuilder.cc model/programdocument.cc)
set_target_properties(astral_gui_patch_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(astral_gui_patch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(astral_gui_patch_test PRIVATE QT_NO_KEYWORDS)
# The static library, so the test never picks up an installed copy.
target_link_libraries(astral_gui_patch_test PRIVATE Qt6::Widgets Qt6::Test astral_static)
add_test(NAME gui_patch COMMAND astral_gui_patch_test -platform offscreen)
