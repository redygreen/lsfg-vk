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
        pacer.markFrame(t);
        double sum = 0.0;
        int counted = 0;
        int extrasFrames = 0;
        for (int i = 0; i < n; ++i) {
            t += std::chrono::microseconds(static_cast<int>(dtMs * 1000.0));
            const auto s = step(pacer, target, t, waitSec);
            pacer.markFrame(t);
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
        pacer.markFrame(t0);

        auto s = step(pacer, 90, t0 + 22ms);
        expect(s.genCount == 1, "first 22 ms at 90 snaps to a stable extra");
        expect(!s.ingest, "stable extra does not ingest");
        pacer.markFrame(t0 + 22ms);

        s = step(pacer, 90, t0 + 44ms);
        expect(s.genCount == 1, "45 Hz at 90 stays at 1 extra");
    }

    {
        AdaptivePacer pacer;
        pacer.markFrame(t0);
        expect(step(pacer, 90, t0 + 22ms, 0.011).genCount == 1,
            "45 Hz with 11 ms FIFO wait still wants x2");
    }

    {
        AdaptivePacer pacer;
        pacer.markFrame(t0);
        auto atTarget = step(pacer, 90, t0 + 11ms);
        expect(atTarget.genCount == 0, "already at target, no extras");
        expect(!atTarget.ingest, "already at target, no ingest");

        AdaptivePacer vsync90;
        vsync90.markFrame(t0);
        expect(step(vsync90, 90, t0 + 11ms, 0.010).genCount == 0,
            "11 ms interval with vsync wait is already at 90");
    }

    {
        AdaptivePacer pacer;
        pacer.markFrame(t0);
        auto t = t0;
        for (int i = 0; i < 6; ++i) {
            t += 22ms;
            step(pacer, 90, t);
            pacer.markFrame(t);
        }
        const double emaBefore = pacer.choose(90, 3, t).emaDt;
        t += 4ms;
        auto s = step(pacer, 90, t);
        expect(s.genCount == 0, "4 ms burst does not generate");
        expect(std::abs(s.emaDt - emaBefore) < 0.0001, "4 ms burst does not poison EMA");
        pacer.markFrame(t);
        t += 22ms;
        expect(step(pacer, 90, t).genCount == 1, "FG resumes after 4 ms burst");
    }

    {
        AdaptivePacer pacer;
        pacer.markFrame(t0);
        auto t = t0;
        for (int i = 0; i < 4; ++i) {
            t += 22ms;
            step(pacer, 90, t);
            pacer.markFrame(t);
        }
        t += 200ms;
        expect(step(pacer, 90, t).genCount == 0, "loading hitch skips extras");
        pacer.markFrame(t);
        t += 22ms;
        expect(step(pacer, 90, t).genCount == 1, "FG resumes after loading hitch");
    }

    {
        AdaptivePacer pacer;
        pacer.markFrame(t0);
        using namespace std::chrono;
        auto t = t0 + duration_cast<Clock::duration>(duration<double>(1.0 / 45.0));
        (void)step(pacer, 90, t);
        pacer.markFrame(t);
        t += duration_cast<Clock::duration>(duration<double>(1.0 / 45.0));
        expect(step(pacer, 90, t).genCount == 1, "exact 45 Hz settles at 1 extra");
        pacer.markFrame(t);
        t += duration_cast<Clock::duration>(duration<double>(1.0 / 45.0));
        (void)step(pacer, 120, t);
        pacer.markFrame(t);
        t += duration_cast<Clock::duration>(duration<double>(1.0 / 45.0));
        expect(step(pacer, 120, t).genCount == 2, "120 target steps to 2");
    }

    {
        const auto [meanFifo, fracFifo] = meanGen(90, 22.222, 90, 0.011);
        expect(meanFifo > 0.9 && fracFifo > 0.98,
            "45 Hz with 11 ms FIFO wait stays at x2");

        const auto [mean90, frac90] = meanGen(90, 22.222, 90);
        expect(mean90 > 0.9 && mean90 < 1.1, "45 Hz game at 90 target averages ~1 extra");
        expect(frac90 > 0.98, "exact 45 Hz at 90 is a full extra every frame");

        const auto [mean47, frac47] = meanGen(90, 1000.0 / 47.0, 140);
        expect(std::abs(mean47 - 1.0) < 0.02,
            "47 Hz game at 90 target stays at a stable 1 extra (no skip hitch)");
        expect(frac47 > 0.98, "47 Hz game at 90 target does not dither skips");

        const auto [mean57, frac57] = meanGen(90, 1000.0 / 57.0, 140);
        expect(mean57 > 0.98 && frac57 > 0.98,
            "57 Hz game at 90 target locks to x2 (not 0/1 dither)");

        const auto [mean60, frac60] = meanGen(60, 22.222, 90);
        expect(mean60 < 0.05 && frac60 < 0.05,
            "45 Hz game at 60 target stays native (0.33 extras is not x2)");

        const auto [mean70, frac70] = meanGen(70, 22.222, 90);
        expect(mean70 > 0.98 && frac70 > 0.98,
            "45 Hz game at 70 target locks to 1 extra (x2 cadence)");
    }

    {
        AdaptivePacer pacer;
        pacer.markFrame(t0);
        auto t = t0;
        int ingestSkips = 0;
        int extras = 0;
        int counted = 0;
        for (int i = 0; i < 40; ++i) {
            t += 16ms;
            const auto s = step(pacer, 90, t);
            pacer.markFrame(t);
            if (i < 8)
                continue;
            ++counted;
            if (s.genCount > 0)
                ++extras;
            if (s.ingest)
                ++ingestSkips;
            expect(s.genCount == 1, "62 Hz at 90 locks to x2, no 0/1 mix");
            expect(!s.ingest, "integer lock does not dither-ingest");
        }
        expect(extras == counted, "62 Hz game at 90 generates every real frame");
        expect(ingestSkips == 0, "no ingest skips at locked x2");
    }

    if (failures != 0) {
        std::cerr << failures << " adaptive pacer checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "adaptive pacer checks OK\n";
    return EXIT_SUCCESS;
}
