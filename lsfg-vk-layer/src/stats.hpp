/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstddef>

namespace lsfgvk::layer {

    /// Accumulate one real present, `genCount` generated frames, and whether
    /// this skip ran optical-flow ingest.
    /// Flushes a one-second snapshot to $LSFGVK_STATS (or next to $LSFGVK_CONFIG).
    void recordFrameStats(size_t genCount, bool ingest, float targetFps, bool adaptive);

}
