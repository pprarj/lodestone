// PluginInfo.cpp
// Intelligence Matters - SKSE plugin
//
// Native implementations behind Lodestone.psc.
//
// API CONVENTION (Stage B.3) - applies to every native in this plugin:
//   - Native functions NEVER let a C++ exception cross into the Papyrus VM.
//     An exception escaping into Papyrus is undefined behavior and can take the
//     game down. Any fallible native wraps its body and returns a sentinel.
//   - Errors are reported by RETURN VALUE, never by throwing:
//       Int    -> -1
//       String -> "" (empty)
//       Bool   -> false
//     The sentinel for each function is documented in Lodestone.psc, which is the
//     contract the Papyrus side reads.
//   - Every public native gets a comment stating what it returns and when it fails.
//
// The two functions here are infallible (they read compile-time constants), so
// they have no error path. They exist to establish the pattern and to prove the
// Papyrus <-> native channel works end to end.
//
// Phase 16 - Stage B.2

#include "PluginInfo.h"

#include "Version.h"

namespace Lodestone::PluginInfo
{
	namespace
	{
		// Lodestone.GetVersion() -> Int
		//
		// Returns the packed DLL version: major * 1000000 + minor * 1000 + patch.
		// Example: 1.0.0 -> 1000000
		//
		// Intended as a presence + minimum-version guard from Papyrus. If the DLL
		// is missing entirely, the Papyrus call fails at the VM level and Papyrus
		// yields 0, which is below any real version - so a simple
		// "GetVersion() >= required" check covers both "absent" and "too old".
		//
		// Cannot fail.
		std::int32_t GetVersion(RE::StaticFunctionTag*)
		{
			return Version::kPacked;
		}

		// Lodestone.GetVersionString() -> String
		//
		// Returns the human-readable DLL version, e.g. "1.0.0".
		// For display and logging only - do NOT parse this for version gating,
		// use GetVersion() instead.
		//
		// Cannot fail.
		RE::BSFixedString GetVersionString(RE::StaticFunctionTag*)
		{
			return RE::BSFixedString(Version::kString);
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("PluginInfo: null VM, cannot register natives.");
			return false;
		}

		// The second argument is the Papyrus SCRIPT name, which must match the
		// Scriptname in Lodestone.psc exactly. Papyrus calls them as
		// Lodestone.GetVersion() / Lodestone.GetVersionString().
		a_vm->RegisterFunction("GetVersion", "Lodestone", GetVersion);
		a_vm->RegisterFunction("GetVersionString", "Lodestone", GetVersionString);

		spdlog::info("PluginInfo: natives registered (GetVersion, GetVersionString).");
		return true;
	}
}
