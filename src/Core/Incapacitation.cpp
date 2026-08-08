// Incapacitation.cpp
// Lodestone - Shared SKSE framework
//
// The managed non-lethal knockout module. See Incapacitation.h for the
// contract and why it has no engine hook and no native timer.
//
// Phase: Hook A (managed non-lethal knockout)

#include "Incapacitation.h"

#include <mutex>
#include <unordered_set>

#include "SKSE/RegistrationSet.h"

namespace Lodestone::Core::Incapacitation
{
	namespace
	{
		// The managed set. A plain mutex is enough here: KnockoutActor and
		// WakeActor are called at most once per takedown, never per frame -
		// unlike SpellTomes' hot read path, there is no case for a lock-free
		// fast path.
		std::mutex                     g_registryLock;
		std::unordered_set<RE::FormID> g_registry;

		// Dispatches OnActorWoke to every registered script. Same shape and
		// same dispatch mechanism (QueueEvent, off the caller's stack) as
		// SpellTomes' OnSpellTomeRead - validated in game there already.
		SKSE::RegistrationSet<RE::Actor*> g_wokeReg{ "OnActorWoke" };

		// Cosave record identity. Multi-char literal, the convention the
		// rest of the SKSE ecosystem uses for a four-byte record tag.
		constexpr std::uint32_t kSerializationID = 'LDST';
		constexpr std::uint32_t kRecordType = 'INC1';
		constexpr std::uint32_t kRecordVersion = 1;

		// -------------------------------------------------------------------
		// Natives
		//
		// Error convention: failure by return value, never by throwing.
		// -------------------------------------------------------------------

		// Lodestone.KnockoutActor(Actor) -> Bool
		//
		// Refuses a null actor, a dead one, one not currently kAlive (already
		// bleeding out, restrained, reanimating, or anything else that is not
		// plain kAlive - stacking on top of another life state is not this
		// module's call to make), and one already in the registry.
		//
		// On success: interrupts whatever the actor is doing (cast, package/
		// dialogue, combat, interaction) before changing the life state, so
		// nothing already committed keeps running past the new state. Order
		// matters for that reason.
		bool KnockoutActor(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			if (!a_actor) {
				spdlog::warn("Incapacitation: KnockoutActor got a None actor - ignored.");
				return false;
			}

			if (a_actor->IsDead()) {
				spdlog::warn("Incapacitation: KnockoutActor called on a dead actor (0x{:08X}) - ignored.",
					a_actor->GetFormID());
				return false;
			}

			if (a_actor->GetLifeState() != RE::ACTOR_LIFE_STATE::kAlive) {
				spdlog::warn("Incapacitation: KnockoutActor called on actor (0x{:08X}) not in kAlive "
							 "(life state {}) - refused, this module does not stack on another life state.",
					a_actor->GetFormID(), static_cast<std::uint32_t>(a_actor->GetLifeState()));
				return false;
			}

			const auto formID = a_actor->GetFormID();

			{
				std::lock_guard lock(g_registryLock);
				if (g_registry.contains(formID)) {
					spdlog::warn("Incapacitation: KnockoutActor called on actor (0x{:08X}) already managed - ignored.",
						formID);
					return false;
				}
			}

			try {
				a_actor->InterruptCast(true);
				a_actor->EndInterruptPackage(false);
				a_actor->StopCombat();
				a_actor->StopInteractingQuick(true);
				a_actor->SetLifeState(RE::ACTOR_LIFE_STATE::kUnconcious);

				std::lock_guard lock(g_registryLock);
				g_registry.insert(formID);
			} catch (...) {
				spdlog::error("Incapacitation: KnockoutActor threw applying state to actor (0x{:08X}) - "
							  "not registered as managed.",
					formID);
				return false;
			}

			spdlog::info("Incapacitation: actor (0x{:08X}) knocked out.", formID);
			return true;
		}

		// Lodestone.WakeActor(Actor) -> Bool
		//
		// The same call serves both halves of the contract - "wake
		// automatically after a time" and "wake forced": the difference is
		// only who calls this and when, never how the state is reverted.
		// Safe and idempotent on an actor this module was never managing:
		// removes nothing, reverts nothing, returns false, never throws.
		bool WakeActor(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			if (!a_actor) {
				return false;
			}

			const auto formID = a_actor->GetFormID();

			bool wasManaged = false;
			{
				std::lock_guard lock(g_registryLock);
				wasManaged = g_registry.erase(formID) > 0;
			}

			if (!wasManaged) {
				return false;
			}

			if (!a_actor->IsDead()) {
				try {
					if (a_actor->GetLifeState() == RE::ACTOR_LIFE_STATE::kUnconcious) {
						a_actor->SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);
					}
					// Forces the engine to re-evaluate the AI package even if
					// the life state had already reverted through some other
					// path - this is what keeps the actor from coming back
					// inert instead of resuming normal behavior.
					a_actor->EvaluatePackage(true, true);
				} catch (...) {
					spdlog::error("Incapacitation: WakeActor threw reverting state on actor (0x{:08X}) - "
								  "registry entry already removed.",
						formID);
					// Best-effort past this point: the registry entry is
					// already gone, so this call cannot leave the actor
					// permanently stuck as far as this module is concerned.
				}
			}

			spdlog::info("Incapacitation: actor (0x{:08X}) woke.", formID);
			g_wokeReg.QueueEvent(a_actor);
			return true;
		}

		// Lodestone.IsManagedUnconscious(Actor) -> Bool
		//
		// Answers from this module's own registry, never from
		// RE::Actor::IsUnconscious() - see Incapacitation.h for why that
		// distinction is the point of this native.
		bool IsManagedUnconscious(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			if (!a_actor) {
				return false;
			}

			std::lock_guard lock(g_registryLock);
			return g_registry.contains(a_actor->GetFormID());
		}

		// Lodestone.RegisterForActorWoke(Form) -> Bool
		bool RegisterForActorWoke(RE::StaticFunctionTag*, RE::TESForm* a_receiver)
		{
			if (!a_receiver) {
				spdlog::warn("Incapacitation: RegisterForActorWoke got a None form - ignored.");
				return false;
			}
			return g_wokeReg.Register(a_receiver);
		}

		// Lodestone.UnregisterForActorWoke(Form) -> Bool
		bool UnregisterForActorWoke(RE::StaticFunctionTag*, RE::TESForm* a_receiver)
		{
			if (!a_receiver) {
				return false;
			}
			return g_wokeReg.Unregister(a_receiver);
		}

		// Lodestone.RegisterForActorWokeAlias(Alias) -> Bool
		//
		// Same reasoning as SpellTomes' RegisterForSpellTomeReadAlias: a
		// ReferenceAlias script is bound to the alias handle, which a
		// Form-keyed registration cannot reach.
		bool RegisterForActorWokeAlias(RE::StaticFunctionTag*, RE::BGSBaseAlias* a_alias)
		{
			if (!a_alias) {
				spdlog::warn("Incapacitation: RegisterForActorWokeAlias got a None alias - ignored.");
				return false;
			}
			return g_wokeReg.Register(a_alias);
		}

		// Lodestone.UnregisterForActorWokeAlias(Alias) -> Bool
		bool UnregisterForActorWokeAlias(RE::StaticFunctionTag*, RE::BGSBaseAlias* a_alias)
		{
			if (!a_alias) {
				return false;
			}
			return g_wokeReg.Unregister(a_alias);
		}

		// -------------------------------------------------------------------
		// Cosave callbacks
		//
		// Persist ONLY the set of managed FormIDs - no duration, the
		// consumer's own Papyrus timer owns that and Papyrus state already
		// survives a save on its own. See Incapacitation.h, PERSISTENCE.
		// -------------------------------------------------------------------

		void SaveCallback(SKSE::SerializationInterface* a_intfc)
		{
			std::lock_guard lock(g_registryLock);

			if (!a_intfc->OpenRecord(kRecordType, kRecordVersion)) {
				spdlog::error("Incapacitation: failed to open the save record - {} managed actor(s) not saved.",
					g_registry.size());
				return;
			}

			const auto count = static_cast<std::uint32_t>(g_registry.size());
			if (!a_intfc->WriteRecordData(count)) {
				spdlog::error("Incapacitation: failed to write the managed-actor count - save record incomplete.");
				return;
			}

			for (const auto formID : g_registry) {
				if (!a_intfc->WriteRecordData(formID)) {
					spdlog::error("Incapacitation: failed to write a FormID (0x{:08X}) - save record incomplete.",
						formID);
					return;
				}
			}

			spdlog::info("Incapacitation: saved {} managed actor(s).", count);
		}

		void LoadCallback(SKSE::SerializationInterface* a_intfc)
		{
			std::lock_guard lock(g_registryLock);
			g_registry.clear();

			std::uint32_t type = 0;
			std::uint32_t version = 0;
			std::uint32_t length = 0;

			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type != kRecordType) {
					// Not this module's record - another module (or a future
					// one) may share the cosave. Skip it and keep scanning.
					continue;
				}

				std::uint32_t count = 0;
				if (a_intfc->ReadRecordData(count) != sizeof(count)) {
					spdlog::error("Incapacitation: failed to read the managed-actor count - load abandoned.");
					return;
				}

				std::uint32_t loaded = 0;
				for (std::uint32_t i = 0; i < count; ++i) {
					RE::FormID oldFormID = 0;
					if (a_intfc->ReadRecordData(oldFormID) != sizeof(oldFormID)) {
						spdlog::error("Incapacitation: failed to read a FormID at index {} of {} - load abandoned.",
							i, count);
						return;
					}

					RE::FormID newFormID = 0;
					if (a_intfc->ResolveFormID(oldFormID, newFormID)) {
						g_registry.insert(newFormID);
						++loaded;
					} else {
						spdlog::warn("Incapacitation: FormID 0x{:08X} from the save did not resolve - "
									 "dropped (form no longer exists in this load order).",
							oldFormID);
					}
				}

				spdlog::info("Incapacitation: loaded {} of {} managed actor(s) from the save.", loaded, count);
			}
		}

		void RevertCallback(SKSE::SerializationInterface*)
		{
			std::lock_guard lock(g_registryLock);
			g_registry.clear();
			spdlog::info("Incapacitation: registry cleared (new game or return to main menu).");
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("Incapacitation: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("KnockoutActor", "Lodestone", KnockoutActor);
		a_vm->RegisterFunction("WakeActor", "Lodestone", WakeActor);
		a_vm->RegisterFunction("IsManagedUnconscious", "Lodestone", IsManagedUnconscious);
		a_vm->RegisterFunction("RegisterForActorWoke", "Lodestone", RegisterForActorWoke);
		a_vm->RegisterFunction("UnregisterForActorWoke", "Lodestone", UnregisterForActorWoke);
		a_vm->RegisterFunction("RegisterForActorWokeAlias", "Lodestone", RegisterForActorWokeAlias);
		a_vm->RegisterFunction("UnregisterForActorWokeAlias", "Lodestone", UnregisterForActorWokeAlias);

		spdlog::info("Incapacitation: natives registered (KnockoutActor, WakeActor, IsManagedUnconscious, "
					 "RegisterForActorWoke, UnregisterForActorWoke, RegisterForActorWokeAlias, "
					 "UnregisterForActorWokeAlias).");
		return true;
	}

	void RegisterSerialization()
	{
		auto* intfc = SKSE::GetSerializationInterface();
		if (!intfc) {
			spdlog::error("Incapacitation: no serialization interface - managed knockout state will not "
						  "survive a save/reload this session.");
			return;
		}

		intfc->SetUniqueID(kSerializationID);
		intfc->SetSaveCallback(SaveCallback);
		intfc->SetLoadCallback(LoadCallback);
		intfc->SetRevertCallback(RevertCallback);

		spdlog::info("Incapacitation: cosave registered (id 'LDST', record 'INC1').");
	}
}
