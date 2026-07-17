// Log.cpp
// Lodestone - Shared SKSE framework
//
// Implementation of the plugin logger. Logic moved out of plugin.cpp (Stage A)
// with no behavior change: same file name, same destination directory.
//
// Phase 16 - Stage B.1

#include "Log.h"

#include "Version.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <format>

namespace Lodestone::Log
{
	bool Init()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			// No log directory - nothing to write to, and no way to report it.
			return false;
		}

		*path /= std::format("{}.log", Version::kProjectName);

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));

		// Release builds stay at info; debug builds get everything.
#ifndef NDEBUG
		log->set_level(spdlog::level::trace);
		log->flush_on(spdlog::level::trace);
#else
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
#endif

		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");

		return true;
	}
}
