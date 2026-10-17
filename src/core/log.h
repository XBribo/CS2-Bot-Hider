#pragma once

#include "core/cs2_sdk/src/core/log.h"

namespace cs2bh {
namespace log = cs2_sdk::log;
}

#define BH_LOG_DEBUG(...) ::cs2bh::log::Write(::cs2bh::log::Level::Debug, __VA_ARGS__)
#define BH_LOG_INFO(...)  ::cs2bh::log::Write(::cs2bh::log::Level::Info, __VA_ARGS__)
#define BH_LOG_WARN(...)  ::cs2bh::log::Write(::cs2bh::log::Level::Warn, __VA_ARGS__)
#define BH_LOG_ERROR(...) ::cs2bh::log::Write(::cs2bh::log::Level::Error, __VA_ARGS__)
