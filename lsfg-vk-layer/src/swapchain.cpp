/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain.hpp"
#include "log.hpp"
#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace lsfgvk;
using namespace lsfgvk::layer;
using SteadyClock = std::chrono::steady_clock;

namespace {
    VkImageMemoryBarrier barrierHelper(VkImage handle,
            VkAccessFlags srcAccessMask,
            VkAccessFlags dstAccessMask,
            VkImageLayout oldLayout,
            VkImageLayout newLayout) {
        return VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = srcAccessMask,
            .dstAccessMask = dstAccessMask,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = handle,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
    }
}

void layer::context_ModifySwapchainCreateInfo(const ls::GameConf& profile, uint32_t maxImages,
        VkSwapchainCreateInfoKHR& createInfo) {
    createInfo.imageUsage |=
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    switch (profile.pacing) {
        case ls::Pacing::None:
            createInfo.minImageCount += profile.multiplier;
            if (maxImages && createInfo.minImageCount > maxImages)
                createInfo.minImageCount = maxImages;

            createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            break;
    }
}

Swapchain::Swapchain(const vk::Vulkan& vk, backend::Instance& backend,
            ls::GameConf profile, SwapchainInfo info) :
        instance(backend),
        profile(std::move(profile)), info(std::move(info)) {
    const VkExtent2D extent = this->info.extent;
    const bool hdr = this->info.format > 57;

    std::vector<int> sourceFds(2);
    std::vector<int> destinationFds(this->profile.multiplier - 1);

    this->sourceImages.reserve(sourceFds.size());
    for (int& fd : sourceFds)
        this->sourceImages.emplace_back(vk,
            extent, hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::nullopt, &fd);

    this->destinationImages.reserve(destinationFds.size());
    for (int& fd : destinationFds)
        this->destinationImages.emplace_back(vk,
            extent, hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::nullopt, &fd);

    int syncFd{};
    this->syncSemaphore.emplace(vk, 0, std::nullopt, &syncFd);

    try {
        this->ctx = ls::owned_ptr<ls::R<backend::Context>>(
            new ls::R<backend::Context>(backend.openContext(
                { sourceFds.at(0), sourceFds.at(1) }, destinationFds, syncFd,
                extent.width, extent.height,
                hdr, 1.0F / this->profile.flow_scale, this->profile.performance_mode
            )),
            [backend = &backend](ls::R<backend::Context>& ctx) {
                backend->closeContext(ctx);
            }
        );

        backend::makeLeaking(); // don't worry about it :3
    } catch (const std::exception& e) {
        throw ls::error("failed to create swapchain context", e);
    }

    this->renderCommandBuffer.emplace(vk);
    this->renderFence.emplace(vk);
    this->copyFence.emplace(vk);
    for (size_t i = 0; i < this->destinationImages.size(); i++) {
        this->passes.emplace_back(RenderPass {
            .commandBuffer = vk::CommandBuffer(vk),
            .acquireSemaphore = vk::Semaphore(vk)
        });
    }

    const size_t frames = std::max(this->info.images.size(), this->destinationImages.size() + 2);
    for (size_t i = 0; i < frames; i++) {
        this->postCopySemaphores.emplace_back(
            vk::Semaphore(vk),
            vk::Semaphore(vk)
        );
    }

    if (this->profile.adaptive) {
        layerLog("lsfg-vk: adaptive mode presents on the game thread (target_fps="
            + std::to_string(this->profile.target_fps)
            + ", multiplier ceiling=" + std::to_string(this->profile.multiplier) + ")");
    }
}

size_t Swapchain::chooseGeneratedCount() {
    const size_t maxGen = this->destinationImages.size();
    const double targetFps = std::max(1.0, static_cast<double>(this->profile.target_fps));
    const auto now = SteadyClock::now();

    if (!this->lastPresentTime.has_value()) {
        this->lastPresentTime = now;
        this->adaptiveError = 0.0;
        return 0;
    }

    const double elapsed = std::chrono::duration<double>(now - *this->lastPresentTime).count();
    this->lastPresentTime = now;

    // Ignore hitches and implausibly fast presents; passthrough the real frame.
    if (elapsed < 0.0005 || elapsed > 0.100) {
        this->adaptiveError = 0.0;
        return 0;
    }

    double desired = targetFps * elapsed - 1.0 + this->adaptiveError;
    if (desired < 0.0)
        desired = 0.0;

    auto gen = static_cast<size_t>(std::llround(desired));
    if (gen > maxGen)
        gen = maxGen;

    this->adaptiveError = desired - static_cast<double>(gen);
    if (gen == maxGen && this->adaptiveError > 0.0)
        this->adaptiveError = 0.0;
    return gen;
}

VkResult Swapchain::present(const vk::Vulkan& vk,
        VkQueue queue, VkSwapchainKHR swapchain,
        void* next_chain, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores) {
    const size_t genCount = this->profile.adaptive
        ? chooseGeneratedCount()
        : this->destinationImages.size();
    if (this->profile.adaptive && this->fidx == 0) {
        layerLog("lsfg-vk: first adaptive present genCount=" + std::to_string(genCount));
    }
    return presentGenerated(vk, queue, swapchain, next_chain, imageIdx, semaphores, genCount);
}

VkResult Swapchain::presentGenerated(const vk::Vulkan& vk,
        VkQueue queue, VkSwapchainKHR swapchain,
        void* next_chain, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores,
        size_t genCount) {
    const auto& swapchainImage = this->info.images.at(imageIdx);
    const auto& sourceImage = this->sourceImages.at(this->fidx % 2);

    try {
        this->instance.get().scheduleFrames(this->ctx.get(), genCount);
    } catch (const std::exception& e) {
        throw ls::error("failed to schedule frames", e);
    }

    forceFifo(next_chain);

    if (this->renderFenceInFlight) {
        if (!this->renderFence->wait(vk, 150ULL * 1000 * 1000))
            throw ls::vulkan_error(VK_TIMEOUT, "vkWaitForFences() failed");
        this->renderFence->reset(vk);
        this->renderFenceInFlight = false;
    }

    const auto& cmdbuf = *this->renderCommandBuffer;
    cmdbuf.begin(vk);

    cmdbuf.blitImage(vk,
        {
            barrierHelper(swapchainImage,
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            ),
            barrierHelper(sourceImage.handle(),
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
            ),
        },
        { swapchainImage, sourceImage.handle() },
        sourceImage.getExtent(),
        {
            barrierHelper(swapchainImage,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            ),
        }
    );

    cmdbuf.end(vk);

    // No extra frames: copy into the FG source ring, then present the real image
    // on this same (game) thread. Steam Deck WSI is not safe from a second thread.
    if (genCount == 0) {
        this->copyFence->reset(vk);
        cmdbuf.submit(vk,
            semaphores, VK_NULL_HANDLE, 0,
            {}, VK_NULL_HANDLE, 0,
            this->copyFence->handle()
        );
        if (!this->copyFence->wait(vk, 2ULL * 1000 * 1000 * 1000))
            throw ls::vulkan_error(VK_TIMEOUT, "vkWaitForFences() failed");

        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = next_chain,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &imageIdx,
        };
        auto res = vk.df().QueuePresentKHR(queue, &presentInfo);
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");

        this->fidx++;
        return res;
    }

    cmdbuf.submit(vk,
        semaphores, VK_NULL_HANDLE, 0,
        {}, this->syncSemaphore->handle(), this->idx++
    );

    for (size_t i = 0; i < genCount; i++) {
        auto& pcs = this->postCopySemaphores.at(this->idx % this->postCopySemaphores.size());
        auto& destinationImage = this->destinationImages.at(i);
        auto& pass = this->passes.at(i);

        uint32_t aqImageIdx{};
        auto res = vk.df().AcquireNextImageKHR(vk.dev(), swapchain,
            UINT64_MAX, pass.acquireSemaphore.handle(),
            VK_NULL_HANDLE,
            &aqImageIdx
        );
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw ls::vulkan_error(res, "vkAcquireNextImageKHR() failed");

        const auto& aquiredSwapchainImage = this->info.images.at(aqImageIdx);

        auto& passCmd = pass.commandBuffer;
        passCmd.begin(vk);

        passCmd.blitImage(vk,
            {
                barrierHelper(destinationImage.handle(),
                    VK_ACCESS_NONE,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                ),
                barrierHelper(aquiredSwapchainImage,
                    VK_ACCESS_NONE,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                ),
            },
            { destinationImage.handle(), aquiredSwapchainImage },
            destinationImage.getExtent(),
            {
                barrierHelper(aquiredSwapchainImage,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                ),
            }
        );

        std::vector<VkSemaphore> waitSemaphores{ pass.acquireSemaphore.handle() };
        if (i) {
            const auto& prevPCS = this->postCopySemaphores.at(
                (this->idx - 1) % this->postCopySemaphores.size());
            waitSemaphores.push_back(prevPCS.second.handle());
        }

        const std::vector<VkSemaphore> signalSemaphores{
            pcs.first.handle(),
            pcs.second.handle()
        };

        passCmd.end(vk);
        passCmd.submit(vk,
            waitSemaphores, this->syncSemaphore->handle(), this->idx,
            signalSemaphores, VK_NULL_HANDLE, 0,
            i == genCount - 1 ? this->renderFence->handle() : VK_NULL_HANDLE
        );
        if (i == genCount - 1)
            this->renderFenceInFlight = true;

        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = i ? nullptr : next_chain,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &pcs.first.handle(),
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &aqImageIdx,
        };
        res = vk.df().QueuePresentKHR(queue, &presentInfo);
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");

        this->idx++;
    }

    auto& lastPCS = this->postCopySemaphores.at((this->idx - 1) % this->postCopySemaphores.size());
    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &lastPCS.second.handle(),
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &imageIdx,
    };
    auto res = vk.df().QueuePresentKHR(queue, &presentInfo);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");

    this->fidx++;
    return res;
}

void Swapchain::forceFifo(void* next_chain) const {
    if (this->profile.pacing != ls::Pacing::None)
        return;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    auto* info = reinterpret_cast<VkSwapchainPresentModeInfoEXT*>(next_chain);
    while (info) {
        if (info->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT) {
            for (size_t i = 0; i < info->swapchainCount; i++)
                const_cast<VkPresentModeKHR*>(info->pPresentModes)[i] =
                    VK_PRESENT_MODE_FIFO_KHR;
        }

        info = reinterpret_cast<VkSwapchainPresentModeInfoEXT*>(const_cast<void*>(info->pNext));
    }
#pragma clang diagnostic pop
}
