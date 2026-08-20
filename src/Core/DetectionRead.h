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
// ONE CACHE ENTRY, TWO NATIVES - AND THE GUARANTEE IS NARROWER THAN 1.14.0
// AND 1.15.0 CLAIMED. Both natives of a pair read the same cache entry,
// filled by one engine reading, through one shared function (ReadCache, or
// ReadCacheFiltered for the Excluding pair). WITHIN a single native call the
// level and the count therefore always come from the same reading.
//
// ACROSS TWO CALLS THEY DO NOT. Those two versions said "never divergent",
// flatly, and that was wrong - the promise only ever held inside one call.
// A consumer reads the pair as two separate natives, and the refresh those
// calls queue runs on the game thread, so it can land BETWEEN them:
//
//   VM thread    GetDetectionObserverCountExcluding -> count from reading N
//   game thread  the queued refresh completes       -> entry becomes reading N+1
//   VM thread    GetHighestDetectionLevelExcluding  -> level from reading N+1
//
// and the consumer holds half of each. The pending flag does not prevent
// this: it stops a second TASK from being queued, not the first task from
// finishing mid-pair. Reported from play as a positive level (detected)
// arriving next to a zero count, at the exact instant the reading flipped -
// which is the only moment the two generations differ enough to notice.
//
// NOT FIXED HERE, AND THE REASON IS THAT IT CANNOT BE, INTERNALLY. This
// module has no way to know that two native calls form one logical pair -
// nothing in the call tells it, and inventing a boundary (a time window, a
// latch) would guess. Closing it properly means one native that returns both
// numbers, which is new permanent public surface and a decision for the
// author rather than a quiet fix. Until then the contract is the honest one:
// treat a level and a count read back to back as two samples taken close
// together, not as one atomic reading.
//
// A SECOND, DLL-BUILT AGGREGATE (added in DLL 1.15.0), FOR FILTERING BY
// KEYWORD. The two natives above read one number the engine already
// computed. Nothing about them lets a consumer exclude an observer by type -
// the Int has no identity attached, so there is no candidate to filter out.
// GetHighestDetectionLevelExcluding and GetDetectionObserverCountExcluding
// answer that by building their OWN aggregate: walk every actor in high
// process (ProcessLists::ForEachHighActor), drop the candidates listed below,
// and call Actor::RequestDetectionLevel - the same direction confirmed for
// the unfiltered pair (observer->RequestDetectionLevel(target)) - on what is
// left, keeping the highest level and counting how many of the SURVIVING
// candidates are at or above zero (the same detected/not-detected boundary
// the level itself uses).
//
// WHAT THE WALK SKIPS, and every one of these is here because leaving it out
// produced a wrong reading in play (1.15.1):
//
//   the target itself     - it does not observe itself
//   dead actors           - a corpse stays in high process and still answers
//                           the pairwise query with its last level
//   kDoNotShowOnStealthMeter - the engine's own "must not move the stealth
//                           meter" flag; counting one disagrees with the
//                           vanilla eye the player is looking at
//   the excluded keyword  - the caller's own filter, the reason this pair
//                           exists at all
//
// The first three are not policy. They are the engine's own aggregate's
// behavior, which this one has to reproduce by hand precisely because it does
// not call it.
//
// THIS AGGREGATE CAN STILL DISAGREE WITH THE ENGINE'S OWN, EVEN WITH NO
// KEYWORD EXCLUDED. It walks a different candidate set (high actors this
// module can see) through a different engine call (the pairwise query, not
// the engine's internal aggregate) and reproduces none of whatever internal
// logic RequestHighestDetectionLevelAgainstActor uses beyond taking a
// maximum and the skips above. A consumer comparing GetHighestDetectionLevel
// against GetHighestDetectionLevelExcluding(actor, None) and finding
// different numbers is not necessarily looking at a bug - see the .cpp for
// what is and is not reproduced.
//
// THE OBSERVER COUNT IS NOT A LINE-OF-SIGHT COUNT HERE. The unfiltered pair
// gets its count for free, as the LOSCount the engine's aggregate call
// already returns. RequestDetectionLevel has no such output - getting real
// line of sight per candidate would cost a second engine call
// (Actor::RequestLOS) per survivor, unproven and not part of what this phase
// built. GetDetectionObserverCountExcluding instead counts how many
// keyword-filtered candidates currently detect the target (level >= 0). Read
// it as "how many are detecting", not "how many can see".
//
// THE CACHE KEY GREW A DIMENSION. A filtered reading depends on the PAIR
// (actor, excluded keyword), not the actor alone - two different exclusions
// on the same actor are both legitimate readings at the same time, and
// neither may overwrite the other. The filtered pair keeps its own cache,
// entirely separate from the unfiltered one above; nothing here touches
// g_cache. Keyword None keys as "no exclusion" (FormID 0, which no real form
// ever has) - see the .cpp for why that reading is still not promised to
// match the unfiltered native's.
//
// "NOBODY PASSED THE FILTER" MIRRORS THE ENGINE'S OWN OBSERVED FLOOR
// (-1000), not a value invented for this module - see the .cpp for what is
// and is not known about that number. Choosing anything else would hand the
// consumer a third convention to learn on top of the sentinel and the real
// scale, which the phase this shipped from explicitly ruled out.
//
// PUBLIC API (declared in Lodestone.psc, "Lodestone" script):
//
//   Int Function GetHighestDetectionLevel(Actor akActor) global native
//   Int Function GetDetectionObserverCount(Actor akActor) global native
//   Int Function GetHighestDetectionLevelExcluding(Actor akActor, Keyword akExcludeType) global native
//   Int Function GetDetectionObserverCountExcluding(Actor akActor, Keyword akExcludeType) global native
//
// Phase L-B1 (detection reading), L-B1b (filterable reading), L-B1b fixes
// (dead and stealth-meter-exempt candidates, and the honest pair contract)

#pragma once

namespace Lodestone::Core::DetectionRead
{
	// Registers this module's natives on the "Lodestone" script.
	// Registers: GetHighestDetectionLevel, GetDetectionObserverCount,
	// GetHighestDetectionLevelExcluding, GetDetectionObserverCountExcluding.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
