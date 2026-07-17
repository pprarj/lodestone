// Papyrus.cpp
// Lodestone - Shared SKSE framework
//
// Implementation of the native registration dispatcher.
//
// Phase 16 - Stage B.1 / B.2

#include "Papyrus.h"

#include "PluginInfo.h"

namespace Lodestone::Papyrus
{
	bool Register(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("Papyrus: null VM handed to Register - no natives available.");
			return false;
		}

		bool ok = true;

		// --- Module registration list ---
		// Stage C appends one line per module here:
		//   ok &= CastTime::RegisterFuncs(a_vm);       // C.1 - replaces external plugin
		//   ok &= BookFramework::RegisterFuncs(a_vm);  // C.2 - replaces DBF
		//   ok &= SpellTomes::RegisterFuncs(a_vm);     // C.3 - replaces external plugin
		//   ok &= ActorValues::RegisterFuncs(a_vm);    // C.4 - replaces external plugin
		ok &= PluginInfo::RegisterFuncs(a_vm);

		if (ok) {
			spdlog::info("Papyrus: all modules registered.");
		} else {
			spdlog::error("Papyrus: one or more modules failed to register.");
		}

		return ok;
	}
}
