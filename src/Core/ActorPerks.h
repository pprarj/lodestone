// ActorPerks.h
// Lodestone - Shared SKSE framework
//
// Module: ActorPerks
// Perks on a SPECIFIC actor instance, for Papyrus: apply, remove and query a
// perk on the actor reference without touching the actor base. Papyrus
// Actor.AddPerk never reaches an NPC (it is a no-op outside the player), so a
// follower build that hands out perks needs this path. Stateless: no hook, no
// cosave, nothing cached - the perk lives in the engine's own actor-instance
// storage and follows the reference.
//
// Papyrus-facing script: Lodestone.psc
//
// Phase L-P2

#pragma once

namespace Lodestone::Core::ActorPerks
{
	// Registers this module's native functions with the Papyrus VM.
	// Called by Lodestone::Core::Papyrus::Register - never called directly.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
