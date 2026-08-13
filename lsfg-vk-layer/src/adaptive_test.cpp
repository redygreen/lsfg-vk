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
}

int main() {
    AdaptivePacer pacer;
    const auto t0 = Clock::time_point(Clock::duration{0});

    auto s = pacer.choose(90.0, 3, t0);
    expect(s.genCount == 0, "first present is passthrough");

    pacer.markPresentReturned(t0);

    // 45 Hz game work, target 90 → one extra frame. Extra present time is
    // excluded by marking return after the generated presents.
    const auto t1 = t0 + 22ms;
    s = pacer.choose(90.0, 3, t1);
    expect(s.genCount == 1, "45 Hz game at 90 target generates 1");
    pacer.markPresentReturned(t1 + 11ms);

    const auto t2 = t1 + 11ms + 22ms;
    s = pacer.choose(90.0, 3, t2);
    expect(s.genCount == 1, "excluding extra presents does not ramp at 90");
    pacer.markPresentReturned(t2 + 11ms);

    // Target above panel rate should step up once, not jump to the ceiling.
    const auto t3 = t2 + 11ms + 22ms;
    s = pacer.choose(120.0, 3, t3);
    expect(s.genCount == 2, "120 target at 45 Hz game steps to 2");
    pacer.markPresentReturned(t3 + 22ms);

    const auto t4 = t3 + 22ms + 22ms;
    s = pacer.choose(120.0, 3, t4);
    expect(s.genCount <= 2, "120 target stays at 2, does not run to ceiling");
    pacer.markPresentReturned(t4 + 22ms);
    auto t = t4 + 22ms;
    for (int i = 0; i < 8; ++i) {
        t += 22ms;
        s = pacer.choose(120.0, 3, t);
        expect(s.genCount <= 2, "120 target remains below ceiling");
        pacer.markPresentReturned(t + 22ms);
        t += 22ms;
    }

    // Hitch resets to passthrough.
    pacer.markPresentReturned(t4);
    s = pacer.choose(90.0, 3, t4 + 200ms);
    expect(s.genCount == 0, "hitch passthrough");

    // Present-bound ~90 Hz game at 90 target stays at 0.
    AdaptivePacer vsyncBound;
    vsyncBound.markPresentReturned(t0);
    s = vsyncBound.choose(90.0, 3, t0 + 11ms);
    expect(s.genCount == 0, "already at target, no extras");

    if (failures != 0) {
        std::cerr << failures << " adaptive pacer checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "adaptive pacer checks OK\n";
    return EXIT_SUCCESS;
}
