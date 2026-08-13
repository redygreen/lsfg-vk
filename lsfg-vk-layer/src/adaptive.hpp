/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>

namespace lsfgvk::layer {

    /// Timing of the game's own vkAcquireNextImageKHR (not layer-internal acquires).
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

    /// Chooses how many interpolated frames to insert for Adaptive FG.
    ///
    /// extrasWant = target * realInterval - 1, so 47 real at 90 target yields
    /// ~0.915 extras/frame (~43 generated). That is floor(T / targetDt) - 1
    /// plus a remainder so fractional extras average correctly (16 ms at 90
    /// needs 0.44 extras, not 0).
    ///
    /// Generate exactly that many frames between the last two *consecutive*
    /// real sources (timestamps (i+1)/(n+1)). Always generating the multiplier
    /// ceiling would put extras at 1/4, 2/4, 3/4 instead of 1/2 when only one
    /// extra is shown, and the extra GPU work lowers real FPS further from
    /// target.
    ///
    /// Acquire wait is used only to detect FIFO/vsync lock: if the game is
    /// blocked on the display and GPU work already meets the target, extras
    /// drop to 0 so we do not cap the game at half refresh. GPU-bound games
    /// (wait ~ 0, long interval) keep generating.
    class AdaptivePacer {
    public:
        using Clock = std::chrono::steady_clock;

        struct Sample {
            size_t genCount{0};
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
            out.genCount = this->lastGenCount;

            if (!this->presentReturnedAt.has_value()) {
                this->emaDt.reset();
                this->lastGenCount = 0;
                this->acc = 0.0;
                out.genCount = 0;
                out.acc = 0.0;
                return out;
            }

            const double gameDt = std::chrono::duration<double>(
                now - *this->presentReturnedAt).count();
            out.gameDt = gameDt;

            double workDt = gameDt - out.waitDt;
            if (workDt < 0.0005)
                workDt = gameDt;
            out.workDt = workDt;

            const double target = std::clamp(targetFps, 30.0, 240.0);

            if (gameDt > 0.120) {
                out.genCount = 0;
                return out;
            }

            // Waiting on FIFO/vsync, and render work already meets target:
            // do not generate (avoids locking the game at refresh / (1+extras)).
            const bool displayBound = out.waitDt > 0.003 && workDt * target <= 1.05;
            if (displayBound) {
                this->acc *= 0.35;
                this->lastGenCount = 0;
                out.acc = this->acc;
                out.genCount = 0;
                return out;
            }

            // This present already met the target. Do not train EMA on it —
            // a burst of 3–5 ms returns would collapse extras to 0 forever.
            // Ignore gameDt == 0 (same-timestamp probes / clock ties).
            if (gameDt > 1e-6 && gameDt * target <= 1.0) {
                this->lastGenCount = 0;
                out.genCount = 0;
                out.acc = this->acc;
                return out;
            }

            if (this->emaDt.has_value()) {
                const double ema = *this->emaDt;
                if (gameDt < 0.003 || (gameDt > 0.040 && gameDt > ema * 1.6)) {
                    out.emaDt = ema;
                    out.genCount = this->lastGenCount;
                    return out;
                }
            } else if (gameDt < 0.003 || gameDt > 0.040) {
                out.genCount = 0;
                return out;
            }

            constexpr double kAlpha = 0.2;
            if (!this->emaDt.has_value())
                this->emaDt = gameDt;
            else
                this->emaDt = kAlpha * gameDt + (1.0 - kAlpha) * *this->emaDt;
            this->emaDt = std::clamp(*this->emaDt, 0.001, 0.25);
            out.emaDt = *this->emaDt;

            if (maxGen == 0) {
                this->lastGenCount = 0;
                out.genCount = 0;
                out.acc = this->acc;
                return out;
            }

            const double extrasWant = std::clamp(
                target * *this->emaDt - 1.0, 0.0, static_cast<double>(maxGen));
            this->acc += extrasWant;
            int extra = static_cast<int>(std::floor(this->acc));
            extra = std::clamp(extra, 0, static_cast<int>(maxGen));
            this->acc -= static_cast<double>(extra);
            this->acc = std::clamp(this->acc, 0.0, 0.999);

            const size_t gen = static_cast<size_t>(extra);
            this->lastGenCount = gen;
            out.genCount = gen;
            out.acc = this->acc;
            return out;
        }

        void markPresentReturned(Clock::time_point now) {
            this->presentReturnedAt = now;
        }

    private:
        std::optional<Clock::time_point> presentReturnedAt;
        std::optional<double> emaDt;
        size_t lastGenCount{0};
        double acc{0.0};
    };

}
