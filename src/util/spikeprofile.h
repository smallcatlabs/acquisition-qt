// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// SPIKE-ONLY (spike/m3-sort-profile, R3-4): timing and counting probes for
// the M3 sort-profiling spike. This header and every use of it lives only on
// the spike branch and is never merged. Probes are plain globals (the whole
// instrumented path is single-threaded) read and reset by
// tests/m3_sort_benchmark.cpp.

#pragma once

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace spike_profile {

    inline int64_t now_ns()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // --- Search::FilterItems (filter pass vs model reset) ---
    inline int64_t filter_begin_reset_ns = 0; // m_model.beginUpdate()  (beginResetModel)
    inline int64_t filter_loop_ns = 0;        // filter loop + bucketing + empty tabs
    inline int64_t filter_bucket_order_ns = 0; // bucket move + display-order sort
    inline int64_t filter_end_reset_ns = 0;   // m_model.endUpdate()  (endResetModel + view rebuild)
    inline int64_t filter_total_ns = 0;

    // --- MainWindow::ModelViewRefresh ---
    inline int64_t mvr_total_ns = 0;
    inline int64_t mvr_filter_ns = 0;            // FilterItems call
    inline int64_t mvr_sort_enable_ns = 0;       // setSortingEnabled(true) -> model sort
    inline int64_t mvr_restore_expansion_ns = 0; // RestoreViewExpansion
    inline int64_t mvr_restore_rest_ns = 0;      // ReselectCurrentItem + RestoreViewScroll

    // --- ItemsModel::sort ---
    inline int64_t model_sort_total_ns = 0;
    inline int64_t sort_layout_about_ns = 0; // layoutAboutToBeChanged emit
    inline int64_t sort_snapshot_ns = 0;     // persistent-index snapshot
    inline int64_t sort_search_sort_ns = 0;  // Search::Sort call (all buckets)
    inline int64_t sort_remap_ns = 0;        // remap + changePersistentIndexList
    inline int64_t sort_layout_changed_ns = 0; // layoutChanged emit

    // --- Search::Sort per-bucket samples: (bucket item count, ns) ---
    inline std::vector<std::pair<int, int64_t>> bucket_sort_samples;

    // --- comparator counters ---
    inline uint64_t lt_calls_base = 0;  // Column::lt (NameColumn and everything without an override)
    inline uint64_t lt_calls_price = 0; // PriceColumn::lt
    inline uint64_t lt_calls_date = 0;  // DateColumn::lt
    inline uint64_t mv_path_double = 0; // multivalue: single-number regex matched
    inline uint64_t mv_path_range = 0;  // multivalue: range/ratio regex matched
    inline uint64_t mv_path_string = 0; // multivalue: string fallthrough (2 failed regexes)
    inline uint64_t item_lt_calls = 0;  // Item::operator< (tuple tie-break reached)

    inline void reset()
    {
        filter_begin_reset_ns = 0;
        filter_loop_ns = 0;
        filter_bucket_order_ns = 0;
        filter_end_reset_ns = 0;
        filter_total_ns = 0;
        mvr_total_ns = 0;
        mvr_filter_ns = 0;
        mvr_sort_enable_ns = 0;
        mvr_restore_expansion_ns = 0;
        mvr_restore_rest_ns = 0;
        model_sort_total_ns = 0;
        sort_layout_about_ns = 0;
        sort_snapshot_ns = 0;
        sort_search_sort_ns = 0;
        sort_remap_ns = 0;
        sort_layout_changed_ns = 0;
        bucket_sort_samples.clear();
        lt_calls_base = 0;
        lt_calls_price = 0;
        lt_calls_date = 0;
        mv_path_double = 0;
        mv_path_range = 0;
        mv_path_string = 0;
        item_lt_calls = 0;
    }

} // namespace spike_profile
