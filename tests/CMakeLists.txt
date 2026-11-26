# SPDX-License-Identifier: GPL-3.0-or-later

# The test support libraries: the fixtures the suites, the benchmarks and the
# hand-run tools share, compiled once (#476). A fixture's header declares it
# and its .cpp file here defines it, so that a suite that uses a fixture does
# not compile the fixture's body, or the JSON library under it, again.
#
# There are two, as there are two libraries under test: acq_testsupport_core
# reaches no further than acquisition_core, and the suites that need no widget
# are compiled against it alone (acq_add_test(... GUILESS) below);
# acq_testsupport adds the fixtures that build windows.
qt_add_library(acq_testsupport_core STATIC
    testfixtures.cpp
    testfixtures.h
    transcriptchanges.cpp
    transcriptchanges.h
)
target_link_libraries(acq_testsupport_core PUBLIC acquisition_core Qt6::Test)

qt_add_library(acq_testsupport STATIC
    viewtranscript.cpp
    viewtranscript.h
)
target_link_libraries(acq_testsupport PUBLIC acq_testsupport_core acquisition_gui)

foreach(library acq_testsupport_core acq_testsupport)
    acq_first_party(${library})
    target_include_directories(${library} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()

# Every suite (tst_*.cpp) is compiled into this one executable, which runs the
# suite its first argument names: `acq_tests tst_search [Qt Test arguments]`
# (testsuite.h). It is every executable ctest runs, so the sanitizer job
# (.github/workflows/sanitizers.yml) builds this target, and the `acquisition`
# app, which must never be built sanitized, is not built at all.
qt_add_executable(acq_tests testrunner.cpp)
acq_first_party(acq_tests)
target_link_libraries(acq_tests PRIVATE acq_testsupport)

# The suites that need no widget (GUILESS below) are compiled here, against
# the core library alone, and their objects go into acq_tests. Qt Widgets is
# not among what this target links, so its headers are not on these suites'
# include path and <QTest> leaves its widget support out: such a suite is
# cheaper to compile, and cannot come to need a window unnoticed.
qt_add_library(acq_tests_guiless OBJECT)
acq_first_party(acq_tests_guiless)
target_link_libraries(acq_tests_guiless PRIVATE acq_testsupport_core)
target_sources(acq_tests PRIVATE $<TARGET_OBJECTS:acq_tests_guiless>)

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

# Adds a suite's source: to acq_tests_guiless when the suite was added with
# GUILESS, to acq_tests otherwise. Sets ${out_args} to the arguments left.
function(_acq_add_suite_source name out_args)
    set(arguments ${ARGN})
    if("GUILESS" IN_LIST arguments)
        list(REMOVE_ITEM arguments GUILESS)
        target_sources(acq_tests_guiless PRIVATE ${name}.cpp)
    else()
        target_sources(acq_tests PRIVATE ${name}.cpp)
    endif()
    set(${out_args} "${arguments}" PARENT_SCOPE)
endfunction()

# Adds the suite ${name}.cpp to acq_tests and registers it with CTest as
# ${name}; the file ends with its registration line (testsuite.h). GUILESS
# after the name is for a suite that registers with ACQ_TEST_GUILESS_MAIN or
# ACQ_TEST_SCENARIO_MAIN and includes no widget: it is compiled without Qt
# Widgets (acq_tests_guiless above).
function(acq_add_test name)
    _acq_add_suite_source(${name} unused ${ARGN})
    _acq_add_ctest(${name} ${name})
endfunction()

# Adds a suite and registers each named scenario as its OWN CTest invocation
# (network-redesign phase 5, verification §6): each shutdown proof
# intentionally leaves detached QCoro frames and forbids any event-loop turn
# after owner destruction, so exactly one destructive scenario may run per
# process. The suite registers with ACQ_TEST_SCENARIO_MAIN, so the runner
# passes the single slot-name argument to QTest::qExec (and rejects any other
# count), and `acq_tests ${name} <scenario>` runs only that slot. GUILESS is
# as for acq_add_test.
function(acq_add_scenario_test name)
    _acq_add_suite_source(${name} scenarios ${ARGN})
    foreach(scenario ${scenarios})
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
    target_link_libraries(${benchmark} PRIVATE acq_testsupport)
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
target_link_libraries(live_capture PRIVATE acq_testsupport)
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
# The GUI library for the tooltip text (ui/itemtooltiptext), which is free
# of widgets but kept with the rest of src/ui.
target_link_libraries(corpus_gamedata PRIVATE acq_testsupport)

# The filter core must stay free of the UI (Phase 5, D5). A STATIC archive has
# no link step, so this cannot be left to target_link_libraries.
add_test(NAME filters_boundary
    COMMAND ${CMAKE_COMMAND}
        "-DBOUNDARY=The filter core (Phase 5, D5)"
        -D "SOURCES_DIR=${acquisition_SOURCE_DIR}/src/filters"
        -D "ARCHIVE=$<TARGET_FILE:acquisition_filters>"
        -P "${acquisition_SOURCE_DIR}/cmake/boundary_check.cmake")
add_dependencies(acq_tests acquisition_filters)

# The core library must stay free of the UI as well (#299): its sources may
# not include a widget header or a header of the GUI library, and its archive
# asks for no widget symbols. The lists are the two targets' own sources.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/core_boundary_sources.txt"
    CONTENT "$<JOIN:$<FILTER:$<TARGET_PROPERTY:acquisition_core,SOURCES>,INCLUDE,^src/.*\\.(h|cpp)$>,\n>\n")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/gui_headers.txt"
    CONTENT "$<JOIN:$<FILTER:$<TARGET_PROPERTY:acquisition_gui,SOURCES>,INCLUDE,^src/.*\\.h$>,\n>\n")
add_test(NAME core_boundary
    COMMAND ${CMAKE_COMMAND}
        "-DBOUNDARY=The core library (#299)"
        -D "SOURCES_FILE=${CMAKE_BINARY_DIR}/core_boundary_sources.txt"
        -D "SRC_DIR=${acquisition_SOURCE_DIR}/src"
        -D "FORBIDDEN_FILE=${CMAKE_BINARY_DIR}/gui_headers.txt"
        -D "ARCHIVE=$<TARGET_FILE:acquisition_core>"
        -P "${acquisition_SOURCE_DIR}/cmake/boundary_check.cmake"
    WORKING_DIRECTORY "${acquisition_SOURCE_DIR}")
add_dependencies(acq_tests acquisition_core)

# glaze is reached only through util/json_glaze.h, and only from .cpp files
# (#476): a file that went around it would compile and use other JSON keys.
add_test(NAME glaze_boundary
    COMMAND ${CMAKE_COMMAND}
        -D "SOURCE_DIR=${acquisition_SOURCE_DIR}"
        -P "${acquisition_SOURCE_DIR}/cmake/glaze_boundary.cmake")

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

acq_add_test(tst_buyout GUILESS)
acq_add_test(tst_buyoutmanager GUILESS)
acq_add_test(tst_applybuyout GUILESS)
acq_add_test(tst_effectiveprice)
acq_add_test(tst_buyoutpanel)
acq_add_test(tst_currencymanager)
acq_add_test(tst_currency)
acq_add_test(tst_itemlocation GUILESS)
acq_add_test(tst_item GUILESS)
acq_add_test(tst_legacydatastore GUILESS)
acq_add_test(tst_liveguards GUILESS)
acq_add_test(tst_sqlitedatastore GUILESS)
acq_add_test(tst_legacybuyoutimporter GUILESS)
acq_add_test(tst_qtassumptions)
acq_add_test(tst_qxlsx GUILESS)
acq_add_test(tst_itemtooltiptext GUILESS)
acq_add_test(tst_uismall)
acq_add_test(tst_itemsmodel)
acq_add_test(tst_mainwindow)
acq_add_test(tst_viewtranscript)
acq_add_test(tst_networkcapture GUILESS)
acq_add_test(tst_oauthratelimitlogging GUILESS)
acq_add_test(tst_payloads GUILESS)
acq_add_test(tst_poeapiclient GUILESS)
acq_add_test(tst_ratelimitdialog)
acq_add_test(tst_ratelimitmanager GUILESS)
acq_add_test(tst_ratelimitpolicy GUILESS)
acq_add_test(tst_ratelimiter GUILESS)
acq_add_test(tst_search)
acq_add_test(tst_searchform)
acq_add_test(tst_securitylogging GUILESS)
acq_add_test(tst_oauthmanager)
acq_add_test(tst_shell)
acq_add_test(tst_shop)
acq_add_test(tst_spikedataset GUILESS)
acq_add_test(tst_stopsleep GUILESS)
acq_add_test(tst_timerscheduler GUILESS)
acq_add_test(tst_gate GUILESS)
acq_add_test(tst_golden GUILESS)
acq_add_test(tst_filters GUILESS)
acq_add_test(tst_foldertree)
acq_add_test(tst_reconcile GUILESS)
acq_add_test(tst_refreshchecked GUILESS)
acq_add_test(tst_repoe GUILESS)
acq_add_test(tst_userstoremigration)
acq_add_test(tst_workerparse GUILESS)
acq_add_test(tst_workerupdate GUILESS)
acq_add_test(tst_wiretypes GUILESS)

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
    refusedLoginLeavesTheBrowserLoginWaiting
    refreshWhileTheBrowserIsOutLeavesItWaiting
    failedRefreshWhileTheBrowserIsOutLeavesItWaiting
    loginDuringTheCodeExchangeKeepsTheSession
    secondAuthenticateDuringTheCodeExchange
    staleRefreshLeavesANewerLoginAlone
    redirectDuringARefreshIsTaken
    replayedRedirectIsNotExchangedAgain
    tokensInARedirectAreNotTaken
    heldLoginIsRefreshedOnTimeAfterAnAbandonedExchange
    exchangeWithNoAnswerEndsTheAttempt
    refreshWithNoAnswerAtStartIsSaid
    codeWithEscapesIsExchangedAsSent
    refreshWithoutARefreshTokenKeepsTheOneHeld
    refreshThatFailsIsTriedAgain)

# Which tabs are open once a filter has come and gone (#387), on the real
# Application; one scenario per process, as tst_applogin.
acq_add_scenario_test(tst_appexpansion
    tabsOpenedWithExpandAllReturnAfterAFilter
    tabsOpenedOneByOneReturnAfterAFilter
    changesUnderAFilterStayWithTheFilter
    openedTabsReturnAfterByItem)

acq_add_scenario_test(tst_workerintegration GUILESS
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
