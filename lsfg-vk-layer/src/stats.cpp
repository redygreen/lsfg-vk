/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "stats.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace lsfgvk::layer {
    namespace {
        using Clock = std::chrono::steady_clock;

        std::string statsPath() {
            const char* path = std::getenv("LSFGVK_STATS");
            if (path && path[0] != '\0')
                return path;
            const char* cfg = std::getenv("LSFGVK_CONFIG");
            if (!cfg || cfg[0] == '\0')
                return {};
            std::string resolved = cfg;
            const auto slash = resolved.find_last_of('/');
            if (slash == std::string::npos)
                return {};
            resolved.resize(slash + 1);
            resolved += "stats.json";
            return resolved;
        }

        void writeAtomically(const std::string& path, const std::string& body) {
            const std::string tmp = path + ".tmp";
            FILE* file = std::fopen(tmp.c_str(), "w");
            if (!file)
                return;
            std::fputs(body.c_str(), file);
            std::fflush(file);
            std::fclose(file);
            std::rename(tmp.c_str(), path.c_str());
        }
    }

    void recordFrameStats(size_t genCount, bool ingest, float targetFps, bool adaptive) {
        static std::mutex mutex;
        const std::lock_guard<std::mutex> lock(mutex);

        static uint64_t realFrames{};
        static uint64_t generatedFrames{};
        static uint64_t ingestFrames{};
        static uint64_t fgFrames{};
        static Clock::time_point windowStart{};
        static bool started{};

        const auto now = Clock::now();
        if (!started) {
            windowStart = now;
            started = true;
        }

        realFrames += 1;
        generatedFrames += genCount;
        if (ingest)
            ingestFrames += 1;
        if (genCount > 0 || ingest)
            fgFrames += 1;

        const double elapsed = std::chrono::duration<double>(now - windowStart).count();
        if (elapsed < 1.0)
            return;

        const double realFps = static_cast<double>(realFrames) / elapsed;
        const double generatedFps = static_cast<double>(generatedFrames) / elapsed;
        const double ingestFps = static_cast<double>(ingestFrames) / elapsed;
        const double fgFps = static_cast<double>(fgFrames) / elapsed;
        const double displayedFps = realFps + generatedFps;
        const double avgGen = realFrames
            ? static_cast<double>(generatedFrames) / static_cast<double>(realFrames)
            : 0.0;

        char json[640];
        std::snprintf(json, sizeof(json),
            "{\"real_fps\":%.2f,\"generated_fps\":%.2f,\"displayed_fps\":%.2f,"
            "\"ingest_fps\":%.2f,\"fg_fps\":%.2f,\"avg_gen\":%.2f,\"target_fps\":%.2f,\"adaptive\":%s,"
            "\"real_frames\":%llu,\"generated_frames\":%llu,\"window_ms\":%.0f}\n",
            realFps, generatedFps, displayedFps, ingestFps, fgFps, avgGen,
            static_cast<double>(targetFps),
            adaptive ? "true" : "false",
            static_cast<unsigned long long>(realFrames),
            static_cast<unsigned long long>(generatedFrames),
            elapsed * 1000.0);

        const std::string path = statsPath();
        if (!path.empty())
            writeAtomically(path, json);

        realFrames = 0;
        generatedFrames = 0;
        ingestFrames = 0;
        fgFrames = 0;
        windowStart = now;
    }
}
