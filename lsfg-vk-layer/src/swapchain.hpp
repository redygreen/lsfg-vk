/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "adaptive.hpp"
#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/timeline_semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace lsfgvk::layer {

    /// swapchain info struct
    struct SwapchainInfo {
        std::vector<VkImage> images;
        VkFormat format;
        VkColorSpaceKHR colorSpace;
        VkExtent2D extent;
        VkPresentModeKHR presentMode;
        VkImageUsageFlags imageUsage{VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    };

    /// modify the swapchain create info based on the profile pre-swapchain creation
    /// @param profile active game profile
    /// @param maxImages maximum number of images supported by the surface
    /// @param createInfo swapchain create info to modify
    void context_ModifySwapchainCreateInfo(const ls::GameConf& profile, uint32_t maxImages,
        VkSwapchainCreateInfoKHR& createInfo);

    /// swapchain context for a layer instance
    class Swapchain {
    public:
        /// create a new swapchain context
        /// @param vk vulkan instance
        /// @param backend lsfg-vk backend instance
        /// @param profile active game profile
        /// @param info swapchain info
        Swapchain(const vk::Vulkan& vk, backend::Instance& backend,
            ls::GameConf profile, SwapchainInfo info);

        Swapchain(const Swapchain&) = delete;
        Swapchain& operator=(const Swapchain&) = delete;
        Swapchain(Swapchain&&) = delete;
        Swapchain& operator=(Swapchain&&) = delete;
        ~Swapchain() = default;

        /// present a frame
        /// @param vk vulkan instance
        /// @param queue presentation queue
        /// @param next_chain next chain pointer for the present info (WARN: shared!)
        /// @param imageIdx swapchain image index to present to
        /// @param semaphores semaphores to wait on before presenting
        /// @param originalInfo original QueuePresent info. Adaptive skip
        ///                     presents wait the source-copy semaphore because
        ///                     the blit already consumed the game's binary waits.
        /// @throws ls::vulkan_error on vulkan errors
        VkResult present(const vk::Vulkan& vk,
            VkQueue queue, VkSwapchainKHR swapchain,
            void* next_chain, uint32_t imageIdx,
            const std::vector<VkSemaphore>& semaphores,
            const VkPresentInfoKHR* originalInfo = nullptr);

        /// Apply target_fps / adaptive without rebuilding FG images.
        /// @return false if multiplier or backend settings changed
        [[nodiscard]] bool tryApplyRuntimeProfile(const ls::GameConf& next);
        [[nodiscard]] bool runtimeProfileCompatible(const ls::GameConf& next) const;

    private:
        [[nodiscard]] size_t chooseGeneratedCount(AdaptivePacer::Clock::time_point now);
        VkResult queuePresentOriginal(const vk::Vulkan& vk, VkQueue queue,
            VkSwapchainKHR swapchain, void* next_chain, uint32_t imageIdx,
            const std::vector<VkSemaphore>& semaphores,
            const VkPresentInfoKHR* originalInfo,
            VkSemaphore extraWait = VK_NULL_HANDLE,
            bool replaceAppWaits = false);
        void waitFence(const vk::Vulkan& vk, const vk::Fence& fence, bool& inFlight);
        void copyToSource(const vk::Vulkan& vk, VkImage swapchainImage,
            const std::vector<VkSemaphore>& waitSemaphores, bool signalSync,
            VkSemaphore copyDone, VkFence fence);
        VkResult presentGeneratedFrames(const vk::Vulkan& vk, VkQueue queue,
            VkSwapchainKHR swapchain, void* next_chain, uint32_t imageIdx,
            size_t genCount, bool presentRealWithInternalSemaphores);
        void forceFifo(void* next_chain) const;
        void waitDisplaySlot();

        std::vector<vk::Image> sourceImages;
        std::vector<vk::Image> destinationImages;
        ls::lazy<vk::TimelineSemaphore> syncSemaphore;

        ls::lazy<vk::CommandBuffer> renderCommandBuffer;
        ls::lazy<vk::Fence> renderFence;
        ls::lazy<vk::Fence> copyFence;
        std::vector<vk::Semaphore> copyDoneSemaphores;
        struct RenderPass {
            vk::CommandBuffer commandBuffer;
            vk::Semaphore acquireSemaphore;
        };
        std::vector<RenderPass> passes;
        std::vector<std::pair<vk::Semaphore, vk::Semaphore>> postCopySemaphores;

        ls::R<backend::Instance> instance;
        ls::owned_ptr<ls::R<backend::Context>> ctx;
        size_t idx{1};
        size_t fidx{0}; // real frame index

        ls::GameConf profile;
        SwapchainInfo info;

        AdaptivePacer pacer;
        double lastGameDt{0.0};
        double lastWorkDt{0.0};
        double lastWaitDt{0.0};
        double lastEmaDt{0.0};
        double lastAcc{0.0};
        double lastExtrasWant{0.0};
        double lastPacedMs{0.0};
        size_t logPresentsRemaining{16};
        bool lastIngest{false};
        bool lastPacedHold{false};
        bool renderFenceInFlight{false};
        bool copyFenceInFlight{false};
    };

}
