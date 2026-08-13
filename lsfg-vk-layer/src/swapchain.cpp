/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain.hpp"
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
#include <mutex>
#include <optional>
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
        const VkImageUsageFlags usage = this->info.imageUsage
            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
            | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        const size_t virtualCount = std::max<size_t>(this->info.images.size(), 3);
        this->virtualImages.reserve(virtualCount);
        for (size_t i = 0; i < virtualCount; i++) {
            this->virtualImages.emplace_back(vk, extent, this->info.format, usage);
            this->availableVirtual.push_back(static_cast<uint32_t>(i));
        }

        this->pacerVk = &vk;
        this->running = true;
        this->pacer = std::thread(&Swapchain::pacerMain, this);
    }
}

Swapchain::~Swapchain() {
    this->running = false;
    this->pacerCv.notify_all();
    if (this->pacer.joinable())
        this->pacer.join();
}

VkResult Swapchain::getSwapchainImages(uint32_t* count, VkImage* images) const {
    const uint32_t n = static_cast<uint32_t>(this->virtualImages.size());
    if (images == nullptr) {
        *count = n;
        return VK_SUCCESS;
    }
    if (*count < n) {
        *count = n;
        return VK_INCOMPLETE;
    }
    for (uint32_t i = 0; i < n; i++)
        images[i] = this->virtualImages.at(i).handle();
    *count = n;
    return VK_SUCCESS;
}

VkResult Swapchain::acquireNextImage(const vk::Vulkan& vk, uint64_t timeout,
        VkSemaphore semaphore, VkFence fence, uint32_t* idx) {
    std::unique_lock lock(this->pacerMutex);
    const auto pred = [this] {
        return !this->availableVirtual.empty() || !this->running.load();
    };

    if (timeout == 0) {
        if (!pred())
            return VK_NOT_READY;
    } else if (timeout == UINT64_MAX) {
        this->pacerCv.wait(lock, pred);
    } else {
        if (!this->pacerCv.wait_for(lock, std::chrono::nanoseconds(timeout), pred))
            return VK_TIMEOUT;
    }

    if (!this->running.load())
        return VK_ERROR_OUT_OF_DATE_KHR;
    if (this->availableVirtual.empty())
        return VK_NOT_READY;

    *idx = this->availableVirtual.front();
    this->availableVirtual.erase(this->availableVirtual.begin());
    lock.unlock();

    if (semaphore == VK_NULL_HANDLE && fence == VK_NULL_HANDLE)
        return VK_SUCCESS;

    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount = semaphore ? 1u : 0u,
        .pSignalSemaphores = semaphore ? &semaphore : nullptr
    };
    std::scoped_lock qlock(this->queueMutex);
    auto res = vk.df().QueueSubmit(vk.queue(), 1, &submitInfo, fence);
    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "vkQueueSubmit() failed");
    return VK_SUCCESS;
}

VkResult Swapchain::present(const vk::Vulkan& vk,
        VkQueue queue, VkSwapchainKHR swapchain,
        void* next_chain, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores) {
    this->realSwapchain = swapchain;
    this->presentQueue = queue;
    this->pacerVk = &vk;

    if (this->profile.adaptive)
        return presentAdaptive(vk, queue, imageIdx, semaphores);
    return presentFixed(vk, queue, swapchain, next_chain, imageIdx, semaphores);
}

VkResult Swapchain::presentAdaptive(const vk::Vulkan& vk,
        VkQueue /*queue*/, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores) {
    if (imageIdx >= this->virtualImages.size())
        throw ls::error("adaptive present of unknown virtual image");

    const auto& virtualImage = this->virtualImages.at(imageIdx);
    const auto& sourceImage = this->sourceImages.at(this->fidx % 2);

    if (this->fidx && !this->copyFence->wait(vk, 2ULL * 1000 * 1000 * 1000))
        throw ls::vulkan_error(VK_TIMEOUT, "vkWaitForFences() failed");
    this->copyFence->reset(vk);

    const auto& cmdbuf = *this->renderCommandBuffer;
    cmdbuf.begin(vk);
    cmdbuf.blitImage(vk,
        {
            barrierHelper(virtualImage.handle(),
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
        { virtualImage.handle(), sourceImage.handle() },
        sourceImage.getExtent(),
        {
            barrierHelper(virtualImage.handle(),
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            ),
        }
    );
    cmdbuf.end(vk);

    {
        std::scoped_lock qlock(this->queueMutex);
        cmdbuf.submit(vk,
            semaphores, VK_NULL_HANDLE, 0,
            {}, VK_NULL_HANDLE, 0,
            this->copyFence->handle()
        );
    }
    if (!this->copyFence->wait(vk, 2ULL * 1000 * 1000 * 1000))
        throw ls::vulkan_error(VK_TIMEOUT, "vkWaitForFences() failed");

    const RealFrame frame{
        .fidx = this->fidx,
        .t = SteadyClock::now()
    };
    this->fidx++;

    {
        std::scoped_lock lock(this->pacerMutex);
        this->pendingFrames.push_back(frame);
        this->availableVirtual.push_back(imageIdx);
    }
    this->pacerCv.notify_all();

    if (this->pacerStatus.load() != VK_SUCCESS)
        return this->pacerStatus.load();
    return VK_SUCCESS;
}

void Swapchain::pacerMain() noexcept {
    try {
        const double targetFps = std::max(1.0F, this->profile.target_fps);
        const auto targetDt = std::chrono::duration<double>(1.0 / targetFps);
        auto next = SteadyClock::now();

        std::optional<RealFrame> prev;
        std::optional<RealFrame> curr;
        size_t gensThisPair{0};
        bool presentedCurr{false};
        bool prepassReady{false};
        const size_t maxGen = this->destinationImages.size();

        while (this->running.load()) {
            {
                std::unique_lock lock(this->pacerMutex);
                this->pacerCv.wait_until(lock, next, [this] {
                    return !this->running.load();
                });
            }
            if (!this->running.load())
                break;

            {
                std::scoped_lock lock(this->pacerMutex);
                while (!this->pendingFrames.empty()) {
                    prev = curr;
                    curr = this->pendingFrames.front();
                    this->pendingFrames.pop_front();
                    gensThisPair = 0;
                    presentedCurr = false;
                    prepassReady = false;
                }
            }

            const auto now = SteadyClock::now();
            if (this->pacerVk && this->presentQueue && this->realSwapchain && curr) {
                const vk::Vulkan& vk = *this->pacerVk;
                auto presentSrc = [&](size_t realFidx) {
                    presentOutput(vk, this->sourceImages.at(realFidx % 2).handle(),
                        VK_NULL_HANDLE, 0, false);
                };

                if (!prev) {
                    if (!presentedCurr) {
                        presentSrc(curr->fidx);
                        presentedCurr = true;
                    }
                } else {
                    const double pairDt = std::chrono::duration<double>(curr->t - prev->t).count();
                    if (pairDt < 0.0005 || pairDt > 0.100) {
                        if (!presentedCurr) {
                            presentSrc(curr->fidx);
                            presentedCurr = true;
                        }
                    } else {
                        const double alpha = std::chrono::duration<double>(now - curr->t).count()
                            / pairDt;
                        if (alpha >= 1.0 || gensThisPair >= maxGen) {
                            if (!presentedCurr) {
                                presentSrc(curr->fidx);
                                presentedCurr = true;
                            }
                        } else if (alpha > 0.001) {
                            if (!prepassReady) {
                                this->instance.get().schedulePrepass(this->ctx.get(), curr->fidx);
                                prepassReady = true;
                            }
                            const uint64_t sig = this->instance.get().scheduleOne(
                                this->ctx.get(), static_cast<float>(alpha));
                            presentOutput(vk, this->destinationImages.at(0).handle(),
                                this->syncSemaphore->handle(), sig, true);
                            gensThisPair++;
                        }
                    }
                }
            }

            next += std::chrono::duration_cast<SteadyClock::duration>(targetDt);
            if (now > next + std::chrono::duration_cast<SteadyClock::duration>(targetDt))
                next = now;
        }
    } catch (const ls::vulkan_error& e) {
        this->pacerStatus = e.error();
    } catch (...) {
        this->pacerStatus = VK_ERROR_UNKNOWN;
    }
}

void Swapchain::presentOutput(const vk::Vulkan& vk, VkImage src,
        VkSemaphore waitTimeline, uint64_t waitValue, bool waitTimelineValid) {
    if (this->passes.empty())
        throw ls::error("adaptive present requires at least one render pass");

    auto& pass = this->passes.at(0);
    auto& pcs = this->postCopySemaphores.at(this->idx % this->postCopySemaphores.size());

    if (this->idx > 1 && !this->renderFence->wait(vk, 2ULL * 1000 * 1000 * 1000))
        throw ls::vulkan_error(VK_TIMEOUT, "vkWaitForFences() failed");
    this->renderFence->reset(vk);

    uint32_t aqImageIdx{};
    auto res = vk.df().AcquireNextImageKHR(vk.dev(), this->realSwapchain,
        100ULL * 1000 * 1000, pass.acquireSemaphore.handle(),
        VK_NULL_HANDLE,
        &aqImageIdx
    );
    if (res == VK_TIMEOUT || res == VK_NOT_READY)
        return;
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw ls::vulkan_error(res, "vkAcquireNextImageKHR() failed");

    const auto& realImage = this->info.images.at(aqImageIdx);
    auto& cmdbuf = pass.commandBuffer;
    cmdbuf.begin(vk);
    cmdbuf.blitImage(vk,
        {
            barrierHelper(src,
                waitTimelineValid ? VK_ACCESS_NONE : VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT,
                waitTimelineValid ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                    : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            ),
            barrierHelper(realImage,
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
            ),
        },
        { src, realImage },
        this->info.extent,
        {
            barrierHelper(realImage,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            ),
        }
    );
    cmdbuf.end(vk);

    const std::vector<VkSemaphore> waitSemaphores{ pass.acquireSemaphore.handle() };
    const std::vector<VkSemaphore> signalSemaphores{ pcs.first.handle() };

    {
        std::scoped_lock qlock(this->queueMutex);
        cmdbuf.submit(vk,
            waitSemaphores,
            waitTimelineValid ? waitTimeline : VK_NULL_HANDLE,
            waitTimelineValid ? waitValue : 0,
            signalSemaphores, VK_NULL_HANDLE, 0,
            this->renderFence->handle()
        );

        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &pcs.first.handle(),
            .swapchainCount = 1,
            .pSwapchains = &this->realSwapchain,
            .pImageIndices = &aqImageIdx,
        };
        res = vk.df().QueuePresentKHR(this->presentQueue, &presentInfo);
    }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw ls::vulkan_error(res, "vkQueuePresentKHR() failed");

    this->idx++;
}

VkResult Swapchain::presentFixed(const vk::Vulkan& vk,
        VkQueue queue, VkSwapchainKHR swapchain,
        void* next_chain, uint32_t imageIdx,
        const std::vector<VkSemaphore>& semaphores) {
    const auto& swapchainImage = this->info.images.at(imageIdx);
    const auto& sourceImage = this->sourceImages.at(this->fidx % 2);
    const size_t genCount = this->destinationImages.size();

    try {
        this->instance.get().scheduleFrames(this->ctx.get(), genCount);
    } catch (const std::exception& e) {
        throw ls::error("failed to schedule frames", e);
    }

    forceFifo(next_chain);

    if (this->fidx && !this->renderFence->wait(vk, 150ULL * 1000 * 1000))
        throw ls::vulkan_error(VK_TIMEOUT, "vkWaitForFences() failed");
    this->renderFence->reset(vk);

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

        auto& cmdbuf = pass.commandBuffer;
        cmdbuf.begin(vk);

        cmdbuf.blitImage(vk,
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
            const auto& prevPCS = this->postCopySemaphores.at((this->idx - 1) % this->postCopySemaphores.size());
            waitSemaphores.push_back(prevPCS.second.handle());
        }

        const std::vector<VkSemaphore> signalSemaphores{
            pcs.first.handle(),
            pcs.second.handle()
        };

        cmdbuf.end(vk);
        cmdbuf.submit(vk,
            waitSemaphores, this->syncSemaphore->handle(), this->idx,
            signalSemaphores, VK_NULL_HANDLE, 0,
            i == genCount - 1 ? this->renderFence->handle() : VK_NULL_HANDLE
        );

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
