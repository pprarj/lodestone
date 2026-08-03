// CastTime.cpp
// Lodestone - Shared SKSE framework
//
// Production implementation of the cast time capability.
//
// Phase L0 moved this module from Domain to Core. The hook, the filter, and the
// formula are UNCHANGED and stay validated: every rule in Apply() below still
// comes from a specific line of the original Part 1.5 trace log, run on TWO
// independent load orders (one heavy modded one, and a clean vanilla new game). What
// changed is only the SOURCE of the two globals: they used to be looked up by a
// hardcoded ESP + FormID on kDataLoaded; they are now REGISTERED at runtime by the
// consumer, which passes its own GlobalVariable records through a native. The
// per-cast behavior is identical once a consumer registers the same two globals.
//
// HOOK TARGET (Part 1, read from the CommonLibSSE-NG headers):
//
//   RE/M/MagicCaster.h
//     virtual void SetCastingTimerForCharge();   // 14   <-- hook target
//     virtual Actor* GetCasterAsActor() const;   // 0C   <-- gives us the actor
//     MagicItem* currentSpell;                   // 28
//     float      castingTimer;                   // 34
//
//   RE/Offsets_VTABLE.h:1862
//     VTABLE_ActorMagicCaster{ VariantID(257613, 205828, 0x16aef00), ... }
//     [0] is the MagicCaster branch of the multiple inheritance. Index 0x14
//     lives in that branch, ahead of the VR-only virtuals at 0x1D/0x1E, so it
//     is stable on SE / AE / VR.
//
// REJECTED TARGET: SpellItem::GetChargeTime (vfunc 0x64). It is const, lives on
// the spell, and takes no caster - there is no way to know who is casting, which
// kills the future NPC path (C.5). The trace also proved it is the wrong VALUE
// (see ACHADO 1 below), so it was a trap twice over.
//
// EXCEPTION SAFETY: a C++ exception escaping a hook into the engine is undefined
// behavior. The thunk wraps its body. This mirrors the Stage B.3 convention for
// natives, applied to hooks: nothing this plugin installs may throw across an
// engine boundary.
//
// Phase L0 (was Phase 16 - Stage C.1 / Part 2)

#include "CastTime.h"

namespace Lodestone::Core::CastTime
{
	namespace
	{
		// -------------------------------------------------------------------
		// The registered channel - the Papyrus -> native link
		//
		// This used to be resolved on kDataLoaded from a hardcoded ESP. It is now
		// handed in by the consumer through RegisterCastTimeChannel (see below).
		//
		// Empty => no consumer has registered => passthrough, and the hook costs
		// one acquire load and a null compare. That is the same "silent
		// degradation" property the old lookup had: "not registered yet" reads
		// identically to "not installed".
		//
		// MULTI-CONTRIBUTOR SINCE 1.9.0. Until 1.8.2 this was two loose
		// std::atomic<TESGlobal*> owned by the first registrant, and a second,
		// different registrant was rejected. MultiChannel replaces both the
		// storage and that policy; the threading discipline is the same one those
		// atomics implemented (hook reads lock-free on the game thread,
		// registrants serialise against each other on a mutex the hook never
		// touches) and is now written down in one place instead of four.
		// -------------------------------------------------------------------
		MultiChannel g_channel{ "CastTime" };

		// -------------------------------------------------------------------
		// Debug trace
		//
		// Compiled in, absent from Release: Log.cpp sets the level to `info` when
		// NDEBUG is defined, so spdlog::debug writes nothing in a shipping build.
		//
		// The should_log guard is not redundant with that. spdlog skips
		// FORMATTING below the active level, but the ARGUMENTS are still
		// evaluated at the call site - and GetName() is a virtual call inside a
		// hook that fires for every ability of every actor in the cell. The guard
		// makes the Release cost exactly one integer compare.
		//
		// What is deliberately NOT here, versus the Part 1.5 trace: school,
		// CastingSource, GetChargeTime(), the enum names. Those were
		// investigation scaffolding. This logs only what the module actually
		// uses.
		// -------------------------------------------------------------------
		bool ShouldLogCasts()
		{
			auto* logger = spdlog::default_logger_raw();
			return logger && logger->should_log(spdlog::level::debug);
		}

		// -------------------------------------------------------------------
		// The rule
		//
		// Ordered cheapest-first on purpose. The Part 1.5 trace measured 316 hook
		// events in roughly one minute of play - the hook fires for every ability
		// of every actor in the cell, and that volume scales with the modlist,
		// not with what the player does. Of those 316, exactly 4 reached the
		// formula. Every early return below is carrying real weight.
		// -------------------------------------------------------------------
		void Apply(RE::MagicCaster* a_this)
		{
			// 0. Anyone registered? One acquire load and a compare, which cuts
			//    100% when no consumer has registered. Deliberately NOT the
			//    composed read: that walks the contributors and reads their
			//    globals, and this hook fires 316 times a minute to reach the
			//    formula 4 times. The values are read at step 4, past every
			//    filter, exactly where the two loose atomics used to be read.
			if (!g_channel.HasContributors()) {
				return;
			}

			// 1. Player only. C.1 is player-scoped by decision, not by
			//    limitation - GetCasterAsActor() (0x0C) returns the actor for
			//    NPCs too. This is the reserved seam for C.5, which will compute
			//    from the actor's school skill instead of reading the globals.
			auto* actor = a_this->GetCasterAsActor();
			if (!actor || !actor->IsPlayerRef()) {
				return;
			}

			auto* magicItem = a_this->currentSpell;
			if (!magicItem) {
				return;
			}

			// 2. ConstantEffect out. This is where the volume goes: abilities,
			//    racial powers, perk dummies, and other mods' constant-effect
			//    controllers reapplied in a loop.
			if (magicItem->GetCastingType() == RE::MagicSystem::CastingType::kConstantEffect) {
				return;
			}

			// 3. The discriminator. NOT castingType, and NOT castingTimer.
			//
			//    ACHADO 2 (Part 1.5, confirmed on BOTH load orders): the engine
			//    floors castingTimer at 0.0001 even for pure concentration with a
			//    zero charge time in the record.
			//
			//      Flames (vanilla, 0x00012FCD)  SPIT.chargeTime=0.0000  timer=0.0001
			//                            ^ a mod may rename this spell, but the
			//                              0.0001 floor is the ENGINE's, not any
			//                              mod's - which is exactly why a second,
			//                              clean vanilla load order was tested.
			//
			//    A `castingTimer <= 0` test would therefore NEVER fire, and
			//    (0.0001 * mult) + offset would have invented ~0.5s of charge on
			//    a pure concentration spell - the precise bug this check exists
			//    to prevent, and one that would have survived Part 3 validation.
			//
			//    So the two values have separate jobs, and must not be confused:
			//      SPIT.chargeTime -> discriminator ("does this spell charge?")
			//      castingTimer    -> the formula's base (see below)
			//
			//    SPIT.chargeTime > 0 on a Concentration spell is legal record
			//    format, not a mod hack: Morokei Channel (0x000F82B4, charge=3)
			//    ships in the base game. Other concentration spells with a real
			//    charge time validated it in the trace. Reading the record field
			//    means this works on any load order without knowing which mods
			//    are present.
			auto* spell = magicItem->As<RE::SpellItem>();
			if (!spell) {
				// EnchantmentItem and friends derive from MagicItem but not from
				// SpellItem - no SPIT to read, not our business.
				return;
			}

			if (spell->data.chargeTime <= 0.0f) {
				return;
			}

			// 4. Apply.
			//
			//    ACHADO 1 (Part 1.5, critical): the base is castingTimer AFTER
			//    the original ran - never GetChargeTime(), never
			//    SPIT.chargeTime.
			//
			//      Firebolt dual cast | timer after=0.6500
			//                         | GetChargeTime()=0.4000 SPIT.chargeTime=0.4000
			//
			//    The dual cast multiplier (1.625x, perks already folded in) lives
			//    ONLY in castingTimer. Building the formula on either of the other
			//    two would silently discard dual casting and every charge-time
			//    perk.
			const float before = a_this->castingTimer;

			// The contributors are composed here, into locals, so the formula
			// sees a consistent multiplier and offset even if a registration
			// lands mid-hook (worst case in that instant: one passthrough cast).
			// This is where the two loose atomics used to be dereferenced, and it
			// fails only if every contributor unregistered since step 0 - which
			// leaves the timer at the engine's own value.
			float multVal = 0.0f, offsetVal = 0.0f;
			if (!g_channel.Read(multVal, offsetVal)) {
				return;
			}

			float after = (before * multVal) + offsetVal;

			// A negative timer is not a state the engine should ever be handed.
			// The globals are MCM-tunable, so this is reachable from a user
			// setting, not just from a bug. No artificial floor above zero: an
			// instant charge is a legitimate outcome at high INT, and inventing a
			// minimum here would be speculation.
			if (after < 0.0f) {
				after = 0.0f;
			}

			a_this->castingTimer = after;

			if (ShouldLogCasts()) {
				spdlog::debug("CastTime: '{}' (0x{:08X}) {:.4f} -> {:.4f} (mult={:.4f} offset={:.4f})",
					magicItem->GetName() ? magicItem->GetName() : "<unnamed>",
					magicItem->GetFormID(),
					before, after, multVal, offsetVal);
			}
		}

		// -------------------------------------------------------------------
		// Hook: MagicCaster::SetCastingTimerForCharge - vfunc 0x14
		//
		// The original runs FIRST and unconditionally. It is what writes the
		// vanilla value into castingTimer, which is the base we then scale:
		//
		//   [SetCastingTimerForCharge] Firebolt | before=0.0000 after=0.4000
		//
		// If everything after that call fails, vanilla behavior is still intact.
		// -------------------------------------------------------------------
		struct SetCastingTimerForChargeHook
		{
			static void thunk(RE::MagicCaster* a_this)
			{
				func(a_this);

				if (!a_this) {
					return;
				}

				try {
					Apply(a_this);
				} catch (...) {
					// Swallow. Cast time is a gameplay nicety; taking the game
					// down over it is not a trade this plugin makes.
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x14 };
		};

		// -------------------------------------------------------------------
		// REMOVED: MagicCaster::StartChargeImpl - vfunc 0x04
		//
		// Part 1.5 hooked 0x04 to answer one question: is it a concurrent path
		// to castingTimer? If it fired without 0x14, or wrote the timer itself,
		// 0x14 would be the wrong target and this module would have to move.
		//
		// The trace answered it. 0x04 runs AFTER 0x14, reads what 0x14 left, and
		// writes nothing. In 5 of 5 events, across both load orders:
		//
		//   [SetCastingTimerForCharge] Firebolt | before=0.0000 after=0.4000
		//   [StartChargeImpl]          Firebolt | before=0.4000 after=0.4000
		//
		// So the hook is gone rather than kept as a no-op. A thunk that calls the
		// original and returns is functionally IDENTICAL to not hooking, while
		// still paying: one more slot swapped in a vtable other SKSE plugins also
		// hook (install-order conflict surface, bought for nothing), one more
		// frame in every charge path, and a piece of code whose only future is to
		// cost someone the time it takes to work out that it does nothing.
		//
		// It is not a safety net either - a no-op protects against nothing. If
		// evidence ever shows something overwriting the timer between 0x14 and
		// the cast, 0x04 becomes the reapplication point. That would be a
		// decision made on new evidence, not code left lying around in advance.
		//
		// The implementation is in git history and in the Part 1.5 summary.
		// -------------------------------------------------------------------

		// -------------------------------------------------------------------
		// Native: Lodestone.RegisterCastTimeChannel(GlobalVariable, GlobalVariable) -> Bool
		//
		// The consumer hands in the two globals it owns and drives from its own
		// Papyrus state. From this call on, the hook scales the player's cast time
		// by those globals' value fields. Before it, the hook is passthrough.
		//
		// MULTI-CONTRIBUTOR SINCE 1.9.0 - the one semantics change in this
		// version, and the signature is untouched by it:
		//   Every distinct plugin that registers contributes. Multipliers compose
		//   by product, offsets by sum, applied once. Re-registering the SAME pair
		//   from the same plugin is still an idempotent refresh, which is what a
		//   consumer re-registering on every game load relies on. Until 1.8.2 a
		//   second, DIFFERENT registrant was warned and rejected; it is now
		//   accepted and composed, and MultiChannel.h says why that policy had to
		//   go.
		//
		// Error convention (Stage B.3): reports failure by return value, never by
		// throwing. Returns true when the CALLER's pair is contributing after this
		// call - which now includes the case where other plugins contribute too -
		// and false on a null argument.
		// -------------------------------------------------------------------
		bool RegisterCastTimeChannel(RE::StaticFunctionTag*, RE::TESGlobal* a_multiplier, RE::TESGlobal* a_offset)
		{
			return g_channel.Register(a_multiplier, a_offset);
		}
	}

	MultiChannel& GetChannel()
	{
		return g_channel;
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("CastTime: null VM, cannot register natives.");
			return false;
		}

		// Registered on the "Lodestone" script (same script as PluginInfo), so the
		// consumer calls it as Lodestone.RegisterCastTimeChannel(mult, offset).
		a_vm->RegisterFunction("RegisterCastTimeChannel", "Lodestone", RegisterCastTimeChannel);

		spdlog::info("CastTime: natives registered (RegisterCastTimeChannel).");
		return true;
	}

	void Install()
	{
		try {
			// [0] = the MagicCaster branch of ActorMagicCaster's multiple
			// inheritance (MagicCaster@00, SimpleAnimationGraphManagerHolder@48,
			// BSTEventSink@60). Index 0x14 lives in that branch.
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_ActorMagicCaster[0] };

			SetCastingTimerForChargeHook::func =
				vtbl.write_vfunc(SetCastingTimerForChargeHook::idx, SetCastingTimerForChargeHook::thunk);

			spdlog::info("CastTime: hook installed on ActorMagicCaster vtable (SetCastingTimerForCharge @0x14). "
						 "No channel registered yet - passthrough until a consumer calls "
						 "Lodestone.RegisterCastTimeChannel.");
		} catch (const std::exception& e) {
			spdlog::error("CastTime: failed to install: {} - cast time will not be modified.", e.what());
		} catch (...) {
			spdlog::error("CastTime: failed to install (unknown exception) - cast time will not be modified.");
		}
	}
}
