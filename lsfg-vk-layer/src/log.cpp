/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "log.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace lsfgvk::layer {
    namespace {
        FILE* openLogFile() {
            const char* path = std::getenv("LSFGVK_LOG");
            std::string resolved;
            if (path && path[0] != '\0') {
                resolved = path;
            } else if (const char* cfg = std::getenv("LSFGVK_CONFIG")) {
                resolved = cfg;
                const auto slash = resolved.find_last_of('/');
                if (slash == std::string::npos)
                    resolved.clear();
                else
                    resolved.resize(slash + 1);
                resolved += "lsfg-vk-layer.log";
            }
            if (resolved.empty())
                return nullptr;
            return std::fopen(resolved.c_str(), "a");
        }
    }

    void layerLog(const std::string& message) {
        std::fputs(message.c_str(), stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);

        static std::mutex mutex;
        const std::lock_guard<std::mutex> lock(mutex);
        static FILE* file = nullptr;
        static bool tried = false;
        if (!tried) {
            tried = true;
            file = openLogFile();
        }
        if (!file)
            return;
        std::fputs(message.c_str(), file);
        std::fputc('\n', file);
        std::fflush(file);
    }
}
