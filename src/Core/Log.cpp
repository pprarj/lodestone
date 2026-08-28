// Log.cpp
// Lodestone - Shared SKSE framework
//
// Implementation of the plugin logger. Logic moved out of plugin.cpp (Stage A)
// with no behavior change: same file name, same destination directory.
//
// Phase 16 - Stage B.1
// Equip veto phase: the log rotates by SESSION - see RotateLogs.

#include "Log.h"

#include "Version.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <filesystem>
#include <format>
#include <string>

namespace Lodestone::Core::Log
{
	namespace
	{
		// How many previous sessions survive. Three, which is what the game's
		// own Papyrus log keeps - matching it means one habit covers both.
		constexpr int kKeptLogs = 3;

		// ROTATES BY SESSION, NOT BY SIZE, and the difference is the whole
		// point.
		//
		// The log used to be truncated on every launch, so the only session you
		// could ever read was the one you just played. That cost a consumer
		// project THREE measurement logs during the equip veto phase: the runs
		// happened, the numbers were real, and the evidence was gone by the
		// time anyone went to look. Papyrus rotates three deep; this did not
		// rotate at all.
		//
		// spdlog ships a rotating_file_sink and it is the WRONG tool here. It
		// rotates when the file grows past a size, so a short session never
		// rotates and a long one rotates in the middle - which splits the thing
		// you are reading and still loses the session you wanted. What was lost
		// was whole sessions, so the boundary has to be the session.
		//
		// Lodestone.log is always the current run. The previous three are
		// Lodestone.1.log (last run) through Lodestone.3.log (three runs ago).
		// The current file keeps its name so that anything pointing at it - the
		// contract's `log` field, a support thread, a bug report template -
		// keeps working.
		//
		// FAILING TO ROTATE MUST NOT COST THE LOG. Every filesystem call takes
		// the error_code overload and nothing throws: if a rename fails because
		// a previous run left a handle open, or the directory is read-only, the
		// sink still opens and truncates exactly as it did before. A degraded
		// log beats no log.
		//
		// Returns a description of what went wrong, or empty on success. It
		// cannot log its own failure - it runs before the logger exists - so
		// the caller reports it once the logger is up.
		std::string RotateLogs(const std::filesystem::path& a_current)
		{
			std::error_code ec;

			const auto dir = a_current.parent_path();
			const auto stem = a_current.stem().string();
			const auto ext = a_current.extension().string();

			const auto nth = [&](int a_n) {
				return dir / std::format("{}.{}{}", stem, a_n, ext);
			};

			// The oldest falls off the end. Missing is not an error.
			std::filesystem::remove(nth(kKeptLogs), ec);

			// Shift the rest down, oldest first so nothing is overwritten
			// before it has moved. A gap in the sequence is fine: renaming a
			// file that is not there sets ec and changes nothing.
			for (int i = kKeptLogs - 1; i >= 1; --i) {
				std::filesystem::rename(nth(i), nth(i + 1), ec);
			}

			if (!std::filesystem::exists(a_current, ec)) {
				// First run, or someone cleaned the folder. Nothing to keep.
				return {};
			}

			std::filesystem::rename(a_current, nth(1), ec);
			if (ec) {
				return std::format("the previous log could not be renamed ({}) - it was overwritten "
								   "instead, as it always used to be",
					ec.message());
			}

			return {};
		}
	}

	bool Init()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			// No log directory - nothing to write to, and no way to report it.
			return false;
		}

		*path /= std::format("{}.log", Version::kProjectName);

		// Before the sink opens, and therefore before anything can be written.
		const auto rotationProblem = RotateLogs(*path);

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

		if (!rotationProblem.empty()) {
			spdlog::warn("Log: {}. The previous session's log is gone.", rotationProblem);
		}

		return true;
	}
}
