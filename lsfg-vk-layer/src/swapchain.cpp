/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain.hpp"
#include "adaptive.hpp"
#include "log.hpp"
#include "stats.hpp"
#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/configuration/config.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace lsfgvk;
using namespace lsfgvk::layer;

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
    this->copyDoneSemaphores.emplace_back(vk);
    this->copyDoneSemaphores.emplace_back(vk);
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

size_t Swapchain::chooseGeneratedCount(AdaptivePacer::Clock::time_point now) {
    const auto sample = this->pacer.choose(
        static_cast<double>(this->profile.target_fps),
        this->destinationImages.size(),
        now,
        GameAcquireTiming::takeWait());
    this->lastGameDt = sample.gameDt;
    this->lastWorkDt = sample.workDt;
    this->lastWaitDt = sample.waitDt;
    this->lastEmaDt = sample.emaDt;
    this->lastAcc = sample.acc;
    this->lastIngest = sample.ingest;
    this->lastExtrasWant = sample.extrasWant;
    return sample.genCount;
}

bool Swapchain::runtimeProfileCompatible(const ls::GameConf& next) const {
    return next.multiplier == this->profile.multiplier
        && next.flow_scale == this->profile.flow_scale
        && next.performance_mode == this->profile.performance_mode
        && next.pacing == this->profile.pacing
        && next.gpu == this->profile.gpu;
}

bool Swapchain::tryApplyRuntimeProfile(const ls::GameConf& next) {
    if (!runtimeProfileCompatible(next))
        return false;

    const bool changed = next.adaptive != this->profile.adaptive
        || next.target_fps != this->profile.target_fps
        || next.enabled != this->profile.enabled;
    this->profile = next;
    if (changed) {
        layerLog("lsfg-vk: adaptive runtime update target_fps="
            + std::to_string(this->profile.target_fps)
            + " adaptive=" + std::string(this->profile.adaptive ? "1" : "0")
            + " enabled=" + std::string(this->profile.enabled ? "1" : "0")
            + " ceiling=" + std::to_string(this->profile.multiplier));
        this->logPresentsRemaining = 8;
    }
    return true;
}

VkResult Swapchain::present(const vk::Vulkan& vk,
        VkQueue queue, VkSwapchainKHR swapchain,
        void* next_chain, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores,
        const VkPresentInfoKHR* originalInfo) {
    const auto now = AdaptivePacer::Clock::now();
    const size_t genCount = !this->profile.enabled
        ? 0
        : this->profile.adaptive
            ? chooseGeneratedCount(now)
            : this->destinationImages.size();
    const bool logThis = this->profile.adaptive
        && (this->logPresentsRemaining > 0 || this->fidx % 60 == 0);
    if (logThis) {
        layerLog("lsfg-vk: adaptive present fidx=" + std::to_string(this->fidx)
            + " genCount=" + std::to_string(genCount)
            + " game_ms=" + std::to_string(this->lastGameDt * 1000.0)
            + " work_ms=" + std::to_string(this->lastWorkDt * 1000.0)
            + " wait_ms=" + std::to_string(this->lastWaitDt * 1000.0)
            + " ema_ms=" + std::to_string(this->lastEmaDt * 1000.0)
            + " acc=" + std::to_string(this->lastAcc)
            + " extrasWant=" + std::to_string(this->lastExtrasWant)
            + " ingest=" + std::string(this->lastIngest ? "1" : "0")
            + " enabled=" + std::string(this->profile.enabled ? "1" : "0")
            + " waits=" + std::to_string(semaphores.size()));
    }

    const auto& swapchainImage = this->info.images.at(imageIdx);
    VkResult result = VK_SUCCESS;

    try {
        if (!this->profile.enabled) {
            this->instance.get().scheduleFrames(this->ctx.get(), 0);
            result = queuePresentOriginal(vk, queue, swapchain, next_chain, imageIdx,
                semaphores, originalInfo);
        } else if (!this->profile.adaptive) {
            this->instance.get().scheduleFrames(this->ctx.get(), genCount);
            forceFifo(next_chain);
            waitFence(vk, *this->renderFence, this->renderFenceInFlight);
            copyToSource(vk, swapchainImage, semaphores, true, VK_NULL_HANDLE, VK_NULL_HANDLE);
            result = presentGeneratedFrames(vk, queue, swapchain, next_chain, imageIdx,
                genCount, true);
        } else {
            // Skip copies wait the game's render semaphores, then
            // QueuePresent waits copy-done only — binary semaphores cannot
            // be waited by both the blit and the original present.
            //
            // Optical-flow ingest runs only on dithered skips (a generate
            // is coming). Skips at/above target are copy-only so Adaptive
            // does not run LSFG at native rate.
            waitFence(vk, *this->copyFence, this->copyFenceInFlight);
            waitFence(vk, *this->renderFence, this->renderFenceInFlight);

            if (this->fidx == 0 || genCount == 0) {
                forceFifo(next_chain);
                const bool ingest = this->fidx != 0 && this->lastIngest;
                if (ingest)
                    this->instance.get().scheduleIngest(this->ctx.get());
                else
                    this->instance.get().scheduleFrames(this->ctx.get(), 0);
                const VkSemaphore copyDone =
                    this->copyDoneSemaphores.at(this->fidx % 2).handle();
                copyToSource(vk, swapchainImage, semaphores, ingest, copyDone,
                    this->copyFence->handle());
                this->copyFenceInFlight = true;
                result = queuePresentOriginal(vk, queue, swapchain, next_chain, imageIdx,
                    semaphores, originalInfo, copyDone, true);
                if (this->logPresentsRemaining > 0)
                    layerLog(std::string(ingest
                            ? "lsfg-vk: adaptive ingest skip present ok res="
                            : "lsfg-vk: adaptive cheap skip present ok res=")
                        + std::to_string(static_cast<int>(result)));
            } else {
                this->instance.get().scheduleFrames(this->ctx.get(), genCount);
                forceFifo(next_chain);
                copyToSource(vk, swapchainImage, semaphores, true, VK_NULL_HANDLE,
                    VK_NULL_HANDLE);
                result = presentGeneratedFrames(vk, queue, swapchain, next_chain, imageIdx,
                    genCount, true);
            }
        }
    } catch (...) {
        this->pacer.markFrame(now);
        this->fidx++;
        throw;
    }

    this->pacer.markFrame(now);
    this->fidx++;
    recordFrameStats(genCount,
        this->profile.adaptive && this->lastIngest && genCount == 0,
        this->profile.target_fps, this->profile.adaptive);
    if (this->logPresentsRemaining > 0)
        this->logPresentsRemaining--;
    return result;
}

VkResult Swapchain::queuePresentOriginal(const vk::Vulkan& vk, VkQueue queue,
        VkSwapchainKHR swapchain, void* next_chain, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores,
        const VkPresentInfoKHR* originalInfo,
        VkSemaphore extraWait, bool replaceAppWaits) {
    std::vector<VkSemaphore> waits;
    if (replaceAppWaits) {
        if (extraWait)
            waits.push_back(extraWait);
    } else {
        waits = semaphores;
        if (originalInfo && originalInfo->pWaitSemaphores && originalInfo->waitSemaphoreCount) {
            waits.assign(originalInfo->pWaitSemaphores,
                originalInfo->pWaitSemaphores + originalInfo->waitSemaphoreCount);
        }
        if (extraWait)
            waits.push_back(extraWait);
    }

    VkResult res = VK_SUCCESS;
    if (originalInfo && originalInfo->swapchainCount == 1) {
        VkPresentInfoKHR info = *originalInfo;
        info.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
        info.pWaitSemaphores = waits.empty() ? nullptr : waits.data();
        res = vk.df().QueuePresentKHR(queue, &info);
    } else {
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = next_chain,
            .waitSemaphoreCount = static_cast<uint32_t>(waits.size()),
            .pWaitSemaphores = waits.empty() ? nullptr : waits.data(),
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &imageIdx,
        };
        res = vk.df().QueuePresentKHR(queue, &presentInfo);
    }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");
    return res;
}

void Swapchain::waitFence(const vk::Vulkan& vk, const vk::Fence& fence, bool& inFlight) {
    if (!inFlight)
        return;
    if (!fence.wait(vk, 150ULL * 1000 * 1000))
        throw ls::vulkan_error(VK_TIMEOUT, "vkWaitForFences() failed");
    fence.reset(vk);
    inFlight = false;
}

void Swapchain::copyToSource(const vk::Vulkan& vk, VkImage swapchainImage,
        const std::vector<VkSemaphore>& waitSemaphores, bool signalSync,
        VkSemaphore copyDone, VkFence fence) {
    const auto& sourceImage = this->sourceImages.at(this->fidx % 2);
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
    std::vector<VkSemaphore> signals;
    if (copyDone)
        signals.push_back(copyDone);
    cmdbuf.submit(vk,
        waitSemaphores, VK_NULL_HANDLE, 0,
        signals,
        signalSync ? this->syncSemaphore->handle() : VK_NULL_HANDLE,
        signalSync ? this->idx++ : 0,
        fence);
}

VkResult Swapchain::presentGeneratedFrames(const vk::Vulkan& vk,
        VkQueue queue, VkSwapchainKHR swapchain,
        void* next_chain, uint32_t imageIdx,
        size_t genCount, bool presentRealWithInternalSemaphores) {
    VkResult res = VK_SUCCESS;
    for (size_t i = 0; i < genCount; i++) {
        auto& pcs = this->postCopySemaphores.at(this->idx % this->postCopySemaphores.size());
        auto& destinationImage = this->destinationImages.at(i);
        auto& pass = this->passes.at(i);

        uint32_t aqImageIdx{};
        if (this->logPresentsRemaining > 0)
            layerLog("lsfg-vk: adaptive acquire generated i=" + std::to_string(i));
        res = vk.df().AcquireNextImageKHR(vk.dev(), swapchain,
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

    if (!presentRealWithInternalSemaphores)
        return res;

    auto& lastPCS = this->postCopySemaphores.at((this->idx - 1) % this->postCopySemaphores.size());
    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &lastPCS.second.handle(),
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &imageIdx,
    };
    res = vk.df().QueuePresentKHR(queue, &presentInfo);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");
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
