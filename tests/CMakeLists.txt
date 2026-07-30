function(_acq_test_environment out_var)
    set(test_environment "QT_QPA_PLATFORM=offscreen")
    if(TARGET Qt6::QSQLiteDriverPlugin)
        list(APPEND test_environment
            "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QSQLiteDriverPlugin>/..")
    endif()
    set(${out_var} "${test_environment}" PARENT_SCOPE)
endfunction()

function(acq_add_test name)
    qt_add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${name} PRIVATE acquisition_core Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
    _acq_test_environment(test_environment)
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "${test_environment}")
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
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${name} PRIVATE acquisition_core Qt6::Test)
    _acq_test_environment(test_environment)
    foreach(scenario ${ARGN})
        add_test(NAME ${name}.${scenario} COMMAND ${name} ${scenario})
        set_tests_properties(${name}.${scenario} PROPERTIES
            ENVIRONMENT "${test_environment}")
    endforeach()
endfunction()

# M2-M2 measurement harness (items-pipeline M2, stage 4): not a test — an
# offscreen benchmark run by hand in a Release build. It reuses the test
# fakes and the deterministic dataset generator, so it lives here.
# Hand-run measurement harness (M2-M2), not a test: excluded from the
# default build; build explicitly with --target m2m2_benchmark.
qt_add_executable(m2m2_benchmark EXCLUDE_FROM_ALL m2m2_benchmark.cpp)
target_include_directories(m2m2_benchmark PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(m2m2_benchmark PRIVATE acquisition_core)

# SPIKE-ONLY (spike/m3-sort-profile, R3-4): the M3 sort-profiling harness.
# Never merged; build explicitly with --target m3_sort_benchmark.
qt_add_executable(m3_sort_benchmark EXCLUDE_FROM_ALL m3_sort_benchmark.cpp)
target_include_directories(m3_sort_benchmark PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(m3_sort_benchmark PRIVATE acquisition_core)

# The filter core must stay free of the UI (Phase 5, D5). A STATIC archive has
# no link step, so this cannot be left to target_link_libraries.
add_test(NAME filters_boundary
    COMMAND ${CMAKE_COMMAND}
        -D "FILTERS_DIR=${acquisition_SOURCE_DIR}/src/filters"
        -D "FILTERS_ARCHIVE=$<TARGET_FILE:acquisition_filters>"
        -P "${acquisition_SOURCE_DIR}/cmake/filters_boundary_check.cmake")

acq_add_test(tst_buyout)
acq_add_test(tst_buyoutmanager)
acq_add_test(tst_itemlocation)
acq_add_test(tst_itemtooltiptext)
acq_add_test(tst_itemsmodel)
acq_add_test(tst_mainwindow)
acq_add_test(tst_networkcapture)
acq_add_test(tst_poeapiclient)
acq_add_test(tst_ratelimitmanager)
acq_add_test(tst_ratelimitpolicy)
acq_add_test(tst_ratelimiter)
acq_add_test(tst_search)
acq_add_test(tst_searchform)
acq_add_test(tst_shop)
acq_add_test(tst_spikedataset)
acq_add_test(tst_stopsleep)
acq_add_test(tst_timerscheduler)
acq_add_test(tst_gate)
acq_add_test(tst_filters)
acq_add_test(tst_reconcile)
acq_add_test(tst_userstoremigration)
acq_add_test(tst_workerparse)
acq_add_test(tst_workerupdate)

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
