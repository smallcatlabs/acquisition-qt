# SPDX-License-Identifier: GPL-3.0-or-later

# The Ninja job pool the first-party sources compile in (#476).
#
# A translation unit of this project can need more than a gigabyte to compile
# (a test suite needs 0.7 GB on average and up to 1.4 GB; it was one to two
# before #476 cut what the sources include), and a library's or the test
# executable's units all become ready at the same moment. Ninja's default is one job per
# hardware thread plus two, so on a machine with many threads and ordinary
# memory those compiles together ask for more than the machine has.
# acq_first_party() puts every first-party target's compiles in this pool, and
# the pool's size follows memory, not threads. The fetched dependencies and
# the links stay outside it.
#
# ACQ_COMPILE_JOBS is the size. Left empty, it is worked out here: one job
# for every ACQ_COMPILE_GB (2) gigabytes of memory, where memory is the
# smaller of the machine's and, on Linux, the limit of the control group the
# configure step runs in (a container or a systemd scope). Set it when that
# guess is wrong for where the build runs: memory shared with other builds, a
# limit CMake cannot see, or a machine with room to spare.
#
#   cmake --preset dev -DACQ_COMPILE_JOBS=4
#
# Only Ninja has job pools; the other generators ignore this, and none of them
# starts a job per thread unasked.

set(ACQ_COMPILE_JOBS "" CACHE STRING
    "How many first-party translation units compile at once (Ninja). Empty: \
one for every 2 GB of memory.")
set(ACQ_COMPILE_GB 2)

# The memory limit of the control group this process is in, in megabytes, or
# an empty string when there is none to read (not Linux, cgroup v1, no limit).
function(_acq_cgroup_memory_mb out_var)
    set(${out_var} "" PARENT_SCOPE)
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR NOT EXISTS "/proc/self/cgroup")
        return()
    endif()
    file(STRINGS "/proc/self/cgroup" lines REGEX "^0::/")
    if(NOT lines)
        return()
    endif()
    list(GET lines 0 line)
    string(REGEX REPLACE "^0::" "" group "${line}")
    set(lowest "")
    # A limit on any ancestor binds as well, so read every level up to the root.
    while(TRUE)
        set(file "/sys/fs/cgroup${group}/memory.max")
        if(EXISTS "${file}")
            file(STRINGS "${file}" value LIMIT_COUNT 1)
            if(value MATCHES "^[0-9]+$")
                # Bytes can pass what math() holds on a 32-bit CMake, so drop
                # six digits first; then decimal megabytes to the binary ones
                # CMake reports the machine's memory in (1000000 / 1048576).
                string(LENGTH "${value}" digits)
                if(digits GREATER 6)
                    math(EXPR keep "${digits} - 6")
                    string(SUBSTRING "${value}" 0 ${keep} megabytes)
                    math(EXPR megabytes "${megabytes} * 15625 / 16384")
                else()
                    set(megabytes 1)
                endif()
                if(lowest STREQUAL "" OR megabytes LESS lowest)
                    set(lowest "${megabytes}")
                endif()
            endif()
        endif()
        if(group STREQUAL "" OR group STREQUAL "/")
            break()
        endif()
        get_filename_component(group "${group}" DIRECTORY)
    endwhile()
    set(${out_var} "${lowest}" PARENT_SCOPE)
endfunction()

if(ACQ_COMPILE_JOBS STREQUAL "")
    cmake_host_system_information(RESULT _acq_memory_mb QUERY TOTAL_PHYSICAL_MEMORY)
    _acq_cgroup_memory_mb(_acq_cgroup_mb)
    if(NOT _acq_cgroup_mb STREQUAL "" AND _acq_cgroup_mb LESS _acq_memory_mb)
        set(_acq_memory_mb "${_acq_cgroup_mb}")
    endif()
    math(EXPR _acq_compile_jobs "${_acq_memory_mb} / (${ACQ_COMPILE_GB} * 1024)")
    if(_acq_compile_jobs LESS 1)
        set(_acq_compile_jobs 1)
    endif()
    set(_acq_compile_source "${_acq_memory_mb} MB of memory")
elseif(ACQ_COMPILE_JOBS MATCHES "^[1-9][0-9]*$")
    set(_acq_compile_jobs "${ACQ_COMPILE_JOBS}")
    set(_acq_compile_source "ACQ_COMPILE_JOBS")
else()
    message(FATAL_ERROR
        "ACQ_COMPILE_JOBS must be a whole number of jobs, 1 or more, or empty; "
        "it is \"${ACQ_COMPILE_JOBS}\".")
endif()

if(CMAKE_GENERATOR MATCHES "Ninja")
    message(STATUS "First-party sources compile ${_acq_compile_jobs} at a time "
        "(${_acq_compile_source}); -DACQ_COMPILE_JOBS=<n> changes it.")
endif()
set_property(GLOBAL APPEND PROPERTY JOB_POOLS "acq_compile=${_acq_compile_jobs}")
