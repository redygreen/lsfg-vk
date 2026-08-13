/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "adaptive.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <utility>

using lsfgvk::layer::AdaptivePacer;
using Clock = AdaptivePacer::Clock;
using namespace std::chrono_literals;

namespace {
    int failures{};

    void expect(bool ok, const char* what) {
        if (ok)
            return;
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }

    AdaptivePacer::Sample step(AdaptivePacer& pacer, double target, Clock::time_point t,
            double waitSec = 0.0, size_t maxGen = 3) {
        return pacer.choose(target, maxGen, t, waitSec);
    }

    std::pair<double, double> meanGen(double target, double dtMs, int n,
            double waitSec = 0.0) {
        AdaptivePacer pacer;
        auto t = Clock::time_point(Clock::duration{0});
        pacer.markPresentReturned(t);
        double sum = 0.0;
        int counted = 0;
        int extrasFrames = 0;
        for (int i = 0; i < n; ++i) {
            t += std::chrono::microseconds(static_cast<int>(dtMs * 1000.0));
            const auto s = step(pacer, target, t, waitSec);
            pacer.markPresentReturned(t);
            if (i < 8)
                continue;
            sum += static_cast<double>(s.genCount);
            ++counted;
            if (s.genCount > 0)
                ++extrasFrames;
        }
        const double mean = counted ? sum / static_cast<double>(counted) : 0.0;
        const double frac = counted
            ? static_cast<double>(extrasFrames) / static_cast<double>(counted)
            : 0.0;
        return {mean, frac};
    }
}

int main() {
    const auto t0 = Clock::time_point(Clock::duration{0});

    {
        AdaptivePacer pacer;
        expect(step(pacer, 90, t0).genCount == 0, "first present is passthrough");
        pacer.markPresentReturned(t0);

        auto s = step(pacer, 90, t0 + 22ms);
        expect(s.genCount == 0, "first 22 ms at 90 banks a 0.98 extra");
        pacer.markPresentReturned(t0 + 22ms);

        s = step(pacer, 90, t0 + 44ms);
        expect(s.genCount == 1, "remainder then inserts the extra");
    }

    {
        AdaptivePacer pacer;
        pacer.markPresentReturned(t0);
        auto s = step(pacer, 90, t0 + 16ms);
        expect(s.genCount == 0, "16 ms frame does not start FG");
        pacer.markPresentReturned(t0 + 16ms);
        s = step(pacer, 90, t0 + 24ms);
        expect(s.genCount == 0, "fast follow-up does not fire leftover extras");
    }

    {
        AdaptivePacer pacer;
        pacer.markPresentReturned(t0);
        auto s = step(pacer, 90, t0 + 22ms, 0.011);
        expect(s.genCount == 0, "22 ms interval with 11 ms acquire wait is not a 45 Hz game");
        expect(s.workDt < 0.012, "work dt subtracts acquire wait");
    }

    {
        AdaptivePacer vsyncBound;
        vsyncBound.markPresentReturned(t0);
        expect(step(vsyncBound, 90, t0 + 11ms).genCount == 0, "already at target, no extras");
    }

    {
        AdaptivePacer pacer;
        pacer.markPresentReturned(t0);
        auto t = t0;
        for (int i = 0; i < 6; ++i) {
            t += 22ms;
            step(pacer, 90, t);
            pacer.markPresentReturned(t);
        }
        const double emaBefore = pacer.choose(90, 3, t).emaDt;
        t += 4ms;
        auto s = step(pacer, 90, t);
        expect(s.genCount == 0, "4 ms burst does not generate");
        expect(std::abs(s.emaDt - emaBefore) < 0.0001, "4 ms burst does not poison EMA");
        pacer.markPresentReturned(t);
        t += 22ms;
        expect(step(pacer, 90, t).genCount == 1, "FG resumes after 4 ms burst");
    }

    {
        AdaptivePacer pacer;
        pacer.markPresentReturned(t0);
        auto t = t0;
        for (int i = 0; i < 4; ++i) {
            t += 22ms;
            step(pacer, 90, t);
            pacer.markPresentReturned(t);
        }
        expect(pacer.choose(90, 3, t).genCount == 1, "trained 45 Hz is generating");
        auto s = step(pacer, 90, t + 47ms);
        expect(s.genCount == 1, "47 ms hitch holds last genCount instead of jumping");
    }

    {
        AdaptivePacer pacer;
        pacer.markPresentReturned(t0);
        auto s = step(pacer, 90, t0 + 200ms);
        expect(s.genCount == 0, "loading hitch passthrough");
    }

    {
        AdaptivePacer pacer;
        pacer.markPresentReturned(t0);
        auto t = t0;
        for (int i = 0; i < 4; ++i) {
            t += 22ms;
            step(pacer, 90, t);
            pacer.markPresentReturned(t);
        }
        t += 200ms;
        expect(step(pacer, 90, t).genCount == 0, "loading hitch skips extras");
        pacer.markPresentReturned(t);
        t += 22ms;
        expect(step(pacer, 90, t).genCount == 1, "FG resumes after loading hitch");
    }

    {
        AdaptivePacer pacer;
        pacer.markPresentReturned(t0);
        using namespace std::chrono;
        auto t = t0 + duration_cast<Clock::duration>(duration<double>(1.0 / 45.0));
        (void)step(pacer, 90, t);
        pacer.markPresentReturned(t);
        t += duration_cast<Clock::duration>(duration<double>(1.0 / 45.0));
        expect(step(pacer, 90, t).genCount == 1, "exact 45 Hz settles at 1 extra");
        pacer.markPresentReturned(t);
        t += duration_cast<Clock::duration>(duration<double>(1.0 / 45.0));
        (void)step(pacer, 120, t);
        pacer.markPresentReturned(t);
        t += duration_cast<Clock::duration>(duration<double>(1.0 / 45.0));
        expect(step(pacer, 120, t).genCount == 2, "120 target steps to 2");
        pacer.markPresentReturned(t);
        for (int i = 0; i < 6; ++i) {
            t += 22ms;
            auto s = step(pacer, 120, t);
            expect(s.genCount <= 2, "120 target does not run to ceiling");
            pacer.markPresentReturned(t);
        }
    }

    {
        const auto [meanFifo, fracFifo] = meanGen(90, 22.222, 90, 0.011);
        expect(meanFifo < 0.05 && fracFifo < 0.05,
            "FIFO-bound 22 ms interval with 11 ms wait does not generate");

        const auto [mean90, frac90] = meanGen(90, 22.222, 90);
        expect(mean90 > 0.9 && mean90 < 1.1, "45 Hz game at 90 target averages ~1 extra");
        expect(frac90 > 0.98, "exact 45 Hz at 90 is a full extra every frame");

        const auto [mean47, frac47] = meanGen(90, 1000.0 / 47.0, 140);
        const double extras47 = 90.0 / 47.0 - 1.0;
        expect(std::abs(mean47 - extras47) < 0.06,
            "47 Hz game at 90 target averages ~0.915 extras (43 generated, not 47)");
        expect(frac47 > 0.85 && frac47 < 0.97,
            "47 Hz game at 90 target skips extras on some frames");

        const auto [mean60, frac60] = meanGen(60, 22.222, 90);
        expect(std::abs(mean60 - 1.0 / 3.0) < 0.12,
            "45 Hz game at 60 target averages ~0.33 extras (not stuck at 0)");
        expect(frac60 > 0.15 && frac60 < 0.55,
            "45 Hz game at 60 target inserts extras on some frames");

        const auto [mean70, frac70] = meanGen(70, 22.222, 90);
        expect(std::abs(mean70 - (70.0 / 45.0 - 1.0)) < 0.12,
            "45 Hz game at 70 target averages ~0.56 extras");
        expect(frac70 > 0.4 && frac70 < 0.75,
            "45 Hz game at 70 target mixes extras instead of full 2x");

        const auto [mean50, _] = meanGen(50, 22.222, 90);
        expect(mean50 > 0.02 && mean50 < 0.25,
            "45 Hz game at 50 target inserts a few extras");
    }

    if (failures != 0) {
        std::cerr << failures << " adaptive pacer checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "adaptive pacer checks OK\n";
    return EXIT_SUCCESS;
}
