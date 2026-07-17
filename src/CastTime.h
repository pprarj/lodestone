// CastTime.h
// Intelligence Matters - SKSE plugin
//
// Module: CastTime
// Replaces external plugin as the source of dynamic cast time (Stage C.1).
//
// CONTRACT - unchanged from the external plugin era, which is the entire point of C.1:
//
//   Papyrus side (IM_CastTime.psc - NOT modified by this stage):
//     Recalculate() computes a multiplier and an offset and writes them into
//     two globals living in IntelligenceMatters_CastTime.esp:
//       IM_CT_Multiplier   local FormID 0x002
//       IM_CT_Offset       local FormID 0x003
//
//   Native side (this module):
//     On every player charge, castingTimer = castingTimer * mult + offset.
//
//   external plugin used to own the native half, reading those same globals through its
//   JSON config. The JSON is now dead weight and the external DLL is gone.
//
// WHY GLOBALS AND NOT A NATIVE (settled in Part 1; recorded here so it is not
// relitigated): only the player has Papyrus-side state - INT, mental fatigue,
// spell tier, Transcendence all live in IM_Core / IM_MentalFatigue /
// IM_Transcendence. The globals are the Papyrus -> native CHANNEL, not a
// "player-only" channel. The future NPC path (C.5) will not read them at all;
// it computes from the actor's school skill inside the hook. Adding NPCs later
// fills the `else` branch below - it does not touch the player path:
//
//     ResolveCastTime(actor, spell, vanillaChargeTime):
//         if actor == player:  read the globals          <- C.1, this stage
//         else:                passthrough (1.0 / 0.0)   <- C.5, out of scope
//
// Keeping the globals also buys: zero change to IM_CastTime.psc and
// IM_Transcendence.psc, one source of truth shared by MCM / debug / DLL, save
// serialization for free, and silent degradation if the DLL fails to load.
//
// This module registers NO Papyrus natives, so unlike PluginInfo it does NOT
// plug into the Papyrus.cpp dispatcher. It hooks the engine directly and is
// installed from plugin.cpp on kDataLoaded.
//
// Phase 16 - Stage C.1 / Part 2

#pragma once

namespace Lodestone::CastTime
{
	// Resolves the IM_CT_* globals and swaps one entry of the ActorMagicCaster
	// vtable. Must be called exactly once - NOT idempotent.
	//
	// Call site: plugin.cpp, on SKSE::MessagingInterface::kDataLoaded.
	//
	// kDataLoaded is required, not merely convenient. The vtable swap alone
	// would work earlier, but LookupForm cannot resolve the globals until plugin
	// data is loaded, and both happen here.
	//
	// A vtable swap needs no address library ID and no trampoline - the VTABLE
	// constant in CommonLibSSE-NG already carries the SE / AE / VR variants.
	//
	// Never throws. Every failure path is logged and swallowed, and leaves the
	// game at vanilla cast time.
	void Install();
}
