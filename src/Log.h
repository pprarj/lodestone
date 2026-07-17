// Log.h
// Intelligence Matters - SKSE plugin
//
// Owns the plugin's spdlog setup. Extracted from plugin.cpp in Stage B.1 so that
// every module logs the same way, to the same file, without plugin.cpp knowing
// the details.
//
// Log file: Documents/My Games/Skyrim Special Edition/SKSE/IntelligenceMatters.log
//
// Usage from any module (spdlog is pulled in by PCH.h):
//     spdlog::info("something happened: {}", value);
//
// Phase 16 - Stage B.1

#pragma once

namespace Lodestone::Log
{
	// Initializes the plugin log file and installs it as the default logger.
	// Must be called once, first thing in SKSEPluginLoad, before any spdlog:: call.
	//
	// Returns false if the SKSE log directory could not be resolved. In that case
	// no logging is available and the caller should abort plugin load - there is
	// no point continuing blind.
	bool Init();
}
