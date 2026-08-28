// Papyrus.cpp
// Lodestone - Shared SKSE framework
//
// Implementation of the native registration dispatcher.
//
// Phase 16 - Stage B.1 / B.2

#include "Papyrus.h"

#include "BookFramework.h"
#include "CastTime.h"
#include "ChannelInfo.h"
#include "Detection.h"
#include "DetectionRead.h"
#include "EquipVeto.h"
#include "Incapacitation.h"
#include "MagicScaling.h"
#include "PluginInfo.h"
#include "SpellTomes.h"

namespace Lodestone::Core::Papyrus
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
		//   ok &= ActorValues::RegisterFuncs(a_vm);    // C.4 - actor values
		ok &= PluginInfo::RegisterFuncs(a_vm);
		ok &= CastTime::RegisterFuncs(a_vm);       // C.1/L0 - cast time
		ok &= BookFramework::RegisterFuncs(a_vm);  // C.2/L1 - book text
		ok &= SpellTomes::RegisterFuncs(a_vm);     // C.3/L2 - spell tomes
		ok &= MagicScaling::RegisterFuncs(a_vm);   // L3 - magic scaling
		ok &= Detection::RegisterFuncs(a_vm);      // L-A - detection scaling
		ok &= DetectionRead::RegisterFuncs(a_vm);  // L-B1/L-B1b - detection reading
		ok &= ChannelInfo::RegisterFuncs(a_vm);    // L0 - channel diagnostics
		ok &= Incapacitation::RegisterFuncs(a_vm); // Hook A - managed knockout
		ok &= EquipVeto::RegisterFuncs(a_vm);      // 1.16.0 - equip veto

		if (ok) {
			spdlog::info("Papyrus: all modules registered.");
		} else {
			spdlog::error("Papyrus: one or more modules failed to register.");
		}

		return ok;
	}
}
