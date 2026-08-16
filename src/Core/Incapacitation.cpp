// Incapacitation.cpp
// Lodestone - Shared SKSE framework
//
// The managed non-lethal knockout module. See Incapacitation.h for the
// contract and why it has no engine hook and no native timer.
//
// Phase: Hook A (managed non-lethal knockout)

#include "Incapacitation.h"

#include <limits>
#include <mutex>
#include <unordered_map>
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

		// How many animation events to write per knockout before going quiet.
		// High enough to cover the fall and the second or two in which the actor
		// gets back up - which is the whole question - and low enough that one
		// knockout cannot bury the log.
		constexpr std::uint32_t kMaxLoggedAnimEvents = 60;

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

					bool report = false;
					{
						std::lock_guard lock(g_registryLock);
						const auto it = g_fallen.find(formID);
						if (it == g_fallen.end()) {
							return RE::BSEventNotifyControl::kContinue;
						}

						if (it->second.loggedAnimEvents < kMaxLoggedAnimEvents) {
							++it->second.loggedAnimEvents;
							report = true;
						} else if (it->second.loggedAnimEvents == kMaxLoggedAnimEvents) {
							++it->second.loggedAnimEvents;
							spdlog::info("Incapacitation: [obs] actor (0x{:08X}) - reached {} logged animation "
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

			// MANAGED STATE IS NO LONGER REQUIRED, AND THE REASON IS AN
			// EXPERIMENT THE OLD RULE MADE IMPOSSIBLE TO RUN.
			//
			// Requiring it forced KnockoutActor to run first, which meant this
			// function was only ever called on an actor already in kUnconcious.
			// The reference implementation reaches the same physical sequence
			// from a LIVING actor, after the engine's own SetUnconscious native
			// has run - a native CommonLibSSE-NG does not expose, so the only
			// way to put it in front of this sequence is from the consumer's
			// Papyrus, and the old precondition refused exactly that ordering.
			//
			// Relaxing it costs nothing structurally: the fallen set has always
			// been separate from the managed registry, precisely because being
			// on the ground and being managed-unconscious do not start or end
			// together. KnockoutRecover works off the fallen set alone and does
			// not care whether the actor was ever managed.
			bool managed = false;
			{
				std::lock_guard lock(g_registryLock);
				managed = g_registry.contains(formID);
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

			// IsDownKnockState rather than "anything but kNormal": kGetUp is an
			// actor on its way back to its feet, which is the opposite of a
			// reason to skip knocking it down.
			const auto guardKnockState = ReadKnockState(a_actor);
			const bool engineHasItDown = a_actor->IsInRagdollState() && IsDownKnockState(guardKnockState);

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
				g_fallen.emplace(formID, FallenState{});
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
				const auto beforeSit = ReadSitSleepState(a_actor);

				// THE ORDER IS THE MECHANISM. Every earlier version performed
				// one piece of this sequence in isolation, and no piece works
				// alone.
				//
				// 1. The AI process has to be at high, or the physics call has
				//    nothing to act on.
				if (!process->InHighProcess()) {
					auto* owner = process->GetUserData();
					if (!owner || !owner->MoveToHigh()) {
						spdlog::warn("Incapacitation: KnockoutFall could not bring actor (0x{:08X}) to high "
									 "process - refused, the physics transition would have nothing to act on.",
							formID);
						return false;
					}
				}

				// 2. Back to kAlive first. This looks like undoing the knockout
				//    and is not: what follows is a transition OUT of a living
				//    state, and every previous version started it from
				//    kUnconcious - which is where KnockoutActor leaves the
				//    actor - asking the engine to leave a state it was already
				//    in.
				a_actor->SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);

				// 3. The nudge, from the actor's own position. With a magnitude
				//    this small there is no force worth aiming, and the 64-unit
				//    offset earlier versions used existed to aim a shove that
				//    should never have been a shove.
				const auto position = a_actor->GetPosition();
				process->KnockExplosion(a_actor, position, kFallNudge);

				// 4. And back down.
				a_actor->SetLifeState(RE::ACTOR_LIFE_STATE::kUnconcious);

				// 5. The state that is meant to hold, and this is the one piece
				//    nobody had tried.
				//
				//    knockState is written for completeness and is NOT expected
				//    to carry anything: 1.12.2 proved the engine overwrites it,
				//    and nothing since has contradicted that.
				//
				//    sitSleepState is the real candidate, and it is set through
				//    DoSetSitSleepState rather than by writing the bitfield.
				//    That distinction is the whole lesson of this trail. There
				//    is no SetKnockState anywhere in the library - knockState
				//    has a getter and nothing else, which is consistent with it
				//    being something the engine reports. sitSleepState has a
				//    dedicated virtual (ActorState.h:153), so the engine has a
				//    formal way to be TOLD, and going through it runs the
				//    engine's own code instead of poking the bit it derives.
				//
				//    Honest about the odds: nothing in the headers promises
				//    this sticks where knockState did not. The reference
				//    implementation appears to write the bitfield directly and
				//    works in game, so the raw write may well be enough - but
				//    given a choice between poking a field and calling the
				//    function the engine provides for it, this trail has
				//    already paid to learn which one is worth trying first.
				a_actor->AsActorState()->actorState1.knockState = RE::KNOCK_STATE_ENUM::kDown;
				const bool sitSleepAccepted =
					a_actor->AsActorState()->DoSetSitSleepState(RE::SIT_SLEEP_STATE::kIsSleeping);

				// Both of the above are OURS. Saying so in the log is not
				// decoration: round 7 read `knock 0 -> 7` as the engine finally
				// choosing kDown, which would have been real progress, and it
				// was this write. A number in a log that does not say who put it
				// there costs a round to disambiguate.

				// EVERY READING HERE IS TAKEN TOO EARLY TO MEAN MUCH, and that
				// is worth stating in the file rather than relearning. The
				// physics and the state machine both run over the frames after
				// this returns: 1.12.2 reported `knock state 0 -> 0` and the
				// animation trace caught the same actor at knock state 2 ten
				// milliseconds later. That reading is why 1.12.2 concluded the
				// engine chose nothing and went off to write the field by hand.
				//
				// What these numbers are good for is the BEFORE half, and for
				// showing that each step of the sequence was reached. The AFTER
				// half of the story is the animation trace and the readings
				// KnockoutRecover takes at the end of the window.
				spdlog::info("Incapacitation: KnockoutFall on actor (0x{:08X}) - sequence applied, managed {}. "
							 "knock {} -> {} (WRITTEN BY US, not the engine choosing), life {} -> {}, "
							 "sit/sleep {} -> {} (WRITTEN BY US, DoSetSitSleepState returned {}), z {:.1f}, "
							 "nudge {:g}. Every reading here is pre-physics - the trace and KnockoutRecover "
							 "carry the outcome.",
					formID, managed, static_cast<std::uint32_t>(guardKnockState),
					static_cast<std::uint32_t>(ReadKnockState(a_actor)),
					static_cast<std::uint32_t>(beforeLife), static_cast<std::uint32_t>(ReadLifeState(a_actor)),
					static_cast<std::uint32_t>(beforeSit), static_cast<std::uint32_t>(ReadSitSleepState(a_actor)),
					sitSleepAccepted, position.z, kFallNudge);

				{
					// The sequence above moved the life state. If nothing else
					// is managing this actor, this call owns putting it back -
					// see FallenState::ownsLifeState.
					FallenState state;
					state.ownsLifeState = !managed;

					std::lock_guard lock(g_registryLock);
					g_fallen.emplace(formID, state);
				}

				// Registered after the entry exists, because the sink filters on
				// it - an event arriving between the two would be discarded.
				// Observation only; this changes nothing about the fall.
				a_actor->AddAnimationGraphEventSink(AnimationSink::GetSingleton());
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

				// knockState is not written here. 1.12.2 settled that the field
				// is the engine's to set, and writing kGetUp into an actor still
				// in life state 3 is an interaction the consumer flagged and
				// nobody has tested. Nothing needs reordering on either side.

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
		// TWO CALL SITES, ONE FUNCTION. The handler calls the same implementation
		// from two branches - one enabling, one disabling - so hooking both is
		// what tells the two apart. Offsets and the ID come from the reference
		// implementation's source, which makes them a hypothesis until this pass
		// says otherwise.
		//
		// NOT INSTALLED ON VR, AND THE REASON IS A TRAP WORTH WRITING DOWN.
		// REL::RelocationID's two-argument constructor assigns the SE id to the
		// VR slot (ID.h:480) rather than refusing. VR has its own Address
		// Library numbering, so on VR that silently resolves to whatever else
		// id 21874 happens to be - a wrong address that installs quietly. The
		// reference implementation uses the same constructor and does not target
		// VR; this plugin does, so the guard is ours to add.
		// -------------------------------------------------------------------

		using SetUnconsciousFn = bool (*)(RE::Actor*, bool);

		struct SetUnconsciousCallHook
		{
			static bool EnableThunk(RE::Actor* a_actor, bool a_enable)
			{
				Report("enable", a_actor, a_enable);
				return reinterpret_cast<SetUnconsciousFn>(enableOriginal)(a_actor, a_enable);
			}

			static bool DisableThunk(RE::Actor* a_actor, bool a_enable)
			{
				Report("disable", a_actor, a_enable);
				return reinterpret_cast<SetUnconsciousFn>(disableOriginal)(a_actor, a_enable);
			}

			static inline std::uintptr_t enableOriginal = 0;
			static inline std::uintptr_t disableOriginal = 0;

		private:
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
			// First and only trampoline user in this plugin - every other hook
			// here is a vtable swap or SafetyHook, which manage their own. Two
			// unique destinations at 14 bytes each; 64 is room to spare. If a
			// second module ever needs the trampoline, this call has to move
			// somewhere both can share, because it replaces the allocation
			// rather than adding to it.
			SKSE::AllocTrampoline(64);
			auto& trampoline = SKSE::GetTrampoline();

			// write_call<5> and not write_branch<5>: the target is an existing
			// `call rel32` (0xE8), not a jmp. Both primitives redirect an
			// existing call or jump site and neither detours a function body -
			// which is the distinction CONVENTIONS.md records, and this is the
			// use it was always the right tool for.
			const REL::Relocation<std::uintptr_t> enableTarget{ REL::RelocationID(21874, 22356),
				REL::Relocate(0xC8, 0xC8) };
			const REL::Relocation<std::uintptr_t> disableTarget{ REL::RelocationID(21874, 22356),
				REL::Relocate(0x120, 0x120) };

			SetUnconsciousCallHook::enableOriginal =
				trampoline.write_call<5>(enableTarget.address(), SetUnconsciousCallHook::EnableThunk);
			SetUnconsciousCallHook::disableOriginal =
				trampoline.write_call<5>(disableTarget.address(), SetUnconsciousCallHook::DisableThunk);

			spdlog::info("Incapacitation: SetUnconscious call-site proof installed - enable @0x{:X}, "
						 "disable @0x{:X}, originals 0x{:X} and 0x{:X}. LOG ONLY, no behaviour attached. "
						 "Both thunks call the original and return its answer.",
				enableTarget.address(), disableTarget.address(), SetUnconsciousCallHook::enableOriginal,
				SetUnconsciousCallHook::disableOriginal);
		} catch (const std::exception& e) {
			spdlog::error("Incapacitation: failed to install the SetUnconscious call-site proof: {}", e.what());
		} catch (...) {
			spdlog::error("Incapacitation: failed to install the SetUnconscious call-site proof "
						  "(unknown exception).");
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
