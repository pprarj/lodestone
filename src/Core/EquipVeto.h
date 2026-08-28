// EquipVeto.h
// Lodestone - Shared SKSE framework
//
// Module: EquipVeto (Core)
//
// Refuses an item on an actor at the point where the engine equips it, and
// puts a replacement on instead. No equip means no unequip, no reactive loop,
// and no window in which the actor wears something it should not.
//
// The consumer registers per (actor, item) from Papyrus. Nothing here decides
// WHICH items - that is policy, and policy lives in the consumer.
//
// PUBLIC API (declared in Lodestone.psc, "Lodestone" script), since 1.16.0:
//
//   Bool Function BlockEquip(Actor akActor, Form akItem, Form akSubstitute, Form akOwner) global native
//   Bool Function UnblockEquip(Actor akActor, Form akItem) global native
//   Int  Function ClearEquipBlocks(Actor akActor) global native
//   Bool Function IsEquipBlocked(Actor akActor, Form akItem) global native
//   Int  Function GetEquipBlockCount() global native
//
// ---------------------------------------------------------------------------
// THE TARGET, AND THE ONE THAT LOOKED RIGHT AND WAS NOT
// ---------------------------------------------------------------------------
//
// `ActorEquipManager::EquipObject`, RELOCATION_ID(37938, 38894), non-virtual,
// so an inline hook. Everything equips through it: the player's own menu, a
// command from the trade menu, `EquipItem` from a script, a follower AI's
// autonomous re-evaluation, and every NPC dressing itself on a cell load.
//
// `Actor::AddWornItem` is the trap. It is virtual, returns bool, sits at
// vtable slot 0x57, and has exactly the shape a veto wants - and it is NOT ON
// THE EQUIP PATH. Measured across three sessions: one call in thirteen minutes
// of equipping and unequipping, and zero while a blocked item went onto the
// player's head. Choosing a target by its signature without asking whether it
// is CALLED is how that hour gets spent twice.
//
// ---------------------------------------------------------------------------
// WHY A SUBSTITUTE IS NOT OPTIONAL
// ---------------------------------------------------------------------------
//
// `EquipObject` returns void, so a refusal is SILENT. The AI picks the best
// item it OWNS before trying to equip anything, is never told the attempt
// failed, and goes on picking the same blocked item on every pass.
//
// With no substitute the actor keeps whatever its base outfit put on earlier
// in the same pass and NEVER upgrades to the allowed piece it already carries.
// Measured on a follower: given a blocked helmet, he stopped equipping the
// allowed one he owned, for the rest of the session.
//
// Passing None as akSubstitute is legal and means exactly that. It is the
// consumer's call, and it should be a deliberate one.
//
// ---------------------------------------------------------------------------
// PERSISTENCE, AND WHY akOwner EXISTS
// ---------------------------------------------------------------------------
//
// Blocks survive a save. They did not at first, and not persisting had a
// measured cost: for roughly 15 seconds after every load nothing was blocked,
// because a consumer cannot register before Papyrus gets control - and a cell
// load is exactly when the engine runs its equip passes. That window cannot be
// closed from the consumer's side. It does not exist yet.
//
// Persisting alone would have reintroduced what the no-persistence rule was
// protecting against: a block left behind by an uninstalled mod, stopping a
// player equipping an item forever with no clue as to why.
//
// So every block names a Form the CONSUMER's plugin defines. On load, SKSE's
// ResolveFormID maps saved FormIDs onto the current load order, and a form
// whose plugin is gone does not resolve - so an uninstalled mod's blocks die
// during the load itself, exactly, with no timer and no heuristic, while an
// installed mod's blocks are live before its scripts wake up.
//
// Measured 2026-08-27, production-style install into an existing save:
// 209 ms from `loaded 5 of 5 block(s)` to a refusal WITH its substitution, and
// the consumer's first BlockEquip 15 seconds later.
//
// It is the same trick Core/MultiChannel uses to tell contributors apart -
// identity derived from a Form the consumer owns.
//
// ---------------------------------------------------------------------------
// NOT AVAILABLE IN VR
// ---------------------------------------------------------------------------
//
// The hook is not installed on a VR runtime and the log says so. The natives
// exist and the registry accepts calls; nothing is refused. RELOCATION_ID of
// two arguments assigns the SE id to the VR slot rather than refusing, and the
// precedent in Core/Incapacitation is that a guessed VR address installs
// quietly on the wrong function.
//
// ---------------------------------------------------------------------------
// COST
// ---------------------------------------------------------------------------
//
// With nothing registered the hot path is one relaxed atomic read and nothing
// else - indistinguishable from the plugin not being installed. That matters
// here more than in most modules: a cell load was measured at 58 calls in 7 ms,
// every NPC in the area dressing itself, and none of it concerns any consumer.
//
// With blocks registered it is a hash lookup on (actor, item) under a shared
// lock. `EquipObject` is called from SEVERAL THREADS - measured by the thread
// ids in this module's own log lines - which is why the registry is guarded by
// a shared_mutex rather than assuming the game thread.
//
// A blocked item can be attempted SEVERAL TIMES per pass rather than once.
// Measured four. It does not track biped slot count: a single-slot body piece
// also produced four while other single-slot pieces produced two. What governs
// it is not known.
//
// Phase: equip veto. Probe stage closed 2026-08-27; nine consumer-driven test
// rounds, zero native errors.

#pragma once

namespace Lodestone::Core::EquipVeto
{
	// Reads Data\SKSE\Plugins\Lodestone.ini, section [EquipVeto], and installs
	// the hook. Called from plugin.cpp on kDataLoaded, for the same reason
	// every other hook in this plugin is.
	//
	// Cannot fail in a way the caller can act on - every failure path logs what
	// happened and leaves the game running vanilla. The log always says which
	// of the two it is, installed or not installed, because from the outside
	// those are indistinguishable.
	void Install();

	// Registers the natives on the "Lodestone" script. Returns false if any
	// registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);

	// The cosave entry points, driven by Core/Serialization - see
	// Serialization.h for why one owner dispatches for the whole plugin.
	//
	// None of the four throws.
	void CosaveSave(SKSE::SerializationInterface* a_intfc);
	void CosaveLoadBegin();
	bool CosaveLoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type);
	void CosaveRevert();
}
