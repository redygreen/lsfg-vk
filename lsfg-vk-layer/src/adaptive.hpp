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
    /// Integer extras cannot hit every target from a 45 Hz game (1 extra = 90
    /// displayed). A remainder accumulator spreads extras over time so a 60 FPS
    /// target actually inserts ~0.33 extras/frame instead of sticking at 0 or 1.
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
                this->acc = std::clamp(this->acc * 0.35, -0.25, 0.25);
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

            this->acc += target * *this->emaDt;
            int presents = static_cast<int>(std::lround(this->acc));
            const int maxPresents = static_cast<int>(maxGen) + 1;
            presents = std::clamp(presents, 1, maxPresents);
            this->acc -= static_cast<double>(presents);
            this->acc = std::clamp(this->acc, -1.15, 1.15);

            const size_t gen = static_cast<size_t>(presents - 1);
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
