/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>

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
    /// extras = target * interval − 1. Values within 0.15 of an integer snap
    /// to that integer so 47 Hz at 90 stays at a stable 1 extra (x2 cadence)
    /// instead of skipping ~9% of extras (FIFO hitch). Mid-range values still
    /// dither with a remainder. multiplier is a ceiling.
    ///
    /// `ingest` is set only on a dithered skip that will generate soon, so
    /// LSFG temporal state stays warm without running optical-flow at native
    /// rate while already at the target.
    ///
    /// Interval is present-to-present of the game's QueuePresent calls.
    /// A long acquire wait plus already-fast GPU work means the game is
    /// vsync-bound at the target: insert nothing (otherwise 2× locks it at
    /// half refresh). GPU-bound frames (wait ≈ 0, long interval) generate.
    class AdaptivePacer {
    public:
        using Clock = std::chrono::steady_clock;

        struct Sample {
            size_t genCount{0};
            bool ingest{false};
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

            // Vsync-bound at (or above) target: extra presents would lock
            // the game at refresh / (1 + extras).
            if (out.waitDt > 0.003 && workDt * target <= 1.05) {
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
            if (!this->emaDt.has_value())
                this->emaDt = dt;
            else
                this->emaDt = kAlpha * dt + (1.0 - kAlpha) * *this->emaDt;
            this->emaDt = std::clamp(*this->emaDt, 0.001, 0.25);
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

    private:
        std::optional<Clock::time_point> frameAt;
        std::optional<double> emaDt;
        double acc{0.0};
    };

}
