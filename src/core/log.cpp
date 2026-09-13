#include "core/log.h"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace cs2bh::log {
namespace {
std::shared_ptr<spdlog::logger> g_logger;
}

// Creates the CSS-style synchronous console and file sinks.
bool Init(const char* baseDir, char* error, size_t maxlen)
{
    try
    {
        const auto directory = std::filesystem::path(baseDir) / "addons/BotHider/logs";
        std::filesystem::create_directories(directory);
        std::vector<spdlog::sink_ptr> sinks;
        sinks.emplace_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
        sinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>((directory / "BotHider.log").string(), false));
        sinks[0]->set_pattern("%^[%T] [%l] %n: %v%$");
        sinks[1]->set_pattern("[%Y-%m-%d %T] [%l] %n: %v");
        g_logger = std::make_shared<spdlog::logger>("BotHider", sinks.begin(), sinks.end());
        spdlog::register_logger(g_logger);
        g_logger->set_level(spdlog::level::info);
        g_logger->flush_on(spdlog::level::info);
        spdlog::cfg::load_env_levels();
        return true;
    }
    catch (const std::exception& exception)
    {
        std::snprintf(error, maxlen, "cannot initialize BotHider logger: %s", exception.what());
        Close();
        return false;
    }
}

// Releases only this plugin's logger, without a process-wide shutdown.
void Close()
{
    if (g_logger) g_logger->flush();
    spdlog::drop("BotHider");
    g_logger.reset();
}

// Preserves printf argument handling while adding severity and file output.
void Write(Level level, const char* format, ...)
{
    const auto severity = level == Level::Debug   ? spdlog::level::debug
                          : level == Level::Warn  ? spdlog::level::warn
                          : level == Level::Error ? spdlog::level::err
                                                  : spdlog::level::info;
    if (g_logger && !g_logger->should_log(severity)) return;
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int size = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (size < 0)
    {
        va_end(args);
        return;
    }
    std::vector<char> message(static_cast<size_t>(size) + 1);
    std::vsnprintf(message.data(), message.size(), format, args);
    va_end(args);
    size_t length = static_cast<size_t>(size);
    while (length && (message[length - 1] == '\n' || message[length - 1] == '\r'))
        --length;
    if (g_logger) g_logger->log(severity, spdlog::string_view_t(message.data(), length));
    else
        std::fprintf(stderr, "%s\n", message.data());
}
} // namespace cs2bh::log
