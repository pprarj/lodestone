// ActorPerks.cpp
// Lodestone - Shared SKSE framework
//
// Natives behind Lodestone.AddPerkToActor, RemovePerkFromActor and HasPerkOnActor.
//
// INSTANCE, NOT BASE. The engine's actor-level perk API (Actor::AddPerk,
// Actor::RemovePerk, Actor::HasPerk) edits the REFERENCE's own perk list, not
// the actor base form's perk array (TESActorBase::perks). The Papyrus
// Actor.AddPerk native never reaches an NPC - the CK wiki records "This does
// nothing for NPCs" - so the only way to hand a follower a perk at runtime is
// this actor-level path. Two actors that share a base stay independent.
//
// READ IS "HAS", NOT "HAS ON THE INSTANCE ONLY". Actor::HasPerk answers the same
// question the vanilla Actor.HasPerk does: instance perks and base-form perks
// together. A perk already on the actor base reads true, and RemovePerkFromActor
// cannot clear it - it only removes the instance copy - so that removal reports
// false because the actor still owns the perk.
//
// The perk follows the reference. Whether a reload keeps it is the reference's
// own save/load, which this module does not measure; the consumer re-applies on
// load.
//
// Phase L-P2

#include "ActorPerks.h"

namespace Lodestone::Core::ActorPerks
{
	namespace
	{
		// Lodestone.AddPerkToActor(Actor, Perk) -> Bool
		bool AddPerkToActor(RE::StaticFunctionTag*, RE::Actor* a_actor, RE::BGSPerk* a_perk)
		{
			try {
				if (!a_actor || !a_perk) {
					return false;
				}
				a_actor->AddPerk(a_perk);
				// Report only an add the engine confirms.
				return a_actor->HasPerk(a_perk);
			} catch (...) {
				return false;
			}
		}

		// Lodestone.RemovePerkFromActor(Actor, Perk) -> Bool
		bool RemovePerkFromActor(RE::StaticFunctionTag*, RE::Actor* a_actor, RE::BGSPerk* a_perk)
		{
			try {
				if (!a_actor || !a_perk) {
					return false;
				}
				a_actor->RemovePerk(a_perk);
				// False when the actor still owns the perk: a base-form copy the
				// instance removal cannot reach, or a removal the engine refused.
				return !a_actor->HasPerk(a_perk);
			} catch (...) {
				return false;
			}
		}

		// Lodestone.HasPerkOnActor(Actor, Perk) -> Bool
		bool HasPerkOnActor(RE::StaticFunctionTag*, RE::Actor* a_actor, RE::BGSPerk* a_perk)
		{
			try {
				return a_actor && a_perk && a_actor->HasPerk(a_perk);
			} catch (...) {
				return false;
			}
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("ActorPerks: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("AddPerkToActor", "Lodestone", AddPerkToActor);
		a_vm->RegisterFunction("RemovePerkFromActor", "Lodestone", RemovePerkFromActor);
		a_vm->RegisterFunction("HasPerkOnActor", "Lodestone", HasPerkOnActor);

		spdlog::info("ActorPerks: natives registered (AddPerkToActor, RemovePerkFromActor, HasPerkOnActor).");
		return true;
	}
}
