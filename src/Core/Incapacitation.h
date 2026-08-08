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
// NO ENGINE HOOK. Applying and reverting the state is a handful of calls
// already exposed publicly by RE::Actor (SetLifeState, EvaluatePackage,
// InterruptCast, EndInterruptPackage, StopCombat, StopInteractingQuick) -
// not interception of anything. This module therefore has no Install(),
// the same as Detection.
//
// NO NATIVE TIMER. Duration is a balance decision, and CONVENTIONS.md is
// explicit that policy stays out of this DLL. The consumer runs its own
// Papyrus timer (RegisterForSingleUpdate, the same pattern already used
// consumer-side for detection polling) and calls WakeActor when it decides
// the knockout is over. WakeActor also serves the "forced wake" half of the
// contract - it is the same call either way; only who calls it and when
// differs.
//
// V1 SCOPE: no physical ragdoll, no touching knockState/sitSleepState. Those
// fields drive the animation graph and are normally paired with a real
// animation - without one, setting them risks exactly the T-pose this
// module exists to avoid. A knockout with a paired animation is reserved
// for a later module; this one trades a visual flourish for never breaking
// the graph.
//
// PERSISTENCE. The registry survives a save/reload: RegisterSerialization()
// wires a cosave record holding the set of managed FormIDs, nothing else -
// no duration, the consumer's own Papyrus timer owns that and Papyrus state
// already survives a save on its own. SetLifeState is NOT reapplied on
// load. Worst case if the native life state does not survive a save/reload
// on its own: an actor looks awake a little early, and the next WakeActor
// call (idempotent, safe on an unmanaged actor) cleans up normally. It
// never gets stuck forever, which is the one hard requirement here.
//
// PUBLIC API (declared in Lodestone.psc, "Lodestone" script):
//
//   Bool  Function KnockoutActor(Actor akActor) global native
//   Bool  Function WakeActor(Actor akActor) global native
//   Bool  Function IsManagedUnconscious(Actor akActor) global native
//   Bool  Function RegisterForActorWoke(Form akReceiver) global native
//   Bool  Function UnregisterForActorWoke(Form akReceiver) global native
//   Bool  Function RegisterForActorWokeAlias(Alias akAlias) global native
//   Bool  Function UnregisterForActorWokeAlias(Alias akAlias) global native
//   Event OnActorWoke(Actor akActor)
//
// This module has TWO seams, but not the usual pair: RegisterFuncs(vm) plugs
// natives into the dispatcher (Core/Papyrus.cpp) like every module, and
// RegisterSerialization() wires the SKSE cosave from plugin.cpp - a seam no
// other module in this project uses yet. There is no Install(): no engine
// hook exists to install.
//
// Phase: Hook A (managed non-lethal knockout)

#pragma once

namespace Lodestone::Core::Incapacitation
{
	// Registers this module's native functions with the Papyrus VM.
	// Called by Lodestone::Core::Papyrus::Register - never called directly.
	//
	// Registers: KnockoutActor, WakeActor, IsManagedUnconscious,
	// RegisterForActorWoke, UnregisterForActorWoke, RegisterForActorWokeAlias,
	// UnregisterForActorWokeAlias on the "Lodestone" script.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);

	// Wires the SKSE cosave callbacks (save/load/revert) that persist the set
	// of managed-unconscious FormIDs across a save. Must be called exactly
	// once, before the first save/load can happen - see plugin.cpp for the
	// call site (SKSEPluginLoad, right after SKSE::Init).
	//
	// Never throws.
	void RegisterSerialization();
}
