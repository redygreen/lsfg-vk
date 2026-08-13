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
    /// Work time is present-to-present minus the game's acquire wait, so extra
    /// FIFO presents cannot feed back as a fake 45 FPS cap.
    ///
    /// Accumulates extras so displayed FPS approaches the target: a 47 Hz game
    /// at 90 should insert ~0.915 extras/frame (43 generated), not a full 2×.
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

            // Loading screen: skip extras this frame, keep EMA / remainder.
            if (gameDt > 0.120) {
                out.genCount = 0;
                return out;
            }

            // Outlier vs EMA: hold last decision, do not train.
            if (this->emaDt.has_value()) {
                const double ema = *this->emaDt;
                if (workDt < 0.003 || workDt > ema * 2.5 || workDt > 0.040) {
                    out.emaDt = ema;
                    out.genCount = this->lastGenCount;
                    return out;
                }
            } else if (workDt < 0.003 || workDt > 0.040) {
                out.genCount = 0;
                return out;
            }

            constexpr double kAlpha = 0.2;
            if (!this->emaDt.has_value())
                this->emaDt = workDt;
            else
                this->emaDt = kAlpha * workDt + (1.0 - kAlpha) * *this->emaDt;
            this->emaDt = std::clamp(*this->emaDt, 0.001, 0.25);
            out.emaDt = *this->emaDt;

            // This frame already met the target: no extras, bleed remainder so a
            // fast frame cannot fire a leftover extra (old error-diffusion bug).
            if (workDt * target <= 1.05) {
                this->acc = std::clamp(this->acc * 0.35, 0.0, 0.25);
                this->lastGenCount = 0;
                out.acc = this->acc;
                out.genCount = 0;
                return out;
            }

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
