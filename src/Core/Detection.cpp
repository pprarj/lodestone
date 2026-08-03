// Detection.cpp
// Lodestone - Shared SKSE framework
//
// The detection scaling channel. See Detection.h for what this module is, and
// in particular for what it deliberately is not.
//
// Phase L-A (detection scaling channel)

#include "Detection.h"

namespace Lodestone::Core::Detection
{
	namespace
	{
		MultiChannel g_channel{ "Detection" };

		// -------------------------------------------------------------------
		// Native: Lodestone.RegisterDetectionMultiplierChannel(GlobalVariable,
		//         GlobalVariable) -> Bool
		//
		// Same shape and same error convention as the four scaling channels that
		// came before it: a pair of globals, true when the caller's pair is
		// contributing after the call, false on a null argument, never a throw.
		//
		// Registering here does not by itself change anything in game - this
		// module applies the composed pair to nothing, because the hook that
		// would read the player's detection context does not exist yet. The
		// channel is real: it composes correctly across registrants and reports
		// through the diagnostics. What is missing is a consumer of the result,
		// and Detection.h says why it is missing on purpose.
		// -------------------------------------------------------------------
		bool RegisterDetectionMultiplierChannel(RE::StaticFunctionTag*, RE::TESGlobal* a_multiplier, RE::TESGlobal* a_offset)
		{
			return g_channel.Register(a_multiplier, a_offset);
		}
	}

	MultiChannel& GetChannel()
	{
		return g_channel;
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("Detection: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("RegisterDetectionMultiplierChannel", "Lodestone", RegisterDetectionMultiplierChannel);

		spdlog::info("Detection: natives registered (RegisterDetectionMultiplierChannel). "
					 "The channel composes registrants; nothing applies it to the engine yet.");
		return true;
	}
}
