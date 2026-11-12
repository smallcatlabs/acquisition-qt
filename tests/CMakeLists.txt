# SPDX-License-Identifier: GPL-3.0-or-later

# Every executable that ctest runs, and nothing else: the sanitizer job
# (.github/workflows/sanitizers.yml) builds this target so that the
# `acquisition` app, which must never be built sanitized, is not built at all.
add_custom_target(acq_tests)

function(_acq_test_environment out_var)
    set(test_environment "QT_QPA_PLATFORM=offscreen")
    if(TARGET Qt6::QSQLiteDriverPlugin)
        list(APPEND test_environment
            "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QSQLiteDriverPlugin>/..")
    endif()
    set(${out_var} "${test_environment}" PARENT_SCOPE)
endfunction()

# On Windows the test executables need the Qt DLLs on PATH. CI gets this from
# install-qt-action; a plain local shell does not, and every test would fail
# with 0xc0000135 (DLL not found). Prepending is a no-op when Qt is already
# on PATH.
function(_acq_test_dll_path test_name)
    if(WIN32)
        set_property(TEST ${test_name} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
endfunction()

function(acq_add_test name)
    qt_add_executable(${name} ${name}.cpp)
    acq_first_party(${name})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${name} PRIVATE acquisition_core Qt6::Test)
    add_dependencies(acq_tests ${name})
    add_test(NAME ${name} COMMAND ${name})
    _acq_test_environment(test_environment)
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "${test_environment}")
    _acq_test_dll_path(${name})
endfunction()

# Build one runner executable and register each named scenario as its OWN CTest
# invocation (network-redesign phase 5, verification §6): each shutdown proof
# intentionally leaves detached QCoro frames and forbids any event-loop turn
# after owner destruction, so exactly one destructive scenario may run per
# process. The runner's custom main() forwards the single slot-name argument to
# QTest::qExec (and rejects any other count), so `COMMAND ${name} <scenario>`
# runs only that slot.
function(acq_add_scenario_test name)
    qt_add_executable(${name} ${name}.cpp)
    acq_first_party(${name})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${name} PRIVATE acquisition_core Qt6::Test)
    add_dependencies(acq_tests ${name})
    _acq_test_environment(test_environment)
    foreach(scenario ${ARGN})
        add_test(NAME ${name}.${scenario} COMMAND ${name} ${scenario})
        set_tests_properties(${name}.${scenario} PROPERTIES
            ENVIRONMENT "${test_environment}")
        _acq_test_dll_path(${name}.${scenario})
    endforeach()
endfunction()

# Hand-run measurement harnesses, not tests: ctest does not run them, but the
# default build compiles them, so a change that breaks one fails the build
# instead of rotting unseen. They reuse the test fakes and the deterministic
# dataset generator, so they live here. Run them by hand in a Release build.
# Not on Windows: they measure with POSIX calls (getrusage) and have never
# been built there, so the Windows build leaves them out of `all`; the Linux
# pull-request job and a macOS build still compile them.
#   m2m2_benchmark: the M2-M2 harness (items-pipeline M2, stage 4).
#   m3_holdpoint_benchmark: the M3 hold-point harness (items-pipeline M3,
#     working rule 3): the conditional budget rows at S3-S5 and the S7 gate's
#     model-side rows, with the spike presets.
#   m1m2_benchmark: the M1-M2 harness (items-pipeline M2, D10): the
#     batch-submit QueueUpdated burst vs status-widget frame time.
foreach(benchmark m2m2_benchmark m3_holdpoint_benchmark m1m2_benchmark)
    qt_add_executable(${benchmark} ${benchmark}.cpp)
    acq_first_party(${benchmark})
    target_include_directories(${benchmark} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${benchmark} PRIVATE acquisition_core)
    if(WIN32)
        set_target_properties(${benchmark} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()

# The mod-match census (#265, #296): runs the mod matching over real RePoE
# stat translation files named on the command line. Not a test; build
# explicitly with --target modmatch_census.
qt_add_executable(modmatch_census EXCLUDE_FROM_ALL modmatch_census.cpp)
target_include_directories(modmatch_census PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(modmatch_census PRIVATE acquisition_core)

# The filter core must stay free of the UI (Phase 5, D5). A STATIC archive has
# no link step, so this cannot be left to target_link_libraries.
add_test(NAME filters_boundary
    COMMAND ${CMAKE_COMMAND}
        -D "FILTERS_DIR=${acquisition_SOURCE_DIR}/src/filters"
        -D "FILTERS_ARCHIVE=$<TARGET_FILE:acquisition_filters>"
        -P "${acquisition_SOURCE_DIR}/cmake/filters_boundary_check.cmake")
add_dependencies(acq_tests acquisition_filters)

acq_add_test(tst_buyout)
acq_add_test(tst_buyoutmanager)
acq_add_test(tst_applybuyout)
acq_add_test(tst_effectiveprice)
acq_add_test(tst_buyoutpanel)
acq_add_test(tst_currencymanager)
acq_add_test(tst_itemlocation)
acq_add_test(tst_item)
acq_add_test(tst_legacydatastore)
acq_add_test(tst_sqlitedatastore)
acq_add_test(tst_legacybuyoutimporter)
acq_add_test(tst_qtassumptions)
acq_add_test(tst_qxlsx)
acq_add_test(tst_itemtooltiptext)
acq_add_test(tst_uismall)
acq_add_test(tst_itemsmodel)
acq_add_test(tst_mainwindow)
acq_add_test(tst_networkcapture)
acq_add_test(tst_oauthratelimitlogging)
acq_add_test(tst_poeapiclient)
acq_add_test(tst_ratelimitdialog)
acq_add_test(tst_ratelimitmanager)
acq_add_test(tst_ratelimitpolicy)
acq_add_test(tst_ratelimiter)
acq_add_test(tst_search)
acq_add_test(tst_searchform)
acq_add_test(tst_securitylogging)
acq_add_test(tst_shell)
acq_add_test(tst_shop)
acq_add_test(tst_spikedataset)
acq_add_test(tst_stopsleep)
acq_add_test(tst_timerscheduler)
acq_add_test(tst_gate)
acq_add_test(tst_filters)
acq_add_test(tst_foldertree)
acq_add_test(tst_reconcile)
acq_add_test(tst_refreshchecked)
acq_add_test(tst_repoe)
acq_add_test(tst_userstoremigration)
acq_add_test(tst_workerparse)
acq_add_test(tst_workerupdate)
acq_add_test(tst_wiretypes)

acq_add_scenario_test(tst_workerintegration
    fullChainStashListSucceeds
    i_cancel_pacing
    i_cancel_gate
    i_cancel_flight
    i_shut_pacing
    i_shut_gate
    i_shut_flight
    i_shut_retry
    i_leak_bound
    i_retention)
