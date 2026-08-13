/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "adaptive.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>

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
            double waitSec = 0.0) {
        return pacer.choose(target, 3, t, waitSec);
    }
}

int main() {
    const auto t0 = Clock::time_point(Clock::duration{0});

    {
        AdaptivePacer pacer;
        expect(step(pacer, 90, t0).genCount == 0, "first present is passthrough");
        pacer.markPresentReturned(t0);

        auto s = step(pacer, 90, t0 + 22ms);
        expect(s.genCount == 1, "45 Hz work at 90 target generates 1");
        pacer.markPresentReturned(t0 + 22ms);

        s = step(pacer, 90, t0 + 44ms);
        expect(s.genCount == 1, "stable 45 Hz work stays at 1");
    }

    {
        AdaptivePacer pacer;
        pacer.markPresentReturned(t0);
        auto s = step(pacer, 90, t0 + 16ms);
        expect(s.genCount == 0, "16 ms frame does not start FG");
        pacer.markPresentReturned(t0 + 16ms);
        s = step(pacer, 90, t0 + 24ms);
        expect(s.genCount == 0, "fast follow-up does not error-diffuse into FG");
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
        auto t = t0 + 22ms;
        expect(step(pacer, 90, t).genCount == 1, "start at 1");
        pacer.markPresentReturned(t);
        t += 22ms;
        expect(step(pacer, 120, t).genCount == 2, "120 target steps to 2");
        pacer.markPresentReturned(t);
        for (int i = 0; i < 6; ++i) {
            t += 22ms;
            auto s = step(pacer, 120, t);
            expect(s.genCount <= 2, "120 target does not run to ceiling");
            pacer.markPresentReturned(t);
        }
    }

    if (failures != 0) {
        std::cerr << failures << " adaptive pacer checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "adaptive pacer checks OK\n";
    return EXIT_SUCCESS;
}
