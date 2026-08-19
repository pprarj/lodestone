// DetectionRead.h
// Lodestone - Shared SKSE framework
//
// Module: DetectionRead (Core)
// Reports the detection score the engine already keeps for the highest-
// priority observer of a given actor - a read, nothing else.
//
// THIS IS NOT THE Detection MODULE. Core/Detection owns a scaling channel
// that composes registrants and applies the result to nothing - it has no
// idea what the engine's own detection state is. This module has no channel,
// no composition, and no consumer-driven multiplier: it reads one number the
// engine already computed and hands it to Papyrus unchanged. The two live in
// separate files on purpose, so that reading and applying stay two different
// questions - see Pesquisas/LODESTONE_L-B_DETECCAO_PESQUISA.md, section 6.5,
// item 6, and the phase document's own non-scope list.
//
// NO HOOK. RequestHighestDetectionLevelAgainstActor is a REL::Relocation
// call straight into the engine's own function, not a vtable or a detour -
// there is nothing to install, so this module has no Install() and
// plugin.cpp does not mention it. RegisterFuncs() through the dispatcher is
// its entire wiring, same shape as Core/Detection.
//
// WHY A CACHE INSTEAD OF A DIRECT CALL. Whether
// RequestHighestDetectionLevelAgainstActor is safe to call from the thread a
// Papyrus native runs on is not established - no published mod calls it from
// anywhere but the game thread (see the research doc, section 2.4). Rather
// than gamble on an unproven thread, the native never calls the engine
// itself: it returns whatever this module has cached and queues the next
// reading on SKSE::GetTaskInterface(), which always runs on the game thread.
// A call right after a load, before any reading has landed, gets the
// documented cold sentinel - see the FRESHNESS note in Lodestone.psc.
//
// ONE CACHE ENTRY, TWO NATIVES, NEVER DIVERGENT. Both
// GetHighestDetectionLevel and GetDetectionObserverCount come from the same
// engine call and the same cache entry, refreshed together. Neither one can
// report "cold" while the other reports a real number - see ReadCache in the
// .cpp, which both natives call.
//
// PUBLIC API (declared in Lodestone.psc, "Lodestone" script):
//
//   Int Function GetHighestDetectionLevel(Actor akActor) global native
//   Int Function GetDetectionObserverCount(Actor akActor) global native
//
// Phase L-B1 (detection reading)

#pragma once

namespace Lodestone::Core::DetectionRead
{
	// Registers this module's natives on the "Lodestone" script.
	// Registers: GetHighestDetectionLevel, GetDetectionObserverCount.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
