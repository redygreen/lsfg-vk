/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "utils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <vulkan/vulkan_core.h>

using namespace lsfgvk;
using namespace lsfgvk::backend;

ConstantBuffer backend::getConstantBuffer(float timestamp, float invFlow) {
    return ConstantBuffer {
        .resolutionInvScale = invFlow,
        .timestamp = timestamp,
        .uiThreshold = 0.5F
    };
}

ConstantBuffer backend::getDefaultConstantBuffer(
        size_t index, size_t total, float invFlow) {
    return getConstantBuffer(
        static_cast<float>(index + 1) / static_cast<float>(total + 1),
        invFlow
    );
}

VkExtent2D backend::shift_extent(VkExtent2D extent, uint32_t i) {
    return VkExtent2D{
        .width = extent.width >> i,
        .height = extent.height >> i
    };
}

VkExtent2D backend::add_shift_extent(VkExtent2D extent, uint32_t a, uint32_t i) {
    return VkExtent2D{
        .width = (extent.width + a) >> i,
        .height = (extent.height + a) >> i
    };
}

std::string backend::to_hex_id(uint32_t id) {
    const std::array<char, 17> chars = std::to_array("0123456789ABCDEF");

    std::string result = "0x";
    result += chars.at((id >> 12) & 0xF);
    result += chars.at((id >> 8) & 0xF);
    result += chars.at((id >> 4) & 0xF);
    result += chars.at(id & 0xF);
    return result;
}
