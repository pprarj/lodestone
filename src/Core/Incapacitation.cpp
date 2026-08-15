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

		// The actors this module has physically knocked down, guarded by the
		// same lock. Deliberately a SECOND set rather than a flag on the first:
		// "managed-unconscious" and "physically on the ground" are not the same
		// state and do not begin or end together. KnockoutRecover has to work
		// after WakeActor already removed the actor from g_registry, or the two
		// natives could only ever be called in one order.
		//
		// NOT PERSISTED, on purpose - see the cosave section below for the
		// reasoning and what the consumer has to do about it.
		std::unordered_set<RE::FormID> g_fallen;

		// How far above the actor's own origin the knockdown impulse comes
		// from, in Skyrim units, and how hard it hits.
		//
		// These are NOT balance numbers and never become parameters. The
		// consumer has exactly one requirement of this call - the target goes
		// down - and no opinion about how hard. A float on the signature would
		// turn "make it work" into "decide how it looks", which is policy, and
		// policy does not live in this DLL. Duration is the balance decision
		// here and it is already the consumer's, as it was in V1.
		//
		// UNMEASURED. Both values are conservative starting points, not
		// observations: KnockExplosion pushes away from a_location, so an
		// origin above the actor drives the impulse downward - a collapse
		// rather than a launch - and a small magnitude fails by doing too
		// little rather than by throwing a body across the room. The
		// instrumented log below exists to calibrate them in game. Do not
		// read these as tuned.
		constexpr float kFallOriginHeight = 64.0F;
		constexpr float kFallImpulse = 1.0F;

		// Dispatches OnActorWoke to every registered script. Same shape and
		// same dispatch mechanism (QueueEvent, off the caller's stack) as
		// SpellTomes' OnSpellTomeRead - validated in game there already.
		SKSE::RegistrationSet<RE::Actor*> g_wokeReg{ "OnActorWoke" };

		// Cosave record identity. Multi-char literal, the convention the
		// rest of the SKSE ecosystem uses for a four-byte record tag.
		constexpr std::uint32_t kSerializationID = 'LDST';
		constexpr std::uint32_t kRecordType = 'INC1';
		constexpr std::uint32_t kRecordVersion = 1;

		// Reads an actor's life state.
		//
		// NEVER call a_actor->GetLifeState() directly, and the same goes for
		// every other member inherited from ActorState, MagicTarget or
		// ActorValueOwner. ActorState is a base class of Actor in the headers,
		// but this DLL is built for SE, AE and VR at once, and in that
		// configuration the compile-time layout is a stub that matches no
		// runtime: Actor.h asserts sizeof(Actor) == 0xC0 for the multi-runtime
		// build against 0x2B0 (SE, VR) and 0x2B8 (AE) for the real object, so
		// no base subobject sits where the running game keeps it. Reaching one
		// through the C++ hierarchy reads a wrong address that the compiler
		// accepts and that nothing reports.
		//
		// The library provides the correct route and uses it itself
		// (see src/RE/A/Actor.cpp, which never touches an inherited accessor
		// directly). Actor declares:
		//
		//   RUNTIME_CAST_ACCESSOR_VERSIONED(ActorState, AsActorState,
		//                                   SKSE::RUNTIME_SSE_1_6_629, 0xB8, 0xC0)
		//
		// which resolves the offset from the runtime version at call time.
		//
		// This is not a precaution written in advance. Version 1.10.0 shipped
		// the direct call and every read landed on the same wrong address:
		// KnockoutActor refused every actor in the game with a constant "life
		// state 7", the same number for a chicken at 5/5 health and a
		// blacksmith at 131/131, across six actors, two saves and four play
		// sessions. A constant unrelated to the actor is the signature of this
		// mistake.
		RE::ACTOR_LIFE_STATE ReadLifeState(RE::Actor* a_actor)
		{
			return a_actor->AsActorState()->GetLifeState();
		}

		// Reads an actor's knock state. Same accessor rule and the same reason
		// as ReadLifeState above: knockState is a bitfield on ActorState, and
		// ActorState is one of the base subobjects that does not sit where the
		// multi-runtime compile-time layout claims.
		//
		//   ActorState.h:116   KNOCK_STATE_ENUM knockState: 3
		//   ActorState.h:52    kNormal 0, kExplode 1, kExplodeLeadIn 2, kOut 3,
		//                      kOutLeadIn 4, kQueued 5, kGetUp 6, kDown 7,
		//                      kWaitForTaskQueue 8
		RE::KNOCK_STATE_ENUM ReadKnockState(RE::Actor* a_actor)
		{
			return a_actor->AsActorState()->GetKnockState();
		}

		// Is this knock state one where the actor is on the ground, or on the
		// way there? kNormal means upright and kGetUp means already standing
		// back up, so neither needs reverting.
		//
		// The library documents none of these values, so this grouping is read
		// off the names and nothing else. It is used only to decide whether
		// KnockoutRecover writes to the field at all - being wrong about a
		// value here makes the recovery a no-op on that state, not a corruption.
		bool IsDownKnockState(RE::KNOCK_STATE_ENUM a_state)
		{
			switch (a_state) {
			case RE::KNOCK_STATE_ENUM::kExplode:
			case RE::KNOCK_STATE_ENUM::kExplodeLeadIn:
			case RE::KNOCK_STATE_ENUM::kOut:
			case RE::KNOCK_STATE_ENUM::kOutLeadIn:
			case RE::KNOCK_STATE_ENUM::kQueued:
			case RE::KNOCK_STATE_ENUM::kDown:
				return true;
			default:
				return false;
			}
		}

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

			const auto lifeState = ReadLifeState(a_actor);
			if (lifeState != RE::ACTOR_LIFE_STATE::kAlive) {
				spdlog::warn("Incapacitation: KnockoutActor called on actor (0x{:08X}) not in kAlive "
							 "(life state {}) - refused, this module does not stack on another life state.",
					a_actor->GetFormID(), static_cast<std::uint32_t>(lifeState));
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
					if (ReadLifeState(a_actor) == RE::ACTOR_LIFE_STATE::kUnconcious) {
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

		// Lodestone.GetActorLifeState(Actor) -> Int
		//
		// The raw ACTOR_LIFE_STATE of a_actor as a number. A diagnostic seam,
		// not a gameplay call: it exists so a consumer can see, from Papyrus,
		// the exact value KnockoutActor is judging, instead of inferring it
		// from a refusal. Values: 0 alive, 1 dying, 2 dead, 3 unconscious,
		// 4 reanimate, 5 recycle, 6 restrained, 7 essential down, 8 bleedout.
		// None actor -> -1.
		//
		// It was added because the alternative cost four play sessions: the
		// consumer had to instrument one Papyrus flag at a time to work out
		// what the DLL was seeing, and every one of those flags was clean
		// while the number the DLL read was wrong.
		std::int32_t GetActorLifeState(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			if (!a_actor) {
				return -1;
			}
			return static_cast<std::int32_t>(ReadLifeState(a_actor));
		}

		// Lodestone.KnockoutFall(Actor) -> Bool
		//
		// Puts a managed-unconscious actor physically on the ground. Refuses a
		// null actor, one this module is not currently managing, a dead one,
		// and one whose 3D is not loaded (there is no body to drop). Returns
		// true if the actor is on the ground when this returns, including the
		// case where it already was.
		//
		// IDEMPOTENT, and that is a requirement rather than a nicety. knockState
		// is animation state and does not survive a save, so a consumer holding
		// a knockout across a reload has to reapply the fall without being able
		// to tell whether it took - it will call this again on an actor that may
		// or may not still be down. A second impulse on an actor already on the
		// ground is exactly the "stacked impulse" this guard exists to prevent.
		//
		// WHY KnockExplosion AND NOT A HAND-WRITTEN knockState. Writing
		// knockState directly is what the V1 contract refused to do, because
		// that field drives the animation graph and a value without the matching
		// transition is how an actor ends up in a T-pose. KnockExplosion is the
		// engine's own route into the same state - it is what runs on every
		// explosion and every Unrelenting Force in the base game - so the engine
		// performs the whole transition, ragdoll included, and sets knockState
		// itself to whatever it considers consistent. This does not work around
		// the V1 restriction; it removes the reason for it.
		//
		// ALTERNATIVE NOT TAKEN, and the first thing to try if this one needs
		// tuning it cannot get: AIProcess::KnockParalyze(Actor*) (AIProcess.h:184)
		// takes no position and no magnitude, which would retire both constants
		// above and the direction question with them. It was not chosen because
		// this module's own history argues against reaching for paralysis: the
		// documented Papyrus-era predecessor in this space combined an
		// unconscious state with a paralysis effect and produced NPCs that never
		// got up. That risk is about state management rather than about this
		// specific call, and this module owns its recovery path - so the
		// objection is weaker than it looks and this stays a live option.
		bool KnockoutFall(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			if (!a_actor) {
				spdlog::warn("Incapacitation: KnockoutFall got a None actor - ignored.");
				return false;
			}

			const auto formID = a_actor->GetFormID();

			{
				std::lock_guard lock(g_registryLock);
				if (!g_registry.contains(formID)) {
					spdlog::warn("Incapacitation: KnockoutFall called on actor (0x{:08X}) this module is not "
								 "managing - refused. Call KnockoutActor first and check that it returned True.",
						formID);
					return false;
				}
			}

			if (a_actor->IsDead()) {
				spdlog::warn("Incapacitation: KnockoutFall called on a dead actor (0x{:08X}) - ignored.", formID);
				return false;
			}

			if (!a_actor->Is3DLoaded()) {
				spdlog::warn("Incapacitation: KnockoutFall called on actor (0x{:08X}) with no 3D loaded - "
							 "ignored, there is no body to knock down.",
					formID);
				return false;
			}

			// Two guards against a second impulse, and they are NOT interchangeable.
			//
			// 1.12.0 combined them with an OR and the fall never happened once.
			// IsInRagdollState() answers true for an actor in kUnconcious, which
			// is the life state KnockoutActor just set - and this native refuses
			// an actor it is not already managing, so every call arrives in
			// exactly that state. The weak guard therefore fired on 100% of
			// calls, by construction of the API, and KnockExplosion was never
			// reached. Three calls out of three in the first play test, with the
			// contradiction visible inside one log line: ragdoll true, knock
			// state 0, recorded false. An actor genuinely on the ground does not
			// have a knock state of zero.
			//
			// So the module's own record decides on its own, and the engine's
			// opinion only counts when the knock state corroborates it. That
			// second case is a real one worth keeping - an actor another mod
			// knocked down should not get a second impulse from here - but it
			// has to be a state the engine actually put the actor in, not a
			// side effect of the life state this module set a moment ago.
			//
			// After a load the record is empty by design, so the consumer's
			// reapply still goes through. That is the case it exists for.
			bool alreadyFallen = false;
			{
				std::lock_guard lock(g_registryLock);
				alreadyFallen = g_fallen.contains(formID);
			}

			const auto guardKnockState = ReadKnockState(a_actor);
			const bool engineHasItDown = a_actor->IsInRagdollState() &&
										 guardKnockState != RE::KNOCK_STATE_ENUM::kNormal;

			if (alreadyFallen || engineHasItDown) {
				// Two distinct messages on purpose: "this module already dropped
				// it" and "something else did" are different situations, and a
				// shared message is what made the 1.12.0 defect need a play
				// session to see instead of a log line.
				if (alreadyFallen) {
					spdlog::info("Incapacitation: KnockoutFall on actor (0x{:08X}) - this module already "
								 "knocked it down (knock state {}, ragdoll {}), no second impulse applied.",
						formID, static_cast<std::uint32_t>(guardKnockState), a_actor->IsInRagdollState());
				} else {
					spdlog::info("Incapacitation: KnockoutFall on actor (0x{:08X}) - already on the ground "
								 "from somewhere else (knock state {}, life state {}), no impulse applied. "
								 "Recording it as fallen so KnockoutRecover will bring it back up.",
						formID, static_cast<std::uint32_t>(guardKnockState),
						static_cast<std::uint32_t>(ReadLifeState(a_actor)));
				}

				std::lock_guard lock(g_registryLock);
				g_fallen.insert(formID);
				return true;
			}

			auto* process = a_actor->GetActorRuntimeData().currentProcess;
			if (!process) {
				spdlog::warn("Incapacitation: KnockoutFall found no AI process on actor (0x{:08X}) - refused.",
					formID);
				return false;
			}

			try {
				const auto beforeLife = ReadLifeState(a_actor);
				const auto position = a_actor->GetPosition();
				const RE::NiPoint3 origin{ position.x, position.y, position.z + kFallOriginHeight };

				process->KnockExplosion(a_actor, origin, kFallImpulse);

				// THE ENGINE DROPS THE ACTOR AND THEN FORGETS IT DID.
				//
				// 1.12.0 deliberately did not write this field on the way down,
				// on the reasoning that the engine performs the transition and
				// therefore knows better than we do which state is consistent
				// with it. The play test answered that: KnockExplosion applies
				// the physics - the actor visibly falls - and leaves knockState
				// at kNormal. With no state, nothing holds the actor down. The
				// ragdoll settles and the engine's own recovery stands it back
				// up within a second or two, while the life state keeps it
				// pacified, so it ends up upright and inert. Three targets out
				// of three, log line `knock state 0 -> 0`.
				//
				// This is not V1's forbidden write. D2 refused to set knockState
				// COLD - without a transition, which is what risks the T-pose.
				// Here the transition has already happened and this only records
				// what is on screen. The write path itself is proven: recovery
				// has been writing kGetUp since 1.12.0 and ran twice in that same
				// test with nothing wrong.
				//
				// kDown over kOut, on the only evidence available rather than on
				// the names: the published reference implementation in this space
				// writes kDown to hold an actor on the ground, and it works in
				// game. kOut reads better - it pairs with kOutLeadIn, which
				// suggests a sustained state - but that is name-reading, and the
				// library documents none of these values. If kDown does not hold,
				// kOut is the next thing to try, and it is a one-word change.
				//
				// Only written if the engine left kNormal. If it ever does pick a
				// state, it is better informed than this line is.
				const auto engineKnock = ReadKnockState(a_actor);
				if (engineKnock == RE::KNOCK_STATE_ENUM::kNormal) {
					a_actor->AsActorState()->actorState1.knockState = RE::KNOCK_STATE_ENUM::kDown;
				}

				// Z goes in the log so the two ends of a knockout can be compared
				// against each other. It is NOT a check on this call: the physics
				// runs over the frames after this returns, so the actor has not
				// moved yet by the time this line is written. The height here
				// against the height in KnockoutRecover is what shows whether the
				// body was still on the ground at the end of the window.
				spdlog::info("Incapacitation: KnockoutFall on actor (0x{:08X}) - knock state {} -> {} (engine) "
							 "-> {} (written), life state {} -> {}, z {:.1f}, impulse {} from {} units above.",
					formID, static_cast<std::uint32_t>(guardKnockState),
					static_cast<std::uint32_t>(engineKnock),
					static_cast<std::uint32_t>(ReadKnockState(a_actor)),
					static_cast<std::uint32_t>(beforeLife), static_cast<std::uint32_t>(ReadLifeState(a_actor)),
					position.z, kFallImpulse, kFallOriginHeight);

				std::lock_guard lock(g_registryLock);
				g_fallen.insert(formID);
			} catch (...) {
				spdlog::error("Incapacitation: KnockoutFall threw on actor (0x{:08X}) - not recorded as fallen.",
					formID);
				return false;
			}

			return true;
		}

		// Lodestone.KnockoutRecover(Actor) -> Bool
		//
		// Gets an actor this module knocked down back on its feet. A harmless
		// false on an actor KnockoutFall never dropped - nothing is touched.
		//
		// ORDER AGAINST WakeActor DOES NOT MATTER, by construction. This call
		// never reads or writes the life state and WakeActor never reads or
		// writes the knock state, so the two revert independent halves and
		// either one may run first. That is deliberate: the failure this module
		// most has to avoid is an actor left permanently down, and a recovery
		// that only works in one call order is a recovery with a way to be
		// skipped.
		//
		// Leaves a corpse alone. Once an actor is dead the engine owns its
		// pose, and forcing a get-up transition on a body that is already in a
		// death ragdoll is how a corpse ends up standing or sliding.
		bool KnockoutRecover(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			if (!a_actor) {
				return false;
			}

			const auto formID = a_actor->GetFormID();

			bool wasFallen = false;
			{
				std::lock_guard lock(g_registryLock);
				wasFallen = g_fallen.erase(formID) > 0;
			}

			if (!wasFallen) {
				return false;
			}

			if (a_actor->IsDead()) {
				spdlog::info("Incapacitation: KnockoutRecover on actor (0x{:08X}) - dead, pose left to the "
							 "engine. Dropped from the fallen set.",
					formID);
				return true;
			}

			try {
				const auto beforeKnock = ReadKnockState(a_actor);

				// THIS IS WHERE THE LOG ANSWERS "DID THE FALL HOLD".
				//
				// Nothing at the moment of the fall can answer it: the physics
				// runs over the frames after KnockoutFall returns, so every
				// reading taken there describes an actor that has not moved yet.
				// Worse, IsInRagdollState() is true for any actor in kUnconcious,
				// which made `knock state 0 -> 0, ragdoll true` mean both "never
				// fell" and "fell and stood back up" - two opposite outcomes
				// printing the same line, separated only by someone watching the
				// screen. That ambiguity cost a whole reading of the 1.12.1 test.
				//
				// The end of the window is where the difference becomes legible.
				// KnockoutFall leaves the actor in kDown; if this call finds
				// kNormal instead, nothing else in this module cleared it, so the
				// engine stood the actor up on its own and the fall did not hold.
				// That is a failed capability rather than a normal path, so it
				// warns.
				if (!IsDownKnockState(beforeKnock)) {
					spdlog::warn("Incapacitation: KnockoutRecover on actor (0x{:08X}) - this module knocked it "
								 "down but the knock state came back {} instead of a down state. The actor got "
								 "up on its own during the window; the fall did not hold.",
						formID, static_cast<std::uint32_t>(beforeKnock));
				}

				// Only written when the actor is still down. If the engine is
				// already unwinding the state, it is better informed than this
				// call is.
				if (IsDownKnockState(beforeKnock)) {
					a_actor->AsActorState()->actorState1.knockState = RE::KNOCK_STATE_ENUM::kGetUp;
				}

				// The 3D resync is the half with no Papyrus equivalent, and the
				// reference implementation in this space treats it - rather than
				// the package re-evaluation - as what keeps the model from
				// coming back wrong. EvaluatePackage last, so the AI resumes
				// against a model that has already been put back together.
				a_actor->Update3DModel();
				a_actor->UpdateActor3DPosition();
				a_actor->EvaluatePackage(true, true);

				// Z against the height KnockoutFall logged for the same actor:
				// still low means the body was on the ground when the window
				// ended, back to standing height means it had got up.
				spdlog::info("Incapacitation: KnockoutRecover on actor (0x{:08X}) - knock state {} -> {}, "
							 "z {:.1f}, ragdoll {}, 3D resynced and package re-evaluated.",
					formID, static_cast<std::uint32_t>(beforeKnock),
					static_cast<std::uint32_t>(ReadKnockState(a_actor)), a_actor->GetPosition().z,
					a_actor->IsInRagdollState());
			} catch (...) {
				spdlog::error("Incapacitation: KnockoutRecover threw on actor (0x{:08X}) - fallen-set entry "
							  "already removed, so this cannot leave the actor stuck as far as this module "
							  "is concerned.",
					formID);
			}

			return true;
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
		// Death sink
		//
		// The one thing this module cannot learn by being called: an actor it
		// is managing can die, and nothing in the Papyrus surface has to tell
		// us. Until this existed, WakeActor was the only path that removed a
		// FormID from the registry, so killing a managed actor left it managed
		// forever - IsManagedUnconscious answered true for a corpse and the
		// cosave carried a dead actor between saves with no way to ever drop
		// it. The physical fall turns that from an invisible leak into a body
		// on the ground that this module still believes it is responsible for.
		//
		// This is the first event sink in the plugin. It is not an engine hook:
		// nothing is detoured and no address is involved, so none of the
		// hooking rules in CONVENTIONS.md apply to it. It does need the event
		// source to exist, which is why it is wired on kDataLoaded like the
		// hooks are, rather than at plugin load like the cosave.
		// -------------------------------------------------------------------

		class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
		{
		public:
			static DeathSink* GetSingleton()
			{
				static DeathSink singleton;
				return std::addressof(singleton);
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent*           a_event,
				RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				// Never let anything escape into the engine, on any path.
				try {
					if (!a_event || !a_event->actorDying) {
						return RE::BSEventNotifyControl::kContinue;
					}

					// The FormID is all this needs - no cast to Actor, nothing
					// dereferenced beyond the form itself.
					const auto formID = a_event->actorDying->GetFormID();

					bool wasManaged = false;
					bool wasFallen = false;
					{
						std::lock_guard lock(g_registryLock);
						wasManaged = g_registry.erase(formID) > 0;
						wasFallen = g_fallen.erase(formID) > 0;
					}

					// This event fires more than once for a single death (the
					// `dead` flag on it separates entering the death state from
					// the confirmed death). That is engine behavior the headers
					// do not document, so nothing here depends on it: erasing
					// is idempotent and only the pass that actually removed
					// something says so.
					if (wasManaged || wasFallen) {
						spdlog::info("Incapacitation: managed actor (0x{:08X}) died - dropped from the "
									 "registry (managed {}, fallen {}). No wake event is dispatched: it did "
									 "not wake.",
							formID, wasManaged, wasFallen);
					}
				} catch (...) {
					spdlog::error("Incapacitation: the death sink threw - swallowed, the engine must not see it.");
				}

				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			DeathSink() = default;
			DeathSink(const DeathSink&) = delete;
			DeathSink(DeathSink&&) = delete;
			~DeathSink() override = default;
			DeathSink& operator=(const DeathSink&) = delete;
			DeathSink& operator=(DeathSink&&) = delete;
		};

		// -------------------------------------------------------------------
		// Cosave callbacks
		//
		// Persist ONLY the set of managed FormIDs - no duration, the
		// consumer's own Papyrus timer owns that and Papyrus state already
		// survives a save on its own. See Incapacitation.h, PERSISTENCE.
		//
		// THE FALLEN SET IS NOT PERSISTED, AND THE RECORD DOES NOT CHANGE.
		// Adding it would mean a 'INC1' version 2 and a load path for both
		// shapes, to persist an answer that is wrong by the time it is read:
		// knockState is animation state that does not survive a save, so a
		// managed actor comes back from a reload standing up no matter what
		// this set claimed. Saving it would restore a record of a fall that
		// did not survive.
		//
		// What that leaves for the consumer: after a load, reapply
		// KnockoutFall to the actors it still considers knocked out. That call
		// is idempotent precisely so this is safe to do without knowing
		// whether the physical state survived.
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

			// Whatever was on the ground before this load is not on the ground
			// now - the fall did not travel in the save. The consumer reapplies
			// it, so starting empty is the honest state rather than a lost one.
			g_fallen.clear();

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
			g_fallen.clear();
			spdlog::info("Incapacitation: registry and fallen set cleared (new game or return to main menu).");
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
		a_vm->RegisterFunction("KnockoutFall", "Lodestone", KnockoutFall);
		a_vm->RegisterFunction("KnockoutRecover", "Lodestone", KnockoutRecover);
		a_vm->RegisterFunction("IsManagedUnconscious", "Lodestone", IsManagedUnconscious);
		a_vm->RegisterFunction("GetActorLifeState", "Lodestone", GetActorLifeState);
		a_vm->RegisterFunction("RegisterForActorWoke", "Lodestone", RegisterForActorWoke);
		a_vm->RegisterFunction("UnregisterForActorWoke", "Lodestone", UnregisterForActorWoke);
		a_vm->RegisterFunction("RegisterForActorWokeAlias", "Lodestone", RegisterForActorWokeAlias);
		a_vm->RegisterFunction("UnregisterForActorWokeAlias", "Lodestone", UnregisterForActorWokeAlias);

		spdlog::info("Incapacitation: natives registered (KnockoutActor, WakeActor, KnockoutFall, "
					 "KnockoutRecover, IsManagedUnconscious, GetActorLifeState, RegisterForActorWoke, "
					 "UnregisterForActorWoke, RegisterForActorWokeAlias, UnregisterForActorWokeAlias).");
		return true;
	}

	void Install()
	{
		auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
		if (!holder) {
			spdlog::error("Incapacitation: no script event source holder - a managed actor that dies will "
						  "stay in the registry, and IsManagedUnconscious will answer true for its corpse.");
			return;
		}

		holder->AddEventSink<RE::TESDeathEvent>(DeathSink::GetSingleton());
		spdlog::info("Incapacitation: death sink registered - a managed actor that dies leaves the registry.");
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
