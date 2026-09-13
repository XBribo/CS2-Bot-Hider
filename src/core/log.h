#pragma once

#include <cstddef>

namespace cs2bh::log {
enum class Level
{
    Debug,
    Info,
    Warn,
    Error
};

// Opens the console and file sinks before plugin initialization.
bool Init(const char* baseDir, char* error, size_t maxlen);
// Flushes and releases the plugin logger after hook cleanup.
void Close();
// Formats existing printf-style diagnostics for the shared logger.
void Write(Level level, const char* format, ...);
} // namespace cs2bh::log

#define BH_LOG_DEBUG(...) ::cs2bh::log::Write(::cs2bh::log::Level::Debug, __VA_ARGS__)
#define BH_LOG_INFO(...)  ::cs2bh::log::Write(::cs2bh::log::Level::Info, __VA_ARGS__)
#define BH_LOG_WARN(...)  ::cs2bh::log::Write(::cs2bh::log::Level::Warn, __VA_ARGS__)
#define BH_LOG_ERROR(...) ::cs2bh::log::Write(::cs2bh::log::Level::Error, __VA_ARGS__)
