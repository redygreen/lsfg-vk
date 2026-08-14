/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <string>

namespace lsfgvk::layer {

    /// Write a line to stderr and to $LSFGVK_LOG (or next to $LSFGVK_CONFIG).
    /// Wine/Proton often swallow stderr; the file is the reliable Deck log.
    void layerLog(const std::string& message);

}
