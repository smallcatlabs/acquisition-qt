# SPDX-License-Identifier: GPL-3.0-or-later

# Every suite (tst_*.cpp) is compiled into this one executable, which runs the
# suite its first argument names: `acq_tests tst_search [Qt Test arguments]`
# (testsuite.h). It is every executable ctest runs, so the sanitizer job
# (.github/workflows/sanitizers.yml) builds this target, and the `acquisition`
# app, which must never be built sanitized, is not built at all.
qt_add_executable(acq_tests testrunner.cpp)
acq_first_party(acq_tests)
target_include_directories(acq_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(acq_tests PRIVATE acquisition_core Qt6::Test)

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

# One CTest entry, running acq_tests with the arguments given.
function(_acq_add_ctest test_name)
    add_test(NAME ${test_name} COMMAND acq_tests ${ARGN})
    _acq_test_environment(test_environment)
    set_tests_properties(${test_name} PROPERTIES
        ENVIRONMENT "${test_environment}")
    _acq_test_dll_path(${test_name})
endfunction()

# Adds the suite ${name}.cpp to acq_tests and registers it with CTest as
# ${name}; the file ends with its registration line (testsuite.h).
function(acq_add_test name)
    target_sources(acq_tests PRIVATE ${name}.cpp)
    _acq_add_ctest(${name} ${name})
endfunction()

# Adds a suite and registers each named scenario as its OWN CTest invocation
# (network-redesign phase 5, verification §6): each shutdown proof
# intentionally leaves detached QCoro frames and forbids any event-loop turn
# after owner destruction, so exactly one destructive scenario may run per
# process. The suite registers with ACQ_TEST_SCENARIO_MAIN, so the runner
# passes the single slot-name argument to QTest::qExec (and rejects any other
# count), and `acq_tests ${name} <scenario>` runs only that slot.
function(acq_add_scenario_test name)
    target_sources(acq_tests PRIVATE ${name}.cpp)
    foreach(scenario ${ARGN})
        _acq_add_ctest(${name}.${scenario} ${name} ${scenario})
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

# The live capture (#378): builds the real Application on a data directory in
# which the user has logged in and refreshes one league against the real API.
# Not a test: ctest never runs it, because it talks to the live API under the
# project's identity. The default build compiles it so it cannot rot unseen,
# on Windows too: the Windows pull-request job builds it (#379).
qt_add_executable(live_capture live_capture.cpp)
acq_first_party(live_capture)
target_include_directories(live_capture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(live_capture PRIVATE acquisition_core Qt6::Test)
# It looks at this source tree's branch before it runs (tests/liveguards.h).
target_compile_definitions(live_capture PRIVATE ACQ_SOURCE_DIR="${acquisition_SOURCE_DIR}")

# The mod-match census (#265, #296): runs the mod matching over real RePoE
# stat translation files named on the command line. Not a test; build
# explicitly with --target modmatch_census.
qt_add_executable(modmatch_census EXCLUDE_FROM_ALL modmatch_census.cpp)
target_include_directories(modmatch_census PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(modmatch_census PRIVATE acquisition_core)

# The game data the payload corpus needs (#246): cuts the excerpt in
# fixtures/payloads-repoe from a directory of RePoE files and shows that the
# excerpt gives the same items as the whole. Not a test: it needs the whole
# game data, which is not in the repository. The default build compiles it,
# as it does live_capture.
qt_add_executable(corpus_gamedata corpus_gamedata.cpp)
acq_first_party(corpus_gamedata)
target_include_directories(corpus_gamedata PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(corpus_gamedata PRIVATE acquisition_core)

# The filter core must stay free of the UI (Phase 5, D5). A STATIC archive has
# no link step, so this cannot be left to target_link_libraries.
add_test(NAME filters_boundary
    COMMAND ${CMAKE_COMMAND}
        -D "FILTERS_DIR=${acquisition_SOURCE_DIR}/src/filters"
        -D "FILTERS_ARCHIVE=$<TARGET_FILE:acquisition_filters>"
        -P "${acquisition_SOURCE_DIR}/cmake/filters_boundary_check.cmake")
add_dependencies(acq_tests acquisition_filters)

# The application finds a TLS backend that supports SSL (#370): a Qt whose
# TLS plugins load but cannot make an encrypted connection fails here, not at
# a user's start-up. The packaging workflows run the same check against the
# packaged binary. --check-tls exits before the data directory is touched.
# Not in a sanitized tree: that tree never builds the app (BUILD.md).
if(NOT ACQ_SANITIZE)
    add_test(NAME app_check_tls
        COMMAND acquisition --check-tls
            --data-dir "${CMAKE_CURRENT_BINARY_DIR}/app_check_tls-data")
    _acq_test_environment(test_environment)
    set_tests_properties(app_check_tls PROPERTIES
        ENVIRONMENT "${test_environment}"
        PASS_REGULAR_EXPRESSION "SSL supported: yes")
    _acq_test_dll_path(app_check_tls)

    # The version the binary reports is the one it was built as, pre-release
    # suffix included. Qt on Windows otherwise takes it from the executable's
    # numeric version resource, so a beta called itself "0.18.11.0" in --version
    # and at the head of the log. The message-box variable makes a Windows GUI
    # build print to stdout. --version exits before the data directory is
    # touched.
    string(REPLACE "." "\\." app_version_pattern "${app_version_string}")
    add_test(NAME app_version COMMAND acquisition --version)
    _acq_test_environment(test_environment)
    list(APPEND test_environment "QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES=1")
    set_tests_properties(app_version PROPERTIES
        ENVIRONMENT "${test_environment}"
        PASS_REGULAR_EXPRESSION " ${app_version_pattern}[ \t\r\n]*$")
    _acq_test_dll_path(app_version)
endif()

acq_add_test(tst_buyout)
acq_add_test(tst_buyoutmanager)
acq_add_test(tst_applybuyout)
acq_add_test(tst_effectiveprice)
acq_add_test(tst_buyoutpanel)
acq_add_test(tst_currencymanager)
acq_add_test(tst_currency)
acq_add_test(tst_itemlocation)
acq_add_test(tst_item)
acq_add_test(tst_legacydatastore)
acq_add_test(tst_liveguards)
acq_add_test(tst_sqlitedatastore)
acq_add_test(tst_legacybuyoutimporter)
acq_add_test(tst_qtassumptions)
acq_add_test(tst_qxlsx)
acq_add_test(tst_itemtooltiptext)
acq_add_test(tst_uismall)
acq_add_test(tst_itemsmodel)
acq_add_test(tst_mainwindow)
acq_add_test(tst_viewtranscript)
acq_add_test(tst_networkcapture)
acq_add_test(tst_oauthratelimitlogging)
acq_add_test(tst_payloads)
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
acq_add_test(tst_golden)
acq_add_test(tst_filters)
acq_add_test(tst_foldertree)
acq_add_test(tst_reconcile)
acq_add_test(tst_refreshchecked)
acq_add_test(tst_repoe)
acq_add_test(tst_userstoremigration)
acq_add_test(tst_workerparse)
acq_add_test(tst_workerupdate)
acq_add_test(tst_wiretypes)

# Logging in on the real Application and a scripted network (#380), one
# scenario per process (appfixture.h says why).
acq_add_scenario_test(tst_applogin
    storedTokenIsRefreshedAndLogsIn
    browserLoginStoresTheTokenAndLogsIn
    rejectedStoredTokenIsDroppedAndSaid
    unreachableServerKeepsTheStoredToken
    serverErrorKeepsTheStoredToken
    otherRefusalKeepsTheStoredToken
    storedLoginRefusedWhileTheBrowserIsOut
    refreshRejectedDuringARunIsLeftAlone
    failedReauthenticationKeepsTheLogin
    redirectWithWrongStateEndsTheAttempt
    deniedInTheBrowserIsShown
    refusedCodeExchangeIsShown
    failedCodeExchangeIsShown
    refusedReauthenticationKeepsTheLogin
    secondAuthenticateReplacesTheFirst
    redirectForAnAbandonedAttemptIsRefused
    loginEndsAWaitingBrowserLogin
    refusedLoginLeavesTheBrowserLoginWaiting)

# Which tabs are open once a filter has come and gone (#387), on the real
# Application; one scenario per process, as tst_applogin.
acq_add_scenario_test(tst_appexpansion
    tabsOpenedWithExpandAllReturnAfterAFilter
    tabsOpenedOneByOneReturnAfterAFilter
    changesUnderAFilterStayWithTheFilter
    openedTabsReturnAfterByItem)

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

# The real Application on a scripted network (#377): the release smoke list
# as scenarios, one per process (appfixture.h says why).
acq_add_scenario_test(tst_appsmoke
    startsWithCachedItems
    aLiveRunsRecordIsWhatTheWindowShows
    categoryFilterWorksAfterAnEarlyLogin
    refreshCheckedKeepsTheView
    refreshSelectedKeepsTheView
    typedPriceIsSavedOnEnter
    notePriceOverridesAndGivesWay
    tabNamePriceOverridesAndGivesWay
    pricedFilterFollowsPrices
    numberFieldsTakeACommaAndRefuseAPercent
    manualShopUpdatePostsTheUsersPrices
    automaticShopUpdateWaitsForACleanRefresh)
