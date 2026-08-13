/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstddef>

namespace lsfgvk::layer {

    /// Accumulate one real present and `genCount` generated frames.
    /// Flushes a one-second snapshot to $LSFGVK_STATS (or next to $LSFGVK_CONFIG).
    void recordFrameStats(size_t genCount, float targetFps, bool adaptive);

}
