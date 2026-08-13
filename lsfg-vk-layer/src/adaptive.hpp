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
    /// Uses GPU/CPU work time (present-to-present minus the game's acquire wait)
    /// so extra FIFO presents cannot feed back as a fake 45 FPS cap. Integer
    /// genCount is held with hysteresis to avoid 0/1 flicker (uneven frametime).
    class AdaptivePacer {
    public:
        using Clock = std::chrono::steady_clock;

        struct Sample {
            size_t genCount{0};
            double gameDt{0.0};
            double workDt{0.0};
            double waitDt{0.0};
            double emaDt{0.0};
        };

        [[nodiscard]] Sample choose(double targetFps, size_t maxGen,
                Clock::time_point now, double acquireWaitSec = 0.0) {
            Sample out{};
            out.waitDt = acquireWaitSec < 0.0 ? 0.0 : acquireWaitSec;

            if (!this->presentReturnedAt.has_value()) {
                this->emaDt.reset();
                this->lastGenCount = 0;
                return out;
            }

            const double gameDt = std::chrono::duration<double>(
                now - *this->presentReturnedAt).count();
            out.gameDt = gameDt;

            double workDt = gameDt - out.waitDt;
            if (workDt < 0.0005)
                workDt = gameDt;
            out.workDt = workDt;

            // Loading hitch: passthrough this frame, keep EMA / last gen.
            if (gameDt > 0.080) {
                out.emaDt = this->emaDt.value_or(0.0);
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
            out.emaDt = *this->emaDt;

            const double target = std::max(1.0, targetFps);
            const double extras = target * *this->emaDt - 1.0;
            const size_t gen = pickGen(extras, maxGen);
            this->lastGenCount = gen;
            out.genCount = gen;
            return out;
        }

        void markPresentReturned(Clock::time_point now) {
            this->presentReturnedAt = now;
        }

    private:
        [[nodiscard]] size_t pickGen(double extras, size_t maxGen) const {
            size_t want = 0;
            if (extras > 0.0) {
                auto rounded = static_cast<size_t>(std::llround(extras));
                if (rounded > maxGen)
                    rounded = maxGen;
                want = rounded;
            }

            const size_t last = this->lastGenCount;
            constexpr double kBand = 0.55;
            if (want > last) {
                if (extras >= static_cast<double>(last) + kBand)
                    return last + 1 > maxGen ? maxGen : last + 1;
                return last;
            }
            if (want < last) {
                if (last > 0 && extras <= static_cast<double>(last) - kBand)
                    return last - 1;
                return last;
            }
            return want > maxGen ? maxGen : want;
        }

        std::optional<Clock::time_point> presentReturnedAt;
        std::optional<double> emaDt;
        size_t lastGenCount{0};
    };

}
