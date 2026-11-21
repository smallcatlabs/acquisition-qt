# Enforces a library boundary: the sources of one library may not reach Qt
# Widgets or another library's headers. Two boundaries use it (see
# tests/CMakeLists.txt): the filter core (Phase 5, D5) and the core library
# (#299), neither of which may reach the UI.
#
# Two checks, because neither alone is sufficient:
#
#   1. Includes. The library links only non-widget Qt modules, but it is a
#      STATIC archive, so its "link" step only archives -- it never resolves
#      symbols and cannot reject anything. Whether a widget header even
#      compiles there depends on the Qt layout (unreachable with macOS
#      frameworks, reachable through the umbrella include dir on a normal Unix
#      install), so the include ban has to be checked explicitly rather than
#      assumed.
#
#   2. Undefined symbols. An include ban cannot see through a header that has
#      already been included elsewhere in the closure, so also assert the
#      archive asks the linker for no widget symbols. Skipped when nm is
#      unavailable (e.g. MSVC), where check 1 still applies.
#
# Every violation names the file and the include, or the archive member and
# the symbol.
#
# Invoked as a CTest test (cmake -P) with:
#   BOUNDARY        what is checked, for the report ("The filter core")
#   SOURCES_DIR     every .h and .cpp under this directory is checked, or
#   SOURCES_FILE    a file listing the sources to check, one path per line
#                   (relative paths are taken from the working directory)
#   SRC_DIR         the directory includes are relative to (src/), with
#   FORBIDDEN_FILE  optionally, a file listing headers of other libraries the
#                   sources may not include, one path per line
#   ARCHIVE         the library's archive, for check 2

# Widget headers, and the class names their symbols carry.
set(forbidden_headers
    QtWidgets
    QWidget
    QLayout
    QBoxLayout
    QVBoxLayout
    QHBoxLayout
    QGridLayout
    QFormLayout
    QLineEdit
    QComboBox
    QCheckBox
    QPushButton
    QAbstractButton
    QLabel
    QCompleter
    QAbstractItemView
    QApplication
    QMainWindow
    QDialog
    QMessageBox
    QTreeView
    QTreeWidget
    QStyle
)

if(DEFINED SOURCES_FILE)
    file(STRINGS "${SOURCES_FILE}" sources)
else()
    file(GLOB_RECURSE sources "${SOURCES_DIR}/*.h" "${SOURCES_DIR}/*.cpp")
endif()
if(NOT sources)
    message(FATAL_ERROR "${BOUNDARY} boundary check: no sources to check")
endif()

# Other libraries' headers, as an include names them: relative to SRC_DIR.
set(foreign_headers "")
if(DEFINED FORBIDDEN_FILE)
    file(STRINGS "${FORBIDDEN_FILE}" foreign_paths)
    foreach(path IN LISTS foreign_paths)
        # A relative path is taken from the working directory.
        get_filename_component(path "${path}" ABSOLUTE)
        file(RELATIVE_PATH relative "${SRC_DIR}" "${path}")
        list(APPEND foreign_headers "${relative}")
    endforeach()
endif()

set(violations "")

foreach(source IN LISTS sources)
    file(STRINGS "${source}" include_lines REGEX "^[ \t]*#[ \t]*include")
    foreach(line IN LISTS include_lines)
        if(NOT line MATCHES "#[ \t]*include[ \t]*[\"<]([^\">]+)[\">]")
            continue()
        endif()
        set(target "${CMAKE_MATCH_1}")
        set(forbidden FALSE)
        if(target MATCHES "^ui/")
            set(forbidden TRUE)
        endif()
        foreach(header IN LISTS forbidden_headers)
            if((target STREQUAL header) OR (target MATCHES "^${header}/"))
                set(forbidden TRUE)
            endif()
        endforeach()
        if(target IN_LIST foreign_headers)
            set(forbidden TRUE)
        endif()
        if(forbidden)
            list(APPEND violations "${source}: ${line}")
        endif()
    endforeach()
endforeach()

# A symbol names a class by its length and its name (Itanium mangling:
# 6QStyle), so QStyle is not found in QStyleHints, a Qt Gui class.
find_program(NM_EXECUTABLE NAMES nm llvm-nm)
if(NM_EXECUTABLE AND EXISTS "${ARCHIVE}")
    execute_process(
        COMMAND "${NM_EXECUTABLE}" -u "${ARCHIVE}"
        OUTPUT_VARIABLE undefined_symbols
        ERROR_VARIABLE nm_errors
        RESULT_VARIABLE nm_result
    )
    if(NOT nm_result EQUAL 0)
        message(WARNING "${BOUNDARY} boundary check: ${NM_EXECUTABLE} failed: ${nm_errors}")
    else()
        set(patterns "")
        foreach(header IN LISTS forbidden_headers)
            if(NOT header STREQUAL "QtWidgets")
                string(LENGTH "${header}" length)
                list(APPEND patterns "(^|[^0-9])${length}${header}")
            endif()
        endforeach()
        string(REPLACE "\n" ";" symbol_lines "${undefined_symbols}")
        set(member "${ARCHIVE}")
        foreach(symbol_line IN LISTS symbol_lines)
            string(STRIP "${symbol_line}" symbol_line)
            if(symbol_line MATCHES ":$")
                # The archive member the symbols below belong to.
                string(REGEX REPLACE ":$" "" member "${symbol_line}")
                continue()
            endif()
            string(REGEX REPLACE "^.*[ \t]" "" symbol "${symbol_line}")
            foreach(pattern IN LISTS patterns)
                if(symbol MATCHES "${pattern}")
                    list(APPEND violations "${member}: undefined symbol ${symbol}")
                endif()
            endforeach()
        endforeach()
    endif()
else()
    message(STATUS "${BOUNDARY} boundary check: nm unavailable, checking includes only")
endif()

if(violations)
    list(REMOVE_DUPLICATES violations)
    string(REPLACE ";" "\n  " report "${violations}")
    message(FATAL_ERROR "${BOUNDARY} must not reach the UI:\n  ${report}")
endif()

message(STATUS "${BOUNDARY} boundary check: OK")
