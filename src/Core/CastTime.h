// CastTime.h
// Lodestone - Shared SKSE framework
//
// Module: CastTime (Core)
// Owns the dynamic cast time capability (the external plugin replacement, Stage C.1).
//
// CORE, NOT DOMAIN (Phase L0). This module used to hardcode
// "IntelligenceMatters_CastTime.esp" and resolve its globals by fixed FormID on
// kDataLoaded - a Domain coupling to one specific consumer. Lodestone is a public
// framework used by other mod authors, so that coupling had to go. The module now
// knows no consumer by name: it exposes a registration native and the consumer
// passes its OWN globals in at runtime.
//
// CONTRACT:
//
//   Papyrus side (the CONSUMER owns this - Lodestone ships none of it):
//     The consumer computes a multiplier and an offset from its own game state and
//     writes them into two GlobalVariable records that IT owns. At startup it
//     hands those two globals to Lodestone via the registration native below.
//
//   Native side (this module):
//     On every player charge, castingTimer = castingTimer * mult + offset, reading
//     the value fields of the REGISTERED globals.
//
// PUBLIC API (declared in Lodestone.psc, registered on the "Lodestone" script):
//
//     Bool Function RegisterCastTimeChannel(GlobalVariable akMultiplier, \
//                                            GlobalVariable akOffset) global native
//
//   Maps to the native below. GlobalVariable is the Papyrus type for
//   RE::TESGlobal (verified against the installed CommonLibSSE-NG 3.5.3: a form
//   pointer parameter is registered under its FORMTYPE and unpacked back to the
//   same pointer type - see RE/P/PackUnpack.h). Returns true when the caller's
//   channel is the active one after the call, false otherwise (null argument, or a
//   distinct second registrant rejected by the single-channel policy below).
//
// ORDERING REQUIREMENT (the consumer must honor this):
//   The channel must be registered BEFORE the first cast the consumer wants
//   scaled. The consumer registers early - a load/init event or an init quest, the
//   same place it would have first written the globals. Until registration lands,
//   the hook is pure passthrough (vanilla cast time). This is the same silent
//   degradation the module has always had: "not registered yet" is indistinguishable
//   from "not installed", and both leave the game at vanilla behavior.
//
// SINGLE CHANNEL - v1 DECISION (open for review):
//   v1 implements ONE channel. The first valid registration wins and owns the
//   channel for the process lifetime. A later registration of the SAME two globals
//   is treated as an idempotent refresh (e.g. the consumer re-registers on every
//   game load) and is accepted silently. A registration of DIFFERENT globals while
//   a channel is already held is a genuine second registrant: it is warned in the
//   log and REJECTED (the held channel is unchanged, the native returns false).
//   This mirrors standard SKSE framework posture - external plugin was likewise the single
//   owner of this hook. Rich arbitration between several cast-time mods (chaining,
//   summing, priority) is a documented FUTURE improvement, deliberately NOT built
//   here and NOT a blocker for going public. See CastTime.cpp for the same note at
//   the registration site.
//
// This module has TWO seams into the plugin, because it is both a native provider
// and an engine hook:
//   1. RegisterFuncs(vm) - plugs one line into Core/Papyrus.cpp (the dispatcher),
//      exactly like PluginInfo. This registers the native above.
//   2. Install() - installs the engine hook. Called from plugin.cpp on
//      kDataLoaded, NOT through the dispatcher (a hook has no Papyrus surface and a
//      different lifecycle). This was the pattern the old Domain module used and it
//      is unchanged.
//
// Phase L0 (was Phase 16 - Stage C.1 / Part 2)

#pragma once

namespace Lodestone::Core::CastTime
{
	// Registers this module's native functions with the Papyrus VM.
	// Called by Lodestone::Core::Papyrus::Register - never called directly.
	//
	// Registers: RegisterCastTimeChannel on the "Lodestone" script.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);

	// Installs the engine hook (one entry of the ActorMagicCaster vtable, vfunc
	// 0x14). Must be called exactly once - NOT idempotent.
	//
	// Call site: plugin.cpp, on SKSE::MessagingInterface::kDataLoaded.
	//
	// Unlike the old Domain version, this resolves NO globals: there is nothing to
	// look up until a consumer registers a channel at runtime. kDataLoaded is still
	// the install point because the vtable must exist, and it is the established,
	// safe seam. A vtable swap needs no address library ID and no trampoline - the
	// VTABLE constant in CommonLibSSE-NG already carries the SE / AE / VR variants.
	//
	// Never throws. Every failure path is logged and swallowed, and leaves the game
	// at vanilla cast time.
	void Install();
}
