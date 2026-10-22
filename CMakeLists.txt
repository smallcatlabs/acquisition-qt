# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2024 Tom Holz

cmake_minimum_required(VERSION 3.24)

# NOTE: For automatic update detection to work, the 'app_version' variable
# defined below should match a tag in github. Historically, the github tags
# have been identical to the 'app_version' as defined below, except the 
# tags on github are prefixed with a 'v'.

project(acquisition
    VERSION 0.18.8
    DESCRIPTION "Stash and forum shop management for Path of Exile (TM)"
    HOMEPAGE_URL "https://github.com/smallcatlabs/acquisition-qt"
    LANGUAGES CXX
)

include(CTest)
include(GNUInstallDirs)

# Use this to define pre-releases
set(app_version_suffix "")

# Variables used in version_defines.h and platforn-specific files and properties.
set(app_name            "${CMAKE_PROJECT_NAME}")
set(app_version         "${CMAKE_PROJECT_VERSION}")
set(app_version_string  "${CMAKE_PROJECT_VERSION}${app_version_suffix}")
set(app_url             "${CMAKE_PROJECT_HOMEPAGE_URL}")
set(app_publisher       "GERWARIC")
set(app_publisher_email "gerwaric@gmail.com")
set(app_copyright       "Copyright (C) 2014-2026 Ilya Zhuravlev and other Acquisition Contributors")
set(app_trademark       "Path of Exile is a trademark of Grinding Gear Games, Ltd.")

#------------------------------------------------------
# Version Checks
#------------------------------------------------------

# Enforce semantic versioning 2.0 per https://semver.org/spec/v2.0.0.html

if(acquisition_VERSION_MINOR STREQUAL "")
    message(FATAL_ERROR "Minor version is missing: ${acquisition_VERSION}")
endif()

if(acquisition_VERSION_PATCH STREQUAL "")
    message(FATAL_ERROR "Patch version is missing: ${acquisition_VERSION}")
endif()

if(NOT acquisition_VERSION_TWEAK STREQUAL "")
    message(FATAL_ERROR "Tweak version cannot be present: ${acquisition_VERSION}")
endif()

# Do extra careful checking of the pre-release suffix when it's present.
if(NOT app_version_suffix STREQUAL "")

    # Check for the leading hyphen.
    if(NOT app_version_suffix MATCHES "^-")
        message(FATAL_ERROR "Pre-release suffix does not start with a hyphen: '${app_version_suffix}'")
    endif()

    # Split into groups after the hyphen into dot-separated parts.
    string(SUBSTRING "${app_version_suffix}" 1 -1 _parts)
    string(REPLACE "." ";" _parts "${_parts}")

    # Check each part.
    foreach(id IN LISTS _parts)

        # Identifiers cannot be empty.
        if(id STREQUAL "")
            message(FATAL_ERROR "Pre-release suffix contains and empty identifier in '${app_version_suffix}'")
        endif()

        # Numeric identifiers cannot have leading zeros.
        if(id MATCHES "^0[0-9]+$")
            message(FATAL_ERROR "Numeric identifier has leading zero: '${id}' in '${app_version_suffix}'")
        endif()

        # Check the identifier against allowable characters.
        if(NOT id MATCHES "^[0-9A-Za-z-]+$")
            message(FATAL_ERROR "Invalid pre-release identifier '${id}' in '${app_version_suffix}'")
        endif()

    endforeach()

    # Get rid of the temporary variable.
    unset(_parts)

endif()

#------------------------------------------------------
# Compiler Floors
#------------------------------------------------------

# The coroutine code (QCoro and the ratelimit primitives) needs more than
# CXX_STANDARD 23, which does not enforce a compiler version (network-redesign
# spec, ER9). GCC below 13 miscompiles coroutines with captures in temporary
# lambdas; the Clang, AppleClang, and MSVC minimums are the ones QCoro v0.13.0
# documents. BUILD.md states the same floors.

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
        message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is not supported: "
            "acquisition requires GCC 13 or newer (earlier GCC miscompiles "
            "coroutines with captures in temporary lambdas).")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
        message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is not supported: "
            "acquisition requires Clang 15 or newer (QCoro v0.13.0 floor).")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
        message(FATAL_ERROR "AppleClang ${CMAKE_CXX_COMPILER_VERSION} is not supported: "
            "acquisition requires AppleClang 15 or newer / Xcode 15.2 (QCoro v0.13.0 floor).")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.40)
        message(FATAL_ERROR "MSVC ${CMAKE_CXX_COMPILER_VERSION} is not supported: "
            "acquisition requires MSVC 19.40 / Visual Studio 2022 17.10 or newer "
            "(QCoro v0.13.0 floor).")
    endif()
else()
    message(WARNING "Unrecognized compiler '${CMAKE_CXX_COMPILER_ID}': the coroutine "
        "compiler floors (see BUILD.md) cannot be checked.")
endif()

#------------------------------------------------------
# MSVC runtime compatibility
#------------------------------------------------------

# MSVC 14.38 (VS 2022 17.8) made std::mutex's constructor constexpr, which
# leaves the mutex's storage zero-initialized instead of set up by
# _Mtx_init_in_situ. An older msvcp140.dll on the user's machine does not
# understand that representation, and its mtx_do_lock dereferences a null
# handle: an access violation reading 0x0 on the process's very first lock.
# Acquisition hit this in the wild on 0.18.3 (a Windows 10 box whose system
# CRT dated from February 2021), crashing inside spdlog's registry mutex
# before logging::init() had returned.
#
# _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR restores the old non-constexpr
# constructor, so the binary works against any 14.x runtime rather than
# requiring users to have a current redistributable.
#
# This MUST be global, and MUST stay ahead of the FetchContent block below.
# std::mutex's constructor is inline, so a mutex inside an inline function or
# a header-only library (spdlog's registry singleton, for one) is emitted as a
# COMDAT by every translation unit that uses it. Defining the macro for only
# some targets makes those definitions disagree and the linker keeps whichever
# one it likes - an ODR violation that reintroduces exactly this crash. The
# fetched dependencies are compiled into the same binaries, so they need it
# too; add_compile_definitions() applies to C as well as C++, which is what
# sentry-native and crashpad want.
add_compile_definitions(
    $<$<OR:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>:_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR>
)

#------------------------------------------------------
# Sanitizers (test/CI only)
#------------------------------------------------------

# ACQ_SANITIZE builds a separate tree with a runtime sanitizer for the checked-in
# test suite. It is a TEST/CI-ONLY knob: it is OFF by default, and release
# packaging (build-{linux,macos,windows}.yml, installer.iss) never sets it, so
# shipped binaries are never sanitized. Configure a throwaway tree and build only
# test targets, e.g.:
#
#     cmake -S . -B build-asan -DACQ_SANITIZE=address \
#         -DCMAKE_BUILD_TYPE=RelWithDebInfo
#     cmake --build build-asan --target tst_workerintegration
#     ASAN_OPTIONS=abort_on_error=1 ctest --test-dir build-asan \
#         -R tst_workerintegration --output-on-failure
#
# Do NOT build/run the `acquisition` app target here: it links crashpad, whose
# signal handlers are only installed by sentry_init() in src/main.cpp (never in
# the test binaries), so the app under ASAN can conflict with the sanitizer.
#
# On macOS ASAN does no leak detection (LSAN is unavailable); this knob is for
# use-after-free coverage. Leak-boundedness (I-LEAK-BOUND,
# network-redesign-phase5-verification.md §6) is enforced on Linux CI by
# .github/workflows/sanitizers.yml, which builds only the integration runner
# with this knob and runs it under LeakSanitizer.
set(ACQ_SANITIZE "" CACHE STRING
    "Sanitizer for the test suite (test/CI only): address, undefined, or a \
comma list. Empty (default) disables it. Never set for release builds.")

if(ACQ_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "ACQ_SANITIZE is only wired for Clang/GCC toolchains.")
    endif()
    message(STATUS "Building the test suite with -fsanitize=${ACQ_SANITIZE} "
        "(test/CI only; shipped binaries are never sanitized).")
    add_compile_options(-fsanitize=${ACQ_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${ACQ_SANITIZE})
endif()

#------------------------------------------------------
# External Dependencies
#------------------------------------------------------

include(FetchContent)

if(NOT TARGET sentry-native)
    FetchContent_Declare(
        sentry-native
        GIT_REPOSITORY https://github.com/getsentry/sentry-native
        GIT_TAG        0.17.4
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(sentry-native)

    # Sentry causes some compiler warnings we want to ignore. Sentry sets its own
    # warning flags on this target (/W4 on MSVC, -Werror -Wall on gcc/clang), and
    # target_compile_options only appends, which makes MSVC emit D9025 for every
    # sentry translation unit. Replace the target's options instead of adding to
    # them; everything sentry puts there is warning-related.
    set_property(TARGET sentry PROPERTY COMPILE_OPTIONS
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>
        $<$<C_COMPILER_ID:MSVC>:/w>
        $<$<CXX_COMPILER_ID:MSVC>:/w>
    )
endif()

if(NOT TARGET glaze)
    FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG        v8.4.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(glaze)
endif()

if(NOT TARGET semver)
    FetchContent_Declare(
        semver
        GIT_REPOSITORY https://github.com/z4kn4fein/cpp-semver
        GIT_TAG        v0.4.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(semver)
endif()

if(NOT TARGET QCoro6::Core)
    # QCoro v0.13.0 is a hard floor, pinned exactly (network-redesign spec,
    # "Dependency: QCoro"): the semantics the shutdown contract leans on are
    # spike-verified at 0.13 exactly, and the FetchContent include-path fix
    # landed in that release. spikes/qcoro/CMakeLists.txt is the verified
    # integration reference for the flags below.
    #
    # FetchContent hygiene (spike-verified S1-10): QCoro's examples default ON
    # and its tests follow BUILD_TESTING, which this project enables globally.
    # QCORO_BUILD_TESTING maps onto a directory-scoped BUILD_TESTING of QCoro's
    # own, so the parent flag — and acquisition's own test suite — stay
    # intact. Never force the global BUILD_TESTING off here.
    set(QCORO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(QCORO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(QCORO_WITH_QTDBUS OFF CACHE BOOL "" FORCE)
    set(QCORO_WITH_QTWEBSOCKETS OFF CACHE BOOL "" FORCE)
    set(QCORO_WITH_QTQUICK OFF CACHE BOOL "" FORCE)
    set(QCORO_WITH_QML OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        qcoro
        GIT_REPOSITORY https://github.com/qcoro/qcoro.git
        GIT_TAG        v0.13.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(qcoro)
endif()

if(NOT TARGET spdlog)
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(spdlog)
endif()

if(NOT TARGET QXlsx::QXlsx)
    FetchContent_Declare(
        qxlsx
        GIT_REPOSITORY https://github.com/QtExcel/QXlsx.git
        GIT_TAG        v1.5.1.1
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  QXlsx
    )
    FetchContent_MakeAvailable(qxlsx)
endif()

#------------------------------------------------------
# Acquisition Sources
#------------------------------------------------------

set(ACQ_CORE
    src/application.cpp
    src/application.h
    src/bucket.cpp
    src/bucket.h
    src/buyout.cpp
    src/buyout.h
    src/buyoutmanager.cpp
    src/buyoutmanager.h
    src/column.cpp
    src/column.h
    src/sortkey.h
    src/currency.cpp
    src/currency.h
    src/currencymanager.cpp
    src/currencymanager.h
    src/imagecache.cpp
    src/imagecache.h
    src/influence.cpp
    src/influence.h
    src/item.cpp
    src/item.h
    src/itemcategories.cpp
    src/itemcategories.h
    src/itemconstants.cpp
    src/itemconstants.h
    src/itemlocation.cpp
    src/itemlocation.h
    src/items_model.cpp
    src/items_model.h
    src/modelprobes.h
    src/itemsmanager.cpp
    src/itemsmanager.h
    src/itemsmanagerworker.cpp
    src/itemsmanagerworker.h
    src/modlist.cpp
    src/modlist.h
    src/pseudomods.cpp
    src/pseudomods.h
    src/replytimeout.h
    src/search.cpp
    src/search.h
    src/shop.cpp
    src/shop.h
)

set(ACQ_FILTERS
    src/filters/filtermatchers.cpp
    src/filters/filtermatchers.h
    src/filters/filterstate.cpp
    src/filters/filterstate.h
    src/filters/filterspec.cpp
    src/filters/filterspec.h
)

set(ACQ_DATASTORE
    src/datastore/buyoutrepo.cpp
    src/datastore/buyoutrepo.h
    src/datastore/characterrepo.cpp
    src/datastore/characterrepo.h
    src/datastore/datastore.cpp
    src/datastore/datastore.h
    src/datastore/datastore_utils.cpp
    src/datastore/datastore_utils.h
    src/datastore/sqlitedatastore.cpp
    src/datastore/sqlitedatastore.h
    src/datastore/stashrepo.cpp
    src/datastore/stashrepo.h
    src/datastore/userstore.cpp
    src/datastore/userstore.h
)

set(ACQ_LEGACY
    src/legacy/legacybuyout.h
    src/legacy/legacybuyoutimporter.cpp
    src/legacy/legacybuyoutimporter.h
    src/legacy/legacycharacter.h
    src/legacy/legacycurrency.h
    src/legacy/legacydatastore.cpp
    src/legacy/legacydatastore.h
    src/legacy/legacyitem.cpp
    src/legacy/legacyitem.h
    src/legacy/legacystash.h
)

set(ACQ_RATELIMIT
    src/ratelimit/fetcherror.h
    src/ratelimit/gate.cpp
    src/ratelimit/gate.h
    src/ratelimit/networkcapture.cpp
    src/ratelimit/networkcapture.h
    src/ratelimit/ratelimit.cpp
    src/ratelimit/ratelimit.h
    src/ratelimit/ratelimitdialog.cpp
    src/ratelimit/ratelimitdialog.h
    src/ratelimit/ratelimitedrequest.cpp
    src/ratelimit/ratelimitedrequest.h
    src/ratelimit/ratelimiter.cpp
    src/ratelimit/ratelimiter.h
    src/ratelimit/ratelimitmanager.cpp
    src/ratelimit/ratelimitmanager.h
    src/ratelimit/ratelimitpolicy.cpp
    src/ratelimit/ratelimitpolicy.h
    src/ratelimit/scheduler.cpp
    src/ratelimit/scheduler.h
    src/ratelimit/stopsleep.cpp
    src/ratelimit/stopsleep.h
)

set(ACQ_REPOE
    src/repoe/repoe.cpp
    src/repoe/repoe.h
    src/repoe/baseitem.h
    src/repoe/itemclass.h
    src/repoe/stattranslation.h
)

set(ACQ_POE
    src/poe/endpoints/accountcharacter.h
    src/poe/endpoints/accountcharacters.h
    src/poe/endpoints/accountleagues.h
    src/poe/endpoints/accountprofile.h
    src/poe/endpoints/accountstash.h
    src/poe/endpoints/accountstashes.h
    src/poe/poe_utils.cpp
    src/poe/poe_utils.h
    src/poe/poeapiclient.cpp
    src/poe/poeapiclient.h
    src/poe/types/account.h
    src/poe/types/character.h
    src/poe/types/cruciblenode.h
    src/poe/types/displaymode.h
    src/poe/types/eventladderentry.h
    src/poe/types/frametype.h
    src/poe/types/gempage.h
    src/poe/types/gemtab.h
    src/poe/types/guild.h
    src/poe/types/item.h
    src/poe/types/itemfilter.h
    src/poe/types/itemjeweldata.h
    src/poe/types/itemmod.h
    src/poe/types/itemproperty.h
    src/poe/types/itemsocket.h
    src/poe/types/ladderentry.h
    src/poe/types/league.h
    src/poe/types/leagueaccount.h
    src/poe/types/leaguerule.h
    src/poe/types/mercenaryskill.h
    src/poe/types/passivegroup.h
    src/poe/types/passivenode.h
    src/poe/types/publicstashchange.h
    src/poe/types/pvpladderteamentry.h
    src/poe/types/pvpladderteammember.h
    src/poe/types/pvpmatch.h
    src/poe/types/stashtab.h
    src/poe/types/website/webcharacter.h
    src/poe/types/website/webstashtab.h
)

set(ACQ_UI
    src/ui/currencydialog.cpp
    src/ui/currencydialog.h
    src/ui/flowlayout.cpp
    src/ui/flowlayout.h
    src/ui/itemtooltip.cpp
    src/ui/itemtooltip.h
    src/ui/itemtooltiptext.cpp
    src/ui/itemtooltiptext.h
    src/ui/logindialog.cpp
    src/ui/logindialog.h
    src/ui/logpanel.cpp
    src/ui/logpanel.h
    src/ui/mainwindow_bridge.cpp
    src/ui/mainwindow_bridge.h
    src/ui/mainwindow.cpp
    src/ui/mainwindow.h
    src/ui/modsfilterform.cpp
    src/ui/modsfilterform.h
    src/ui/searchcombobox.cpp
    src/ui/searchcombobox.h
    src/ui/searchform.cpp
    src/ui/searchform.h
    src/ui/verticalscrollarea.cpp
    src/ui/verticalscrollarea.h
    # Forms
    src/ui/logindialog.ui
    src/ui/mainwindow.ui
)

set(ACQ_UTIL
    src/util/checkmsvc.cpp
    src/util/checkmsvc.h
    src/util/fatalerror.cpp
    src/util/fatalerror.h
    src/util/glaze_qt.h
    src/util/json_readers.cpp
    src/util/json_readers.h
    src/util/json_writers.cpp
    src/util/json_writers.h
    src/util/logging.cpp
    src/util/logging.h
    src/util/networkmanager.cpp
    src/util/networkmanager.h
    src/util/oauthmanager.cpp
    src/util/oauthmanager.h
    src/util/oauthtoken.cpp
    src/util/oauthtoken.h
    src/util/spdlog_qt.h
    src/util/updatechecker.cpp
    src/util/updatechecker.h
    src/util/util.cpp
    src/util/util.h
)

#------------------------------------------------------
# Resources
#------------------------------------------------------

set(ACQ_RESOURCES
    assets/backgrounds/ElderBackground_1x1.png
    assets/backgrounds/ElderBackground_1x3.png
    assets/backgrounds/ElderBackground_1x4.png
    assets/backgrounds/ElderBackground_2x1.png
    assets/backgrounds/ElderBackground_2x2.png
    assets/backgrounds/ElderBackground_2x3.png
    assets/backgrounds/ElderBackground_2x4.png
    assets/backgrounds/ShaperBackground_1x1.png
    assets/backgrounds/ShaperBackground_1x3.png
    assets/backgrounds/ShaperBackground_1x4.png
    assets/backgrounds/ShaperBackground_2x1.png
    assets/backgrounds/ShaperBackground_2x2.png
    assets/backgrounds/ShaperBackground_2x3.png
    assets/backgrounds/ShaperBackground_2x4.png
    assets/fonts/Fontin-SmallCaps.ttf
    assets/headers/CurrencyLeft.png
    assets/headers/CurrencyRight.png
    assets/headers/CurrencyMiddle.png
    assets/headers/GemLeft.png
    assets/headers/GemRight.png
    assets/headers/GemMiddle.png
    assets/headers/MagicLeft.png
    assets/headers/MagicRight.png
    assets/headers/MagicMiddle.png
    assets/headers/RareLeft.png
    assets/headers/RareRight.png
    assets/headers/RareMiddle.png
    assets/headers/RareSingleLineLeft.png
    assets/headers/RareSingleLineRight.png
    assets/headers/RareSingleLineMiddle.png
    assets/headers/UniqueLeft.png
    assets/headers/UniqueRight.png
    assets/headers/UniqueMiddle.png
    assets/headers/UniqueSingleLineLeft.png
    assets/headers/UniqueSingleLineRight.png
    assets/headers/UniqueSingleLineMiddle.png
    assets/headers/WhiteLeft.png
    assets/headers/WhiteRight.png
    assets/headers/WhiteMiddle.png
    assets/icons/icon.svg
    assets/separators/SeparatorWhite.png
    assets/separators/SeparatorMagic.png
    assets/separators/SeparatorRare.png
    assets/separators/SeparatorUnique.png
    assets/separators/SeparatorGem.png
    assets/separators/SeparatorCurrency.png
    assets/sockets/Socket_A.png
    assets/sockets/Socket_D.png
    assets/sockets/Socket_G.png
    assets/sockets/Socket_I.png
    assets/sockets/Socket_Link_Horizontal.png
    assets/sockets/Socket_Link_Vertical.png
    assets/sockets/Socket_S.png
    assets/symbols/CrusaderSymbol.png
    assets/symbols/EaterOfWorldsSymbol.png
    assets/symbols/ElderSymbol.png
    assets/symbols/FracturedSymbol.png
    assets/symbols/HunterSymbol.png
    assets/symbols/RedeemerSymbol.png
    assets/symbols/SearingExarchSymbol.png
    assets/symbols/ShaperSymbol.png
    assets/symbols/SynthesisedSymbol.png
    assets/symbols/WarlordSymbol.png
)

#------------------------------------------------------
# Other files
#------------------------------------------------------

# Automatically populate and include the versions header file.
configure_file("src/version_defines.h.in" "version_defines.h" @ONLY)

# Add other project files so they show up in Qt Creator.
add_custom_target(project_files SOURCES
    .clang-format
    .gitignore
    .github/workflows/build-linux.yml
    .github/workflows/build-macos.yml
    .github/workflows/build-windows.yml
    .github/workflows/codeql.yml
    AGENTS.md
    CLAUDE.md
    CONTRIBUTORS.md
    LICENSE
    README.md
    BUILD.md
    acquisition.desktop
    installer.iss
    # Project documents
    docs/adr/0001-qml-ui-migration-strategy.md
    docs/adr/0002-defer-qml-migration.md
    docs/cleanup/findings.md
    docs/design/items-pipeline.md
    docs/design/network-ggg-email-draft.md
    docs/design/network-ground-truth.md
    docs/design/network-redesign.md
    docs/design/network-redesign-phase5-verification.md
    docs/design/network-redesign-reviews.md
    docs/README.md

)

#------------------------------------------------------
# Qt Project Setup
#------------------------------------------------------

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Network NetworkAuth Sql Test Widgets)

qt_standard_project_setup()

# This target is the build gate for the filter core: its include and link
# closure deliberately contains no Qt Widgets module.
qt_add_library(acquisition_filters STATIC ${ACQ_FILTERS})
set_target_properties(acquisition_filters PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(acquisition_filters PUBLIC "${acquisition_SOURCE_DIR}/src")
target_link_libraries(acquisition_filters
    PUBLIC
        Qt6::Core
        Qt6::Gui
    # Item's include closure reaches ItemLocation's Qt Network/spdlog formatter,
    # and poe::ItemMod's glaze reader for GGG's "flags": [] quirk.
    # These remain deliberately non-widget so the filter-core boundary stays intact.
    PRIVATE
        Qt6::Network
        glaze::glaze
        spdlog::spdlog_header_only
)

qt_add_library(acquisition_core STATIC
    ${ACQ_CORE}
    ${ACQ_DATASTORE}
    ${ACQ_LEGACY}
    ${ACQ_RATELIMIT}
    ${ACQ_REPOE}
    ${ACQ_POE}
    ${ACQ_UI}
    ${ACQ_UTIL}
    # Generated version header
    "${CMAKE_BINARY_DIR}/version_defines.h"
)

qt_add_resources(acquisition_core
    PREFIX "/"
    BASE "assets"
    FILES ${ACQ_RESOURCES}
)

set_target_properties(acquisition_core PROPERTIES
    AUTORCC ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

# Setup include directories.
target_include_directories(acquisition_core PUBLIC
    "${acquisition_SOURCE_DIR}/src"
    "${acquisition_BINARY_DIR}"
)

add_subdirectory(deps/qdarkstyle)

# Link required libraries.
target_link_libraries(acquisition_core PUBLIC
    acquisition_filters
    # Qt libraries
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::NetworkAuth
    Qt6::Widgets
    Qt6::Sql
    # Extenal libraries
    glaze::glaze
    QCoro6::Core
    QCoro6::Network
    qdarkstyle
    QXlsx::QXlsx
    sentry::sentry
    semver
    spdlog::spdlog_header_only
)

qt_add_executable(acquisition WIN32 MACOSX_BUNDLE
    src/main.cpp
)

# Add configure_file() inputs so they show up in Qt Creator.
target_sources(acquisition PRIVATE
    FILE_SET project_files TYPE HEADERS
    FILES
        version_info.rc.in
        src/version_defines.h.in
)

set_target_properties(acquisition PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

target_link_libraries(acquisition PRIVATE acquisition_core)

#----------------------------------
# Platform-specific section
#----------------------------------

if(WIN32)

    # Enable more warnings by default (MSVC only).
    target_compile_options(acquisition_core PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/EHsc>
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/Zi>
    )
    target_link_options(acquisition PRIVATE
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/DEBUG>
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/OPT:REF>
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/OPT:ICF>
    )

    # Define versions for the versioninfo.
    math(EXPR app_version_major "${acquisition_VERSION_MAJOR} + 0")
    math(EXPR app_version_minor "${acquisition_VERSION_MINOR} + 0")
    math(EXPR app_version_patch "${acquisition_VERSION_PATCH} + 0")
    math(EXPR app_version_tweak "${acquisition_VERSION_PATCH} + 0")

    # Define the rest of the versioninfo data.
    set(app_company_name        "")
    set(app_file_description    "")
    set(app_file_version        "${app_version_string}")
    set(app_internal_name       "${app_name}")
    set(app_legal_copyright     "${app_copyright}")
    set(app_legal_trademark     "${app_trademark}")
    set(app_original_filename   "${app_name}.exe")
    set(app_product_name        "${app_name}")
    set(app_product_version     "${app_version_string}")
    set(app_icon                "${acquisition_SOURCE_DIR}/assets/icons/icon.ico")

    # Create the windows VERSIONINFO resource file.
    configure_file(version_info.rc.in version_info.rc @ONLY)

    target_sources(acquisition PRIVATE
        "${app_icon}"
        "version_info.rc")
        
elseif(APPLE)

    set(macos_app_icon "${acquisition_SOURCE_DIR}/assets/icons/icon.icns")

    # Setup the macOS bundle
    set_target_properties(acquisition PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Acquisition"
        MACOSX_BUNDLE_BUNDLE_VERSION "${app_version}"
        MACOSX_BUNDLE_COPYRIGHT "${app_copyright}"
        #MACOSX_BUNDLE_GUI_IDENTIFIER ""
        MACOSX_BUNDLE_ICON_FILE "icon.icns"
        MACOSX_BUNDLE_INFO_STRING "${acquisition_DESCRIPTION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${app_version}"
    )

    # Add the macOS application icon
    target_sources(acquisition PRIVATE "${macos_app_icon}")
    set_source_files_properties("${macos_app_icon}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    
    # Generate debugging symbols file.
    target_compile_options(acquisition PRIVATE $<$<NOT:$<CONFIG:Debug>>:-g>)
    add_custom_command(TARGET acquisition POST_BUILD
        COMMAND xcrun dsymutil
            "$<TARGET_FILE:acquisition>"
            -o "${CMAKE_BINARY_DIR}/acquisition_${app_version_string}.dSYM"
        VERBATIM
    )
    
elseif(UNIX)

    # Link OpenSSL on Linux.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(acquisition_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)

    # Generate debug symbols.
    target_compile_options(acquisition_core PRIVATE $<$<NOT:$<CONFIG:Debug>>:-g>)
    target_compile_options(acquisition PRIVATE $<$<NOT:$<CONFIG:Debug>>:-g>)
    if(CMAKE_OBJCOPY AND CMAKE_STRIP)
        add_custom_command(TARGET acquisition
            POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} --only-keep-debug acquisition acquisition.debug
            COMMAND ${CMAKE_STRIP} --strip-debug --strip-unneeded acquisition
            COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink=acquisition.debug acquisition
            COMMAND chmod -x acquisition.debug
        )
    else()
        message(NOTICE "Could not create acquisition.debug")
    endif()
    
endif()

#------------------------------------------------------
# Crashpad for Sentry
#------------------------------------------------------

# Make sure the crashpad handler is copied next to the output executable.
# Ask CMake for the handler's location rather than assuming a single-config
# output layout: multi-config generators (e.g. Visual Studio) put it under a
# per-config subdirectory of crashpad_build/handler/.
set(CRASHPAD_HANDLER_DST "$<TARGET_FILE_DIR:acquisition>/$<TARGET_FILE_NAME:crashpad_handler>")
add_dependencies(acquisition crashpad_handler)
add_custom_command(TARGET acquisition POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:crashpad_handler>" "${CRASHPAD_HANDLER_DST}"
    COMMAND_EXPAND_LISTS
    VERBATIM
)

# Install the application and the Crashpad handler it launches at runtime.
install(TARGETS acquisition
    BUNDLE DESTINATION . COMPONENT Runtime
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime
)

if(APPLE)
    install(PROGRAMS "$<TARGET_FILE:crashpad_handler>"
        DESTINATION "acquisition.app/Contents/MacOS"
        COMPONENT Runtime
    )
else()
    install(PROGRAMS "$<TARGET_FILE:crashpad_handler>"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT Runtime
    )
endif()

if(UNIX AND NOT APPLE)
    install(FILES acquisition.desktop
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications"
        COMPONENT Runtime
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

target_link_libraries(qdarkstyle PRIVATE Qt6::Core)
