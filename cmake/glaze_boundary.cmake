# SPDX-License-Identifier: GPL-3.0-or-later

# Enforces the one way into glaze (src/util/json_glaze.h, #476).
#
# glaze reflects any struct it is given, so a file that includes glaze without
# the project's own declarations for a type (the glaze_meta.h files) compiles
# and quietly reads and writes other JSON. So:
#
#   1. only src/util/glaze_qt.h includes a glaze header;
#   2. only the glaze_meta.h files and src/util/json_glaze.h include
#      util/glaze_qt.h, and only json_glaze.h includes a glaze_meta.h;
#   3. no header includes util/json_glaze.h, which is too expensive to hand to
#      every file that includes the header.
#
# Every violation names the file, the line and the rule.
#
# Invoked as a CTest test (cmake -P) with:
#   SOURCE_DIR  the repository root; every .h and .cpp under src/ and tests/
#               is checked

file(GLOB_RECURSE sources RELATIVE "${SOURCE_DIR}"
    "${SOURCE_DIR}/src/*.h" "${SOURCE_DIR}/src/*.cpp"
    "${SOURCE_DIR}/tests/*.h" "${SOURCE_DIR}/tests/*.cpp")

set(violations "")
foreach(source ${sources})
    file(STRINGS "${SOURCE_DIR}/${source}" includes REGEX "^[ \t]*#[ \t]*include[ \t]")
    foreach(line ${includes})
        if(line MATCHES "[<\"]glaze/" AND NOT source STREQUAL "src/util/glaze_qt.h")
            list(APPEND violations
                "${source}: ${line}\n    only src/util/glaze_qt.h includes a glaze header")
        endif()
        if(line MATCHES "\"util/glaze_qt\\.h\""
           AND NOT source MATCHES "^src/(util/json_glaze|poe/types/glaze_meta|legacy/glaze_meta)\\.h$")
            list(APPEND violations
                "${source}: ${line}\n    util/glaze_qt.h is included through util/json_glaze.h")
        endif()
        if(line MATCHES "glaze_meta\\.h\"" AND NOT source STREQUAL "src/util/json_glaze.h")
            list(APPEND violations
                "${source}: ${line}\n    a glaze_meta.h is included through util/json_glaze.h")
        endif()
        if(line MATCHES "\"util/json_glaze\\.h\"" AND source MATCHES "\\.h$")
            list(APPEND violations
                "${source}: ${line}\n    util/json_glaze.h is included from .cpp files only")
        endif()
    endforeach()
endforeach()

if(violations)
    list(JOIN violations "\n  " report)
    message(FATAL_ERROR
        "The one way into glaze is src/util/json_glaze.h, from a .cpp file:\n  ${report}")
endif()
list(LENGTH sources count)
message(STATUS "glaze is reached only through util/json_glaze.h (${count} files checked).")
