// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

// The one way into glaze. Every file that reads or writes JSON with glaze
// includes this header, and no other file includes a glaze header.
//
// It brings glaze itself, the support for Qt's types (util/glaze_qt.h), and
// what glaze has to know about this project's own types (the glaze_meta.h
// files). Those last declarations are why there is only one way in: glaze
// reflects a struct it has been told nothing about, so a file that included
// glaze without them would compile and quietly use other keys. The
// glaze_boundary test fails when a file goes around this header.
//
// It is expensive to include (about half a gigabyte and two seconds of
// compile, #476). Include it from a .cpp file, never from a header.

#include "legacy/glaze_meta.h"    // IWYU pragma: export
#include "poe/types/glaze_meta.h" // IWYU pragma: export
#include "util/glaze_qt.h"        // IWYU pragma: export
