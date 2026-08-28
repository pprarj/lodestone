// Incapacitation.h
// Lodestone - Shared SKSE framework
//
// Module: Incapacitation (Core)
// Owns a managed non-lethal knockout state for an actor: mark an actor as
// managed-unconscious, query that state, and wake the actor - automatically
// or forced, from the consumer's own Papyrus timer. Built for a stealth
// takedown feature in a consumer mod, but Core never names a consumer:
// nothing here is specific to any particular gameplay design.
//
// WHAT "MANAGED" MEANS. IsManagedUnconscious answers from Lodestone's OWN
// registry, not from RE::Actor::IsUnconscious(). Vanilla stun causes (magic
// paralysis, Unrelenting Force) never touch ACTOR_LIFE_STATE, so the two are
// already distinct in practice - the registry is what makes that distinction
// durable even if that ever changes, and it is what answers "did I do this"
// rather than "is this actor stunned by anything right now".
//
// ONE ENGINE HOOK, AND IT ARRIVED LATE. Most of what this module does is
// plain calls the engine already exposes (SetLifeState, EvaluatePackage,
// the interrupts, KnockExplosion, Update3DModel, UpdateActor3DPosition).
// V1 had no hook at all and said so here.
//
// The physical fall is what changed that. Dropping an actor works; keeping
// it down does not follow from dropping it, because the engine decides on
// its own to stand the actor back up a second or two later. Two versions
// tried to prevent that by describing the actor as down - writing
// knockState - and neither worked: that field is the engine's output, not
// its input. So the module now hooks Actor::InitiateGetUpPackage and
// suppresses it for actors it is holding down, which is a suppressing hook
// under the exception in CONVENTIONS.md. The reasoning, the three
// conditions and the runtime-dependent vtable index are all at the hook
// itself in the .cpp.
//
// NO NATIVE TIMER. Duration is a balance decision, and CONVENTIONS.md is
// explicit that policy stays out of this DLL. The consumer runs its own
// Papyrus timer (RegisterForSingleUpdate, the same pattern already used
// consumer-side for detection polling) and calls WakeActor when it decides
// the knockout is over. WakeActor also serves the "forced wake" half of the
// contract - it is the same call either way; only who calls it and when
// differs.
//
// THE PHYSICAL FALL (added after V1). KnockoutFall drops a managed actor on
// the ground and KnockoutRecover puts it back on its feet. Both are optional
// and additive: a consumer written against V1 that never calls either keeps
// exactly the behavior it already had.
//
// V1 refused to touch knockState/sitSleepState, on the grounds that those
// fields drive the animation graph and a value set without the matching
// transition is how an actor ends up in a T-pose. That reasoning was right
// and is not being reversed here. What changed is the route: the fall goes
// through AIProcess::KnockExplosion, the engine's own knockdown path - the
// one every explosion and every Unrelenting Force in the base game already
// runs - so the engine performs the transition and decides knockState for
// itself. The recovery writes knockState only when the actor is still down
// afterwards, and resyncs the 3D model before letting the AI resume.
//
// THE FALL IS A SEPARATE STATE FROM THE KNOCKOUT, tracked in its own
// in-memory set. The two do not begin or end together, and KnockoutRecover
// works whether or not WakeActor has already run - neither call reads the
// state the other one owns, so they may be issued in either order. An actor
// left permanently on the ground is the failure this module most has to
// avoid, and a recovery that only works in one call order is a recovery
// with a way to be missed.
//
// DEATH. A managed actor that dies is dropped from both sets by a
// TESDeathEvent sink - the first event sink in this plugin. Without it,
// WakeActor was the only path that ever removed a FormID, so killing a
// managed actor left it managed forever: IsManagedUnconscious answered true
// for a corpse and the cosave carried a dead actor between saves with no way
// to drop it. No wake event is dispatched on death, because the actor did
// not wake, and the corpse's pose is left to the engine.
//
// PERSISTENCE. The registry survives a save/reload: a cosave record holds
// the set of managed FormIDs, nothing else -
// no duration, the consumer's own Papyrus timer owns that and Papyrus state
// already survives a save on its own. SetLifeState is NOT reapplied on
// load. Worst case if the native life state does not survive a save/reload
// on its own: an actor looks awake a little early, and the next WakeActor
// call (idempotent, safe on an unmanaged actor) cleans up normally. It
// never gets stuck forever, which is the one hard requirement here.
//
// The fallen set is deliberately NOT persisted and the cosave record is
// unchanged at version 1. knockState is animation state that does not
// survive a save, so an actor comes back from a reload standing up whatever
// the set claimed - persisting it would restore a record of a fall that did
// not survive. The consumer reapplies KnockoutFall after a load to the
// actors it still considers knocked out, which is safe without checking
// anything because KnockoutFall is idempotent.
//
// PUBLIC API (declared in Lodestone.psc, "Lodestone" script):
//
//   Bool  Function KnockoutActor(Actor akActor) global native
//   Bool  Function WakeActor(Actor akActor) global native
//   Bool  Function KnockoutFall(Actor akActor) global native
//   Bool  Function KnockoutRecover(Actor akActor) global native
//   Bool  Function IsManagedUnconscious(Actor akActor) global native
//   Int   Function GetActorLifeState(Actor akActor) global native
//   Bool  Function RegisterForActorWoke(Form akReceiver) global native
//   Bool  Function UnregisterForActorWoke(Form akReceiver) global native
//   Bool  Function RegisterForActorWokeAlias(Alias akAlias) global native
//   Bool  Function UnregisterForActorWokeAlias(Alias akAlias) global native
//   Event OnActorWoke(Actor akActor)
//
// This module now uses all THREE seams: RegisterFuncs(vm) plugs natives into
// the dispatcher (Core/Papyrus.cpp) like every module, the cosave entry
// points below are driven by Core/Serialization - a seam this module was the
// first to need - and Install() registers the death sink on kDataLoaded,
// alongside the modules that install engine hooks there. It is the only
// Install() in the plugin that installs no hook.
//
// Phase: Hook A (managed non-lethal knockout)

#pragma once

namespace Lodestone::Core::Incapacitation
{
	// Registers this module's native functions with the Papyrus VM.
	// Called by Lodestone::Core::Papyrus::Register - never called directly.
	//
	// Registers: KnockoutActor, WakeActor, KnockoutFall, KnockoutRecover,
	// IsManagedUnconscious, GetActorLifeState, RegisterForActorWoke,
	// UnregisterForActorWoke, RegisterForActorWokeAlias,
	// UnregisterForActorWokeAlias on the "Lodestone" script.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);

	// Wires the two things this module needs the game to be up for: the
	// TESDeathEvent sink that drops a dying actor from its sets, and the
	// vtable hook on Actor::InitiateGetUpPackage that keeps a knocked-down
	// actor from standing back up. Called on kDataLoaded from plugin.cpp,
	// alongside the other modules' hook installation.
	//
	// Never throws, and the two halves are independent: either can fail
	// without taking the other with it. Each failure logs what specifically
	// stops working - a dead actor stuck in the registry, or a fall that the
	// engine undoes after a moment - rather than a generic install error,
	// because those are the symptoms someone would otherwise be debugging
	// from the wrong end.
	void Install();

	// THE COSAVE ENTRY POINTS, driven by Core/Serialization.
	//
	// This module used to register them with SKSE itself. It cannot any more,
	// and the reason is a hard constraint rather than a preference: SKSE gives
	// a plugin ONE set of callbacks, and the load callback walks the record
	// stream with GetNextRecordInfo - whoever calls it first consumes every
	// record, so a second module registering its own load callback would see
	// an empty stream. One owner dispatches by record type; that owner is
	// Core/Serialization.
	//
	// What this module persists did not change: same record tag, same version,
	// same bytes, same resolve-or-drop on load.
	//
	// None of the four throws.
	void CosaveSave(SKSE::SerializationInterface* a_intfc);

	// Runs on EVERY load, record present or not - "start empty" is the correct
	// state for a fall that did not travel in the save.
	void CosaveLoadBegin();

	// Returns false when the record is not this module's, so the caller can
	// offer it to the next module.
	bool CosaveLoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type);

	void CosaveRevert();
}
