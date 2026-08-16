// Incapacitation.cpp
// Lodestone - Shared SKSE framework
//
// The managed non-lethal knockout module. See Incapacitation.h for the
// contract and why it has no engine hook and no native timer.
//
// Phase: Hook A (managed non-lethal knockout)

#include "Incapacitation.h"

#include <functional>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <safetyhook.hpp>

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
		//
		// The value is this module's per-actor scratch pad for one knockout.
		//
		// blockedGetUps counted the get-ups the hook refused, and its verdict is
		// already in: across a full window it stayed at zero, which is how we
		// know the engine does not stand these actors up through the function
		// that hook is on.
		//
		// loggedAnimEvents bounds the observation pass below. An actor produces
		// a lot of animation events, and a knockout is thirty seconds long.
		struct FallenState
		{
			std::uint32_t blockedGetUps = 0;
			std::uint32_t loggedAnimEvents = 0;

			// Last animation event written for this actor, so runs of the same
			// event at the same knock state collapse to one line. Without this
			// the budget goes to repetition: one knockout spent 53 of its 60
			// lines on consecutive AddCharacterControllerToWorld at knock 1, and
			// the trace went quiet six tenths of a second after the fall - long
			// before the actor got back up, which was the thing being watched.
			std::size_t   lastTagHash = 0;
			std::uint32_t lastKnock = 0xFFFFFFFFU;

			// Whether KnockoutFall is the one that put this actor into
			// kUnconcious, and therefore the one that owes it a way back.
			//
			// True only when the fall ran on an actor this module was NOT
			// managing. When it IS managed, KnockoutActor set the life state
			// and WakeActor reverts it, and the two halves stay independent -
			// which is the guarantee that lets the consumer call WakeActor and
			// KnockoutRecover in either order.
			//
			// 1.12.6 relaxed the fall's precondition without following this
			// through, and left a path where nobody restored the life state at
			// all: the fall set kUnconcious, recovery did not touch it by
			// contract, and WakeActor refuses an unmanaged actor. An actor
			// pacified forever is precisely the failure this module refused
			// KnockParalyze over, and it arrived by the back door.
			bool ownsLifeState = false;
		};

		std::unordered_map<RE::FormID, FallenState> g_fallen;

		// How many DISTINCT animation events to write per knockout before going
		// quiet. Distinct is what makes the budget go far enough: repetition is
		// collapsed before it is counted, so this covers transitions rather
		// than frames.
		constexpr std::uint32_t kMaxLoggedAnimEvents = 120;

		// The magnitude handed to KnockExplosion, and it is deliberately the
		// smallest positive float rather than a force.
		//
		// 1.12.x through 1.12.4 passed 1.0 and read that as conservative. It
		// was not: the animation trace showed the actor going through
		// kExplodeLeadIn into kExplode, firing Collision_RecoilCancelable and
		// Collision_SRecoil_FailSafe. That is the engine's RECOIL path, which
		// is transitory by design - it plays and resolves itself, which is
		// exactly why the actor got back up and why no get-up was ever
		// initiated. A push big enough to be a push is a push the engine will
		// undo.
		//
		// This call is not here for the shove. It is here for its side effect:
		// it drives the physics/ragdoll transition on an actor that the steps
		// around it have already prepared. A magnitude this small cannot
		// trigger the recoil the trace caught. The published reference
		// implementation in this space passes the same thing, and it works in
		// game.
		//
		// Still not a balance number, and still never a parameter - the
		// consumer's only requirement is that the target goes down.
		//
		// The parentheses around the call are load bearing, not style: Windows
		// defines a `min` macro, and without them the preprocessor eats this
		// line before the compiler sees it.
		constexpr float kFallNudge = (std::numeric_limits<float>::min)();

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

		// Reads an actor's sit/sleep state. Same accessor rule as the two
		// above - it is the neighbouring bitfield in ActorState1.
		//
		//   ActorState.h:113   SIT_SLEEP_STATE sitSleepState: 4
		//   ActorState.h:65    kNormal 0, kWantToSit 1, kWaitingForSitAnim 2,
		//                      kIsSitting 3, kWantToStand 4, kWantToSleep 5,
		//                      kWaitingForSleepAnim 6, kIsSleeping 7,
		//                      kWantToWake 8
		RE::SIT_SLEEP_STATE ReadSitSleepState(RE::Actor* a_actor)
		{
			return a_actor->AsActorState()->GetSitSleepState();
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
		// Animation observation
		//
		// THIS SINK CHANGES NOTHING. It exists because four rounds in a row were
		// spent the same way - pick a candidate mechanism, implement it, test it
		// in game, find out it was something else - and each round costs a play
		// session. The engine knows what it is doing to these actors and names it
		// out loud in animation events, so this round asks it instead of guessing
		// a fifth time.
		//
		// What the answer has to settle: the actor falls and is back on its feet
		// in one to two seconds, with knockState never leaving kNormal and the
		// get-up function never called. Either the engine runs a get-up by some
		// path nobody has found, or - the reading the consumer favours, and mine
		// too - nothing gets the actor up at all, because nothing ever decided it
		// was down: KnockExplosion throws the body and the animation graph
		// returns to standing when the physics settles. Those two produce very
		// different event traces, and a `GetUpStart`-shaped tag appearing or not
		// appearing separates them in one pass.
		//
		// Registered per actor on the way down and removed on recovery. The sink
		// is a singleton with static lifetime, so an actor that dies mid-window
		// and never reaches recovery leaves a registration behind rather than a
		// dangling pointer, and the filter below makes it inert - the FormID is
		// gone from g_fallen by then.
		// -------------------------------------------------------------------

		class AnimationSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			static AnimationSink* GetSingleton()
			{
				static AnimationSink singleton;
				return std::addressof(singleton);
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent*           a_event,
				RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				try {
					if (!a_event || !a_event->holder) {
						return RE::BSEventNotifyControl::kContinue;
					}

					// FormID comes off the holder as a TESObjectREFR, before any
					// cast. The membership check below is what establishes that
					// this reference is one of the actors this module knocked
					// down, and therefore that treating it as an Actor is sound -
					// animation events reach other kinds of reference too.
					const auto formID = a_event->holder->GetFormID();

					auto* actorForState = static_cast<RE::Actor*>(a_event->holder);
					const auto tagHash = std::hash<std::string_view>{}(
						a_event->tag.empty() ? std::string_view{} : std::string_view{ a_event->tag.c_str() });
					const auto knockNow = static_cast<std::uint32_t>(ReadKnockState(actorForState));

					bool report = false;
					{
						std::lock_guard lock(g_registryLock);
						const auto it = g_fallen.find(formID);
						if (it == g_fallen.end()) {
							return RE::BSEventNotifyControl::kContinue;
						}

						// Same event, same knock state as the line before it: the
						// engine repeating itself, which says nothing a second
						// time. Skipped before the budget is charged.
						if (it->second.lastTagHash == tagHash && it->second.lastKnock == knockNow) {
							return RE::BSEventNotifyControl::kContinue;
						}

						it->second.lastTagHash = tagHash;
						it->second.lastKnock = knockNow;

						if (it->second.loggedAnimEvents < kMaxLoggedAnimEvents) {
							++it->second.loggedAnimEvents;
							report = true;
						} else if (it->second.loggedAnimEvents == kMaxLoggedAnimEvents) {
							++it->second.loggedAnimEvents;
							spdlog::info("Incapacitation: [obs] actor (0x{:08X}) - reached {} distinct animation "
										 "events, going quiet for the rest of this knockout.",
								formID, kMaxLoggedAnimEvents);
						}
					}

					if (report) {
						auto* actor = static_cast<RE::Actor*>(a_event->holder);

						// The state snapshot travels with every event so the log
						// reads as a timeline rather than a list of names: what
						// the engine announced, and what the actor looked like at
						// that moment.
						spdlog::info("Incapacitation: [obs] actor (0x{:08X}) anim '{}' payload '{}' - knock {}, "
									 "life {}, z {:.1f}, ragdoll {}.",
							formID, a_event->tag.c_str(), a_event->payload.c_str(),
							static_cast<std::uint32_t>(ReadKnockState(actor)),
							static_cast<std::uint32_t>(ReadLifeState(actor)), actor->GetPosition().z,
							actor->IsInRagdollState());
					}
				} catch (...) {
					spdlog::error("Incapacitation: the animation observation sink threw - swallowed.");
				}

				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			AnimationSink() = default;
			AnimationSink(const AnimationSink&) = delete;
			AnimationSink(AnimationSink&&) = delete;
			~AnimationSink() override = default;
			AnimationSink& operator=(const AnimationSink&) = delete;
			AnimationSink& operator=(AnimationSink&&) = delete;
		};


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
		// Registers an actor for the physical fall. It applies nothing. The
		// fall itself lands inside the SetUnconscious hook, and only for actors
		// registered here - so this call arms and that call fires.
		//
		// Refuses a null actor and a dead one. Idempotent: registering an actor
		// already registered returns true and does nothing, which is what makes
		// it safe for a consumer to reapply after a load without first working
		// out whether it needs to.
		//
		// WHY IT APPLIES NOTHING, given that it used to. Through 1.13.2 this
		// ran the whole sequence and the hook ran it again fourteen
		// milliseconds later. Measured by phase in the consumer's trace: the
		// first application threw the actor 56 units up and left it settling 35
		// units above standing height; the second wrote kDown and held it there.
		// The result looked like an actor stuck in mid-air and was in fact an
		// actor held correctly in the place the first impulse had thrown it.
		//
		// The second application was doing its job. The first was the defect,
		// and it was unreachable by testing: only this call registers,
		// registering is what arms the hook, so every application the hook made
		// landed on an actor that had already been launched. No ordering on
		// either side produced a single application.
		//
		// So the impulse lives in one place, and it is the place that works.
		// Everything the sequence needs - the AI process, the 3D, the life
		// state cycle, the nudge, the state writes - is checked and done at the
		// hook, at the moment it applies. Checking any of it here would answer
		// for an instant that no longer matters by the time it is used.
		bool KnockoutFall(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			if (!a_actor) {
				spdlog::warn("Incapacitation: KnockoutFall got a None actor - ignored.");
				return false;
			}

			const auto formID = a_actor->GetFormID();

			// THIS CALL REGISTERS AND DOES NOTHING ELSE, WHICH IS WHAT THE
			// DOCUMENTATION HAS SAID ALL ALONG AND WHAT THE CODE FINALLY DOES.
			//
			// Through 1.13.2 it also ran the whole physical sequence, and the
			// hook ran it again fourteen milliseconds later. The consumer's
			// trace separated the two by phase and measured what each did: the
			// first launched the actor 56 units UP and let it settle 35 units
			// above where it had been standing; the second wrote kDown and
			// froze it there. The actor was not stuck in the air by accident -
			// it was held, successfully, in the wrong place, because something
			// had thrown it there first.
			//
			// The second application was working. The first was the defect, and
			// it could not even be tested around: only this call registers, and
			// registering is what arms the hook, so every application the hook
			// ever made landed on an already-launched actor. There was no path -
			// not from the consumer, not from the console - that produced a
			// single application.
			//
			// So the impulse belongs to one place, and it is the place that
			// works: inside the handler. Here, nothing is applied. No nudge, no
			// life state, no state writes.
			bool managed = false;
			bool alreadyFallen = false;
			{
				std::lock_guard lock(g_registryLock);
				managed = g_registry.contains(formID);
				alreadyFallen = g_fallen.contains(formID);
			}

			if (a_actor->IsDead()) {
				spdlog::warn("Incapacitation: KnockoutFall called on a dead actor (0x{:08X}) - ignored.", formID);
				return false;
			}

			if (alreadyFallen) {
				spdlog::info("Incapacitation: KnockoutFall on actor (0x{:08X}) - already registered, nothing "
							 "to do. Call SetUnconscious(True) to apply the fall.",
					formID);
				return true;
			}

			// The 3D and AI-process checks that used to live here are gone with
			// the sequence they guarded. They belong at the moment of
			// application, which is now inside the handler, and they are there.
			// Checking them at registration would only answer for an instant
			// that no longer matters.
			try {
				{
					FallenState state;

					// Nothing here moves the life state any more, so nothing here
					// owes it back. The native the consumer is about to call owns
					// it in both directions: SetUnconscious(True) sets it and
					// SetUnconscious(False) restores it. The flag stays for the
					// managed path's benefit and is simply never set by this
					// call.
					state.ownsLifeState = false;

					std::lock_guard lock(g_registryLock);
					g_fallen.emplace(formID, state);
				}

				// The observation sink goes on at registration so the trace
				// covers the application when it happens, rather than starting
				// after it.
				a_actor->AddAnimationGraphEventSink(AnimationSink::GetSingleton());

				spdlog::info("Incapacitation: KnockoutFall registered actor (0x{:08X}) - managed {}, life {}, "
							 "knock {}. NOTHING APPLIED HERE. The fall lands when SetUnconscious(True) runs "
							 "on this actor; without that call this is a no-op.",
					formID, managed, static_cast<std::uint32_t>(ReadLifeState(a_actor)),
					static_cast<std::uint32_t>(ReadKnockState(a_actor)));
			} catch (...) {
				spdlog::error("Incapacitation: KnockoutFall threw on actor (0x{:08X}) - not registered.",
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

			// The count has to come out in the same locked step that removes the
			// entry - reading it afterwards would always find nothing, because
			// the entry is gone by then.
			bool          wasFallen = false;
			bool          ownsLifeState = false;
			std::uint32_t blocked = 0;
			{
				std::lock_guard lock(g_registryLock);
				if (const auto it = g_fallen.find(formID); it != g_fallen.end()) {
					wasFallen = true;
					blocked = it->second.blockedGetUps;
					ownsLifeState = it->second.ownsLifeState;
					g_fallen.erase(it);
				}
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

			// Paired with the registration in KnockoutFall. Done before anything
			// else here so the trace ends where the knockout ends, rather than
			// picking up the get-up this call is about to cause.
			a_actor->RemoveAnimationGraphEventSink(AnimationSink::GetSingleton());

			try {
				const auto beforeKnock = ReadKnockState(a_actor);

				// THIS IS THE READING THAT SETTLES THE ROUND.
				//
				// Taken at the end of the window, when the physics and the state
				// machine have long since finished - unlike everything
				// KnockoutFall can see, which is pre-physics by construction.
				//
				// sitSleepState still kIsSleeping (7) means the state held for
				// the whole knockout, which is the capability working. Back at
				// kNormal (0) means the engine let it go, the same way it let
				// knockState go in 1.12.2, and then the sit/sleep route is
				// answered too.
				//
				// The blocked count is no longer the headline. Zero used to point
				// at the hook; the trace has since explained it - a recoil is not
				// a knockdown, so no get-up was ever initiated and there was
				// nothing to block. It stays in the log because if this sequence
				// does produce a real knockdown, a get-up may finally be attempted
				// and the count would be the first place that shows.
				const auto beforeSit = ReadSitSleepState(a_actor);
				const bool sitSleepHeld = beforeSit == RE::SIT_SLEEP_STATE::kIsSleeping;

				if (!sitSleepHeld) {
					spdlog::warn("Incapacitation: KnockoutRecover on actor (0x{:08X}) - sit/sleep state came "
								 "back {} instead of kIsSleeping (7). The engine let the state go during the "
								 "window; the fall did not hold, the same way knockState did not.",
						formID, static_cast<std::uint32_t>(beforeSit));
				}

				// Reverting goes through the same door it was set through. Left
				// alone, an actor released from a knockout would carry a sleeping
				// sit/sleep state into normal play, which is a worse residue than
				// anything this module has left behind so far.
				a_actor->AsActorState()->DoSetSitSleepState(RE::SIT_SLEEP_STATE::kNormal);

				// AND THE LIFE STATE, BUT ONLY WHEN THIS MODULE'S FALL IS WHAT
				// MOVED IT.
				//
				// When the actor is managed, KnockoutActor moved it and
				// WakeActor moves it back, and touching it here would break the
				// guarantee that the two calls can be issued in either order.
				// When the actor is not managed, the fall moved it and nothing
				// else will ever move it back - WakeActor refuses an unmanaged
				// actor - so this is the only place the cycle can close.
				//
				// Guarded on the state still being kUnconcious so this cannot
				// wake something that changed underneath us in the meantime.
				if (ownsLifeState && ReadLifeState(a_actor) == RE::ACTOR_LIFE_STATE::kUnconcious) {
					a_actor->SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);
					spdlog::info("Incapacitation: KnockoutRecover on actor (0x{:08X}) - restored life state to "
								 "kAlive. The fall set it on an unmanaged actor, so nothing else would have.",
						formID);
				}

				// knockState IS written here again, and the reasoning that
				// removed it in 1.12.5 was right for its time and wrong now.
				// Back then the field never held, so reverting it was undoing
				// nothing. Once the fall started holding, the same field became
				// what keeps an actor on the ground, and leaving it set is how
				// the first successful fall produced an actor that could not get
				// up at all.
				//
				// Best effort rather than the main path. The revert that
				// actually takes hold is the one inside the disable handler,
				// which needs the consumer to call SetUnconscious(False). This
				// covers the case where that call never comes, or comes after
				// this one - by then the actor has left the fallen set and the
				// handler no longer recognises it.
				a_actor->AsActorState()->actorState1.knockState = RE::KNOCK_STATE_ENUM::kGetUp;

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
				spdlog::info("Incapacitation: KnockoutRecover on actor (0x{:08X}) - sit/sleep {} -> {} (held "
							 "{}), knock {} -> {}, blocked {} get-up attempt(s), z {:.1f}, ragdoll {}, 3D "
							 "resynced and package re-evaluated.",
					formID, static_cast<std::uint32_t>(beforeSit),
					static_cast<std::uint32_t>(ReadSitSleepState(a_actor)), sitSleepHeld,
					static_cast<std::uint32_t>(beforeKnock),
					static_cast<std::uint32_t>(ReadKnockState(a_actor)), blocked, a_actor->GetPosition().z,
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
		// The get-up hook
		//
		// This is what holds a knocked-down actor on the ground, after two
		// versions failed to do it by other means. KnockExplosion drops the
		// actor and works, but the engine stands it back up a second or two
		// later; writing knockState to say "this actor is down" did nothing,
		// because that field is what the engine reports rather than what it
		// reads. The thing to stop was never the description, it was the
		// decision - and InitiateGetUpPackage is where the decision is taken.
		//
		// THIS IS A SUPPRESSING HOOK, the exception CONVENTIONS.md allows, and
		// it has to meet that section's three conditions:
		//
		//   1. Only the suppressing path skips the original. Every actor this
		//      module is not currently holding down calls it and behaves
		//      exactly as vanilla - the overwhelming majority of calls.
		//   2. The effect cannot be undone afterwards. Once the get-up package
		//      is running, the actor is standing; there is no un-get-up, and
		//      putting it back down means a second impulse, which is the
		//      visual mess this design avoids.
		//   3. Every skipped side effect accounted for - AND THIS ONE IS ONLY
		//      PARTLY DISCHARGED, which is worth saying rather than glossing.
		//      The library exposes the declaration but not the body, so what
		//      else this function does on the way to starting the package is
		//      not known here. What bounds the risk is scope: suppression
		//      applies only to actors this module knocked down, only while
		//      the consumer holds the knockout, and it is released the moment
		//      the actor leaves the fallen set - including on death. If it
		//      turns out to do something else that matters, the symptom will
		//      be confined to knocked-out actors.
		//
		// THE INDEX DIVERGES BETWEEN RUNTIMES AND THE TWO VALUES COLLIDE.
		// CommonLibSSE-NG resolves it at call time and states both numbers
		// (src/RE/A/Actor.cpp:1993):
		//
		//     RelocateVirtual<...>(0x0DE, 0x0E0, this);   // SE/AE, VR
		//
		// write_vfunc takes a single index and the library offers no helper
		// that installs across both, so the branch below is written by hand.
		// Getting it backwards does not crash: 0x0E0 on SE/AE is UpdateAlpha,
		// so the swap would quietly hook actor transparency and leave the
		// knockout looking broken for reasons nothing would connect to this.
		// -------------------------------------------------------------------

		struct InitiateGetUpPackageHook
		{
			static void thunk(RE::Actor* a_this)
			{
				if (a_this) {
					try {
						const auto formID = a_this->GetFormID();

						std::lock_guard lock(g_registryLock);
						if (const auto it = g_fallen.find(formID); it != g_fallen.end()) {
							++it->second.blockedGetUps;

							// Only the first one is written. The engine may
							// retry every time it re-evaluates, and a line per
							// attempt would bury the log of a long knockout;
							// the total is reported once, by KnockoutRecover.
							if (it->second.blockedGetUps == 1) {
								spdlog::info("Incapacitation: blocked the engine from standing actor "
											 "(0x{:08X}) back up. This module is holding it down; further "
											 "attempts are counted, not logged.",
									formID);
							}

							return;
						}
					} catch (...) {
						// Fall through to the original. An actor that stays
						// down because this threw is the failure mode with no
						// way back, so the safe direction is always vanilla.
						spdlog::error("Incapacitation: the get-up hook threw - letting the original run.");
					}
				}

				func(a_this);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		// -------------------------------------------------------------------
		// The SetUnconscious call-site hooks - PROVING PASS, NO BEHAVIOUR
		//
		// Eight rounds went into composing public calls from outside the engine,
		// and the trace eventually explained why none of it held: a Papyrus call
		// lands BETWEEN engine frames, and by the time the next one runs the
		// state machine has settled. The reference implementation does not
		// compose calls at all - it replaces two `call` instructions inside the
		// body of the handler behind Actor.SetUnconscious, so its code runs
		// INSIDE that handler, before it returns. That is a place no amount of
		// calling public functions can reach.
		//
		// THIS VERSION CHANGES NOTHING. Both thunks call the original and return
		// its answer, and all they add is a log line. CONVENTIONS.md asks for
		// exactly this before behaviour goes on a borrowed address, and this
		// trail has already paid twice for skipping the proving step - once on a
		// stale second-hand reading, once on a vtable index that happened to be
		// right. What the log has to show: the thunk firing when, and only when,
		// something calls SetUnconscious, with an actor whose FormID makes sense
		// and a bool that matches what was asked. Anything else and the address
		// is wrong.
		//
		// THE FUNCTION, NOT THE CALL SITES, AND THE DIFFERENCE IS THE WHOLE
		// POINT OF THIS VERSION.
		//
		// 1.12.8 and 1.13.0 redirected two call sites inside the console's
		// handler, which is where the reference implementation puts them. That
		// works, and it was proven working - but only for callers that go
		// through that handler. Papyrus does not: an Actor.SetUnconscious call
		// from a script moved the life state with the hooks installed and
		// produced no line at all, and the report below logs unconditionally,
		// so silence there means the call never reached us. Two entry points,
		// one implementation.
		//
		// Both call sites resolved to the same address, which is the thing that
		// makes this possible: hooking the implementation itself catches every
		// caller - console, Papyrus, another mod, anything. The address is not
		// borrowed from anywhere; it is read out of the displacement of a call
		// site this project has already proven, and the first byte is checked
		// to be a five-byte call before trusting it.
		//
		// NOT INSTALLED ON VR, AND THE REASON IS A TRAP WORTH WRITING DOWN.
		// REL::RelocationID's two-argument constructor assigns the SE id to the
		// VR slot (ID.h:480) rather than refusing. VR has its own Address
		// Library numbering, so on VR that silently resolves to whatever else
		// id 21874 happens to be - a wrong address that installs quietly. The
		// reference implementation uses the same constructor and does not target
		// VR; this plugin does, so the guard is ours to add.
		// -------------------------------------------------------------------

		SafetyHookInline g_setUnconsciousHook{};

		// Reads the target of a five-byte `call rel32` (E8 cd). The
		// displacement sits at +1 and is relative to the instruction after it.
		// Returns 0 if the byte is not E8, because trusting a displacement read
		// out of something that is not a call is how a wrong address gets
		// installed quietly.
		std::uintptr_t ResolveCallTarget(std::uintptr_t a_callSite)
		{
			if (*reinterpret_cast<const std::uint8_t*>(a_callSite) != 0xE8) {
				return 0;
			}

			const auto disp = *reinterpret_cast<const std::int32_t*>(a_callSite + 1);
			return a_callSite + 5 + disp;
		}

		struct SetUnconsciousHook
		{
			static bool thunk(RE::Actor* a_actor, bool a_enable)
			{
				// Original first, always - the same rule every other thunk in
				// this plugin follows, and here it is also what the reference
				// implementation does: the engine's own work has to be finished
				// before anything is stacked on it.
				const auto ret = g_setUnconsciousHook.call<bool, RE::Actor*, bool>(a_actor, a_enable);

				Report(a_enable ? "enable" : "disable", a_actor, a_enable);

				// The bool decides the direction now. Hooking the two call
				// sites used to tell enabling from disabling by WHICH site
				// fired; hooking the function reads the argument instead, which
				// is the same answer from a more direct source.
				if (a_enable) {
					ApplyInsideHandler(a_actor, a_enable);
				} else {
					RevertInsideHandler(a_actor);
				}

				return ret;
			}

		private:
			// THIS IS THE POINT OF THE WHOLE EXERCISE. It runs INSIDE the
			// handler, after the engine's own SetUnconscious has finished and
			// before the handler returns to Papyrus - the one place eight
			// rounds of composing public calls could not reach, because a
			// Papyrus call always lands between frames and this does not.
			//
			// AND IT ONLY TOUCHES ACTORS THE CONSUMER ASKED FOR. The reference
			// implementation applies this to every actor anyone makes
			// unconscious, which is right for a knockout mod and wrong for
			// this: a framework that changes what a vanilla call does the
			// moment it is installed is the exact defect 1.0.1 had to fix for
			// SpellTomes, where merely having Lodestone in the load order broke
			// spell tomes for everybody. So the actor has to be in the fallen
			// set, which means KnockoutFall armed it - no registration, no
			// change from vanilla, same rule as every other module here.
			static void ApplyInsideHandler(RE::Actor* a_actor, bool a_enable)
			{
				if (!a_actor || !a_enable) {
					return;
				}

				try {
					const auto formID = a_actor->GetFormID();

					{
						std::lock_guard lock(g_registryLock);
						if (!g_fallen.contains(formID)) {
							return;
						}
					}

					auto* process = a_actor->GetActorRuntimeData().currentProcess;
					if (!process || !a_actor->Is3DLoaded()) {
						spdlog::warn("Incapacitation: [inside] actor (0x{:08X}) is armed but has no AI process "
									 "or no 3D - nothing applied.",
							formID);
						return;
					}

					if (!process->InHighProcess()) {
						auto* owner = process->GetUserData();
						if (!owner || !owner->MoveToHigh()) {
							spdlog::warn("Incapacitation: [inside] could not bring actor (0x{:08X}) to high "
										 "process - nothing applied.",
								formID);
							return;
						}
					}

					// NO KnockExplosion HERE ANY MORE, AND THE MEASUREMENT IS
					// WHY.
					//
					// The trace of a single application on a standing actor: the
					// state written here lasted twelve milliseconds. knockState
					// went 7 -> 2 -> 1 - kDown to kExplodeLeadIn to kExplode -
					// alongside Collision_RecoilCancelable and
					// Collision_SRecoil_FailSafe, and z jumped 65 units in nine
					// milliseconds. The recoil state machine overwrote what we
					// had just written, ran its course, and put the actor back
					// on its feet.
					//
					// That happens with a magnitude of 1.17549e-38, from inside
					// the handler. The place does not change it: KnockExplosion
					// is the recoil, and asking for a recoil at any strength
					// starts the machine that undoes this.
					//
					// It also corrects a reading taken from the round where two
					// applications ran. The second one looked stable there only
					// because the first had already thrown the actor into the
					// air - there was nowhere left to launch it to.
					//
					// AND THE ANSWER CAME BACK: THE STATE ALONE DOES NOT DROP
					// ANYONE. Without the impulse the actor stands pacified and
					// never falls at all, which is worse than falling and
					// getting up. So the impulse is back, and what it costs is
					// known rather than suspected - it starts the recoil, the
					// recoil overwrites this state, and something has to put the
					// state back once the body has settled.
					//
					// Both halves are necessary and neither is sufficient. That
					// is the whole finding of this round, and it is measured on
					// four data points: impulse alone gets up, state alone never
					// falls, the two together seconds apart held, the two
					// together fourteen milliseconds apart froze mid-air.
					const auto position = a_actor->GetPosition();
					a_actor->SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);
					process->KnockExplosion(a_actor, position, kFallNudge);
					a_actor->SetLifeState(RE::ACTOR_LIFE_STATE::kUnconcious);

					a_actor->AsActorState()->actorState1.knockState = RE::KNOCK_STATE_ENUM::kDown;
					const bool sitSleepAccepted =
						a_actor->AsActorState()->DoSetSitSleepState(RE::SIT_SLEEP_STATE::kIsSleeping);

					spdlog::info("Incapacitation: [inside] applied the sequence to actor (0x{:08X}) from "
								 "inside the handler - knock {}, life {}, sit/sleep {} (DoSetSitSleepState "
								 "returned {}), z {:.1f}. Every value here is pre-physics, as always - "
								 "KnockoutRecover carries the verdict.",
						formID, static_cast<std::uint32_t>(ReadKnockState(a_actor)),
						static_cast<std::uint32_t>(ReadLifeState(a_actor)),
						static_cast<std::uint32_t>(ReadSitSleepState(a_actor)), sitSleepAccepted, position.z);
				} catch (...) {
					spdlog::error("Incapacitation: the inside-handler sequence threw - swallowed, the engine "
								  "must not see it.");
				}
			}

			// THE MIRROR, AND IT IS NOT OPTIONAL ANY MORE.
			//
			// While the state never stuck, recovery could afford to be sloppy -
			// 1.12.5 dropped the kGetUp write on the grounds that it had never
			// worked. It had never worked because nothing it was undoing had
			// worked either. The first version that made the fall hold also
			// made the actor impossible to get up, which is the same failure
			// mode this module refused KnockParalyze over, arriving for the
			// third time by a different door.
			//
			// So the revert runs where the apply runs. Same reasoning: if the
			// state only takes hold from inside the handler, it can only be
			// released from inside the handler.
			static void RevertInsideHandler(RE::Actor* a_actor)
			{
				if (!a_actor) {
					return;
				}

				try {
					const auto formID = a_actor->GetFormID();

					{
						std::lock_guard lock(g_registryLock);
						if (!g_fallen.contains(formID)) {
							return;
						}
					}

					if (a_actor->IsDead()) {
						return;
					}

					// kGetUp rather than kNormal: the actor is not standing yet,
					// it is being told to stand. kNormal would claim the
					// transition already happened.
					auto* state = a_actor->AsActorState();
					state->actorState1.knockState = RE::KNOCK_STATE_ENUM::kGetUp;
					state->DoSetSitSleepState(RE::SIT_SLEEP_STATE::kNormal);

					// The same nudge as the way down. The reference
					// implementation makes this call on both halves, and the
					// point is the same on both: it is not a push, it is what
					// drives the physics transition.
					if (auto* process = a_actor->GetActorRuntimeData().currentProcess;
						process && a_actor->Is3DLoaded()) {
						process->KnockExplosion(a_actor, a_actor->GetPosition(), kFallNudge);
					}

					a_actor->Update3DModel();
					a_actor->UpdateActor3DPosition();
					a_actor->EvaluatePackage(true, true);

					spdlog::info("Incapacitation: [inside] reverted actor (0x{:08X}) from inside the handler - "
								 "knock {}, sit/sleep {}, life {}, 3D resynced and package re-evaluated.",
						formID, static_cast<std::uint32_t>(ReadKnockState(a_actor)),
						static_cast<std::uint32_t>(ReadSitSleepState(a_actor)),
						static_cast<std::uint32_t>(ReadLifeState(a_actor)));
				} catch (...) {
					spdlog::error("Incapacitation: the inside-handler revert threw - swallowed, the engine "
								  "must not see it.");
				}
			}

			// Never lets anything escape into the engine, on any path - this
			// runs inside an engine function, which is the least forgiving
			// place in the plugin to throw from.
			static void Report(const char* a_site, RE::Actor* a_actor, bool a_enable)
			{
				try {
					if (!a_actor) {
						spdlog::info("Incapacitation: [proof] SetUnconscious {} call site fired with a null "
									 "actor, enable={}.",
							a_site, a_enable);
						return;
					}

					const auto formID = a_actor->GetFormID();

					bool managed = false;
					bool fallen = false;
					{
						std::lock_guard lock(g_registryLock);
						managed = g_registry.contains(formID);
						fallen = g_fallen.contains(formID);
					}

					spdlog::info("Incapacitation: [proof] SetUnconscious {} call site fired - actor "
								 "(0x{:08X}), enable={}, life {}, knock {}, sit/sleep {}, managed {}, "
								 "fallen {}. Nothing was changed by this hook.",
						a_site, formID, a_enable, static_cast<std::uint32_t>(ReadLifeState(a_actor)),
						static_cast<std::uint32_t>(ReadKnockState(a_actor)),
						static_cast<std::uint32_t>(ReadSitSleepState(a_actor)), managed, fallen);
				} catch (...) {
					spdlog::error("Incapacitation: the SetUnconscious proof hook threw - swallowed.");
				}
			}
		};

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
		} else {
			holder->AddEventSink<RE::TESDeathEvent>(DeathSink::GetSingleton());
			spdlog::info("Incapacitation: death sink registered - a managed actor that dies leaves the "
						 "registry.");
		}

		try {
			// [0] is Actor's primary vtable. InitiateGetUpPackage is declared in
			// Actor's own "add" section rather than as an override of one of the
			// secondary bases, and new virtuals go to the primary vtable.
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_Actor[0] };

			// Both numbers come from the library's own call-time resolution in
			// src/RE/A/Actor.cpp:1993. Do not collapse this to one constant, and
			// do not swap the arms - see the note on the hook above for what a
			// swap silently hooks instead.
			const std::size_t idx = REL::Module::IsVR() ? 0x0E0 : 0x0DE;

			InitiateGetUpPackageHook::func =
				vtbl.write_vfunc(idx, InitiateGetUpPackageHook::thunk);

			spdlog::info("Incapacitation: get-up hook installed on the Actor vtable "
						 "(InitiateGetUpPackage @0x{:X}, {} runtime). Passthrough for every actor this "
						 "module is not holding down.",
				idx, REL::Module::IsVR() ? "VR" : "SE/AE");
		} catch (const std::exception& e) {
			spdlog::error("Incapacitation: failed to install the get-up hook: {} - KnockoutFall will still "
						  "drop an actor, but the engine will stand it back up after a moment.",
				e.what());
		} catch (...) {
			spdlog::error("Incapacitation: failed to install the get-up hook (unknown exception) - "
						  "KnockoutFall will still drop an actor, but the engine will stand it back up "
						  "after a moment.");
		}

		// The SetUnconscious call-site proof. See the hook above for what this
		// is for and why VR is excluded.
		if (REL::Module::IsVR()) {
			spdlog::info("Incapacitation: SetUnconscious call-site proof NOT installed - this is a VR "
						 "runtime and the address is only known for SE and AE. Installing it here would "
						 "resolve the SE id against the VR database and hook something else entirely.");
			return;
		}

		try {
			// The call site is only used to FIND the function - nothing is
			// written to it. Two of them were redirected in 1.12.8 and both
			// resolved to the same address, which is what makes reading one
			// enough.
			const REL::Relocation<std::uintptr_t> callSite{ REL::RelocationID(21874, 22356),
				REL::Relocate(0xC8, 0xC8) };
			const auto target = ResolveCallTarget(callSite.address());

			if (!target) {
				spdlog::error("Incapacitation: the address at 0x{:X} does not hold a five-byte call, so the "
							  "SetUnconscious target could not be resolved - the fall is unavailable. This "
							  "means the id or the offset no longer points where it used to.",
					callSite.address());
				return;
			}

			// Inline detour of the function body, which is SafetyHook's job -
			// the trampoline primitives redirect call and jump sites and cannot
			// do this, which is the distinction CONVENTIONS.md records. The
			// project already depends on SafetyHook for exactly this reason.
			g_setUnconsciousHook = safetyhook::create_inline(
				reinterpret_cast<void*>(target), reinterpret_cast<void*>(&SetUnconsciousHook::thunk));

			if (!g_setUnconsciousHook) {
				spdlog::error("Incapacitation: SafetyHook refused to hook the SetUnconscious target at "
							  "0x{:X} - the fall is unavailable, knockouts otherwise unaffected.",
					target);
				return;
			}

			spdlog::info("Incapacitation: SetUnconscious hooked at 0x{:X}, resolved from the call site at "
						 "0x{:X}. Every caller passes through here now - console, Papyrus and anything "
						 "else - and nothing happens to an actor KnockoutFall did not register.",
				target, callSite.address());
		} catch (const std::exception& e) {
			spdlog::error("Incapacitation: failed to hook SetUnconscious: {} - the fall is unavailable.",
				e.what());
		} catch (...) {
			spdlog::error("Incapacitation: failed to hook SetUnconscious (unknown exception) - the fall is "
						  "unavailable.");
		}
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
