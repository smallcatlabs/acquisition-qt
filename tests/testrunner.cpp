// SPDX-License-Identifier: GPL-3.0-or-later

// main() of acq_tests: runs the one suite named by the first argument
// (testsuite.h).

#include "testsuite.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

    struct Suite
    {
        std::string name;
        acq_test::SuiteMain main;
        bool one_scenario;
    };

    // A function-local static, so that it exists before the first registrar runs
    // whatever order the files' static initializers run in.
    std::vector<Suite> &suites()
    {
        static std::vector<Suite> list;
        return list;
    }

} // namespace

acq_test::Registrar::Registrar(const char *file, SuiteMain main, bool one_scenario)
{
    // The suite's name is its file's name: .../tests/tst_search.cpp is tst_search.
    std::string name = file;
    name.erase(0, name.find_last_of("/\\") + 1);
    name = name.substr(0, name.find('.'));
    suites().push_back({name, main, one_scenario});
}

int main(int argc, char *argv[])
{
    auto &list = suites();
    std::sort(list.begin(), list.end(), [](const Suite &a, const Suite &b) {
        return a.name < b.name;
    });
    const auto suite = std::find_if(list.begin(), list.end(), [&](const Suite &s) {
        return argc > 1 && s.name == argv[1];
    });
    if (suite == list.end()) {
        if (argc > 1) {
            std::fprintf(stderr, "acq_tests: no suite named '%s'\n", argv[1]);
        }
        std::fprintf(stderr, "Usage: acq_tests <suite> [Qt Test arguments]\nSuites:\n");
        for (const Suite &s : list) {
            std::fprintf(stderr, "  %s\n", s.name.c_str());
        }
        return 2;
    }

    // The suite sees the command line a test executable of its own would
    // have seen: the program, then the arguments after the suite's name.
    argv[1] = argv[0];
    --argc;
    ++argv;

    if (suite->one_scenario) {
        // Each destructive shutdown scenario must be isolated in its own
        // process (network-redesign verification §6): no slot name runs every
        // slot, and several run several. CTest passes exactly one.
        int scenarios = 0;
        for (int i = 1; i < argc; ++i) {
            if (argv[i][0] != '-') {
                ++scenarios;
            }
        }
        if (scenarios != 1) {
            std::fprintf(stderr,
                         "%s: run exactly one scenario per process. Got %d scenario "
                         "argument(s); run via ctest or pass a single slot name.\n",
                         suite->name.c_str(),
                         scenarios);
            return 2;
        }
    }

    return suite->main(suite->name.c_str(), argc, argv);
}
