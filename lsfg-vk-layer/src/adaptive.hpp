/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>

namespace lsfgvk::layer {

    /// Chooses how many interpolated frames to insert for Adaptive FG.
    ///
    /// Interval is the time the game spent *outside* present() — simulation,
    /// render, and the game's own acquire. Extra generated presents are
    /// excluded so genCount cannot feed back into itself and run away to the
    /// multiplier ceiling.
    class AdaptivePacer {
    public:
        using Clock = std::chrono::steady_clock;

        struct Sample {
            size_t genCount{0};
            double gameDt{0.0};
            double emaDt{0.0};
        };

        [[nodiscard]] Sample choose(double targetFps, size_t maxGen, Clock::time_point now) {
            Sample out{};
            if (!this->presentReturnedAt.has_value()) {
                this->adaptiveError = 0.0;
                this->emaDt.reset();
                this->lastGenCount = 0;
                return out;
            }

            const double gameDt = std::chrono::duration<double>(
                now - *this->presentReturnedAt).count();
            out.gameDt = gameDt;

            // Ignore hitches and implausibly fast presents; do not train the EMA.
            if (gameDt < 0.0005 || gameDt > 0.100) {
                this->adaptiveError = 0.0;
                this->emaDt.reset();
                this->lastGenCount = 0;
                return out;
            }

            constexpr double kAlpha = 0.25;
            if (!this->emaDt.has_value())
                this->emaDt = gameDt;
            else
                this->emaDt = kAlpha * gameDt + (1.0 - kAlpha) * *this->emaDt;
            out.emaDt = *this->emaDt;

            const double target = std::max(1.0, targetFps);
            double desired = target * *this->emaDt - 1.0 + this->adaptiveError;
            if (desired < 0.0)
                desired = 0.0;

            auto gen = static_cast<size_t>(std::llround(desired));
            if (gen > maxGen)
                gen = maxGen;

            // At most one generated-frame step per real frame.
            if (gen > this->lastGenCount + 1)
                gen = this->lastGenCount + 1;
            else if (this->lastGenCount > 0 && gen + 1 < this->lastGenCount)
                gen = this->lastGenCount - 1;

            this->adaptiveError = desired - static_cast<double>(gen);
            if (gen == maxGen && this->adaptiveError > 0.0)
                this->adaptiveError = 0.0;

            this->lastGenCount = gen;
            out.genCount = gen;
            return out;
        }

        void markPresentReturned(Clock::time_point now) {
            this->presentReturnedAt = now;
        }

    private:
        std::optional<Clock::time_point> presentReturnedAt;
        std::optional<double> emaDt;
        double adaptiveError{0.0};
        size_t lastGenCount{0};
    };

}
