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
    /// Original LS Adaptive presents from its own capture window at the
    /// panel rate with fractional timestamps. This layer only runs inside
    /// the game's QueuePresent, so smoothness like Fixed x2 needs a stable
    /// integer extra count (extra-real-extra-real). extrasWant =
    /// target × interval − 1, then locked to 0/1/2/… with hysteresis.
    ///
    /// FIFO wait from our own extras is not "already at target": 45 Hz
    /// with an 11 ms acquire wait still wants x2. Only a present interval
    /// that is already at the target (dt × target ≈ 1) drops extras.
    /// multiplier is a ceiling.
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
                this->locked.reset();
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
                this->locked.reset();
                out.genCount = 0;
                return out;
            }

            // Already presenting at the target (vsync-bound at 90, etc.).
            // Do not use workDt: extra FIFO presents add ~1/target acquire
            // wait at 45 Hz, and treating that as "at target" turns x2 off.
            if (out.waitDt > 0.003 && dt * target <= 1.08) {
                this->acc *= 0.35;
                this->locked.reset();
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

            // Enter the next integer a bit below 0.5 so 55–62 Hz at 90
            // locks to x2 instead of dithering. Leave only when clearly
            // near the target (want ≲ 0.30 for dropping 1 extra).
            if (!this->locked.has_value()) {
                size_t initial = 0;
                if (extrasWant >= 0.40)
                    initial = std::min(maxGen, std::max<size_t>(1,
                        static_cast<size_t>(std::lround(extrasWant))));
                this->locked = initial;
            } else {
                const double cur = static_cast<double>(*this->locked);
                if (extrasWant >= cur + 0.40 && *this->locked < maxGen)
                    this->locked = *this->locked + 1;
                else if (extrasWant <= cur - 0.70 && *this->locked > 0)
                    this->locked = *this->locked - 1;
            }
            out.genCount = *this->locked;
            this->acc = 0.0;
            out.acc = 0.0;
            return out;
        }

        void markFrame(Clock::time_point now) {
            this->frameAt = now;
        }

    private:
        std::optional<Clock::time_point> frameAt;
        std::optional<double> emaDt;
        std::optional<size_t> locked;
        double acc{0.0};
    };

}
