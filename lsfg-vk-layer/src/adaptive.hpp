/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>
#include <thread>

namespace lsfgvk::layer {

    /// Wait time of the game's own vkAcquireNextImageKHR (not layer extras).
    struct GameAcquireTiming {
        static void noteWait(double seconds) { waitSec = seconds; }
        static double takeWait() {
            const double w = waitSec;
            waitSec = 0.0;
            return w;
        }
    private:
        static inline double waitSec{0.0};
    };

    /// How many interpolated frames to insert before this real present.
    ///
    /// extras = target × interval − 1, remainder-dithered so 60 Hz at 90
    /// averages 0.5 extras (30 generated), not a sticky 2×. Values within
    /// 0.15 of an integer snap (47 Hz at 90 stays at 1 extra) so FIFO does
    /// not hitch on a 9% skip. multiplier is a ceiling.
    ///
    /// `ingest` is set on a dithered skip that will generate soon, so LSFG
    /// temporal state stays warm without optical-flow at native rate.
    ///
    /// FIFO wait from our own extras is not "already at target": 16 ms
    /// present-to-present with an 11 ms acquire wait still wants Adaptive
    /// extras. Only a present interval already at the target drops them.
    ///
    /// Sleep after a generated extra (so Gamescope can show it on its own
    /// vsync) sits inside the next present-to-present interval. Training
    /// EMA on that interval snaps extrasWant to 1 and locks Adaptive at
    /// 45+45. `notePacing` holds EMA and still dithers from native frame
    /// time. A true 45 Hz game trains EMA on unpaced presents first.
    class AdaptivePacer {
    public:
        using Clock = std::chrono::steady_clock;

        struct Sample {
            size_t genCount{0};
            bool ingest{false};
            bool pacedHold{false};
            double extrasWant{0.0};
            double gameDt{0.0};
            double workDt{0.0};
            double waitDt{0.0};
            double emaDt{0.0};
            double acc{0.0};
        };

        [[nodiscard]] Sample choose(double targetFps, size_t maxGen,
                Clock::time_point now, double acquireWaitSec = 0.0) {
            Sample out{};
            out.waitDt = acquireWaitSec < 0.0 ? 0.0 : acquireWaitSec;
            out.acc = this->acc;
            out.emaDt = this->emaDt.value_or(0.0);
            const bool pacedPrev = this->lastPacedSec > 0.0005;
            this->lastPacedSec = 0.0;
            out.pacedHold = pacedPrev;

            if (!this->frameAt.has_value()) {
                this->acc = 0.0;
                out.genCount = 0;
                return out;
            }

            const double dt = std::chrono::duration<double>(now - *this->frameAt).count();
            out.gameDt = dt;

            double workDt = dt - out.waitDt;
            if (workDt < 0.0005)
                workDt = dt;
            out.workDt = workDt;

            const double target = std::clamp(targetFps, 30.0, 240.0);

            // Loading hitch, or a same-timestamp probe.
            if (dt > 0.100 || dt < 0.001) {
                out.genCount = 0;
                return out;
            }

            // Already presenting at the target (vsync-bound at 90, etc.).
            // Use present-to-present dt, not workDt: extra FIFO presents add
            // ~1/target acquire wait and would otherwise turn Adaptive off.
            if (out.waitDt > 0.003 && dt * target <= 1.08) {
                this->acc *= 0.35;
                out.acc = this->acc;
                out.genCount = 0;
                return out;
            }

            // Too fast to be a real game frame. Do not train EMA (a burst of
            // 3–5 ms presents would otherwise collapse extras to 0 forever).
            if (dt * target <= 1.0) {
                out.acc = this->acc;
                out.genCount = 0;
                return out;
            }

            constexpr double kAlpha = 0.2;
            if (!pacedPrev) {
                if (!this->emaDt.has_value())
                    this->emaDt = dt;
                else
                    this->emaDt = kAlpha * dt + (1.0 - kAlpha) * *this->emaDt;
                this->emaDt = std::clamp(*this->emaDt, 0.001, 0.25);
            } else if (!this->emaDt.has_value()) {
                out.genCount = 0;
                return out;
            }
            out.emaDt = *this->emaDt;

            if (maxGen == 0) {
                out.genCount = 0;
                return out;
            }

            const double extrasWant = std::clamp(
                target * *this->emaDt - 1.0, 0.0, static_cast<double>(maxGen));
            out.extrasWant = extrasWant;

            constexpr double kSnap = 0.15;
            const double nearest = std::round(extrasWant);
            if (std::abs(extrasWant - nearest) <= kSnap) {
                out.genCount = static_cast<size_t>(nearest);
                this->acc = 0.0;
                out.acc = 0.0;
                return out;
            }

            this->acc += extrasWant;
            int extra = static_cast<int>(std::floor(this->acc));
            extra = std::clamp(extra, 0, static_cast<int>(maxGen));
            this->acc -= static_cast<double>(extra);
            this->acc = std::clamp(this->acc, 0.0, 0.999);
            out.genCount = static_cast<size_t>(extra);
            out.acc = this->acc;
            out.ingest = extra == 0;
            return out;
        }

        void markFrame(Clock::time_point now) {
            this->frameAt = now;
        }

        /// Call after sleeping one display slot for a generated extra.
        /// The next choose() will not train EMA on that inflated interval.
        void notePacing(double seconds) {
            this->lastPacedSec += std::max(0.0, seconds);
        }

    private:
        std::optional<Clock::time_point> frameAt;
        std::optional<double> emaDt;
        double acc{0.0};
        double lastPacedSec{0.0};
    };

    /// Sleep one Adaptive display slot (1/target_fps).
    ///
    /// Call only after a generated QueuePresent, before the next extra or
    /// the real frame. Skip presents must not sleep: a global 90 Hz grid
    /// turned the following game present into a 2–8 ms "burst" that the
    /// pacer discarded, so extras never caught up to the target.
    inline double sleepDisplaySlot(double targetFps) {
        const double hz = std::clamp(targetFps, 30.0, 240.0);
        const auto slot = std::chrono::duration<double>(1.0 / hz);
        const auto started = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(slot);
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
    }

}
