// CastTime.cpp
// Lodestone - Shared SKSE framework
//
// Production implementation of the cast time capability (the external plugin replacement).
//
// Phase L0 moved this module from Domain to Core. The hook, the filter, and the
// formula are UNCHANGED and stay validated: every rule in Apply() below still
// comes from a specific line of the original Part 1.5 trace log, run on TWO
// independent load orders (an overhaul mod active, and a clean vanilla new game). What
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

#include <atomic>
#include <mutex>

namespace Lodestone::Core::CastTime
{
	namespace
	{
		// -------------------------------------------------------------------
		// The registered channel - the Papyrus -> native link
		//
		// These used to be resolved on kDataLoaded from a hardcoded ESP. They are
		// now handed in by the consumer through RegisterCastTimeChannel (see
		// below). The per-cast cost is still two float reads through two stable
		// pointers.
		//
		// Both null => no consumer has registered => passthrough, and the hook
		// costs two pointer compares. That is the same "silent degradation"
		// property the old lookup had: "not registered yet" reads identically to
		// "not installed".
		//
		// THREADING: the hook (Apply) runs on the game thread; registration runs
		// on a Papyrus VM thread. The pointers are therefore atomic so the hook
		// reads them lock-free with no torn/reordered read. On x64 an acquire load
		// of an aligned pointer is a plain mov, so the hot path pays nothing over a
		// raw pointer read. g_registerLock serializes registrants against each
		// other only (the check-then-store decision); the hook never touches it.
		// -------------------------------------------------------------------
		std::atomic<RE::TESGlobal*> g_multiplier{ nullptr };
		std::atomic<RE::TESGlobal*> g_offset{ nullptr };
		std::mutex                  g_registerLock;

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
			// 0. Channel registered? Two acquire loads + two pointer compares,
			//    cuts 100% when no consumer has registered. Loaded into locals
			//    once so the formula below reads a consistent pair even if a
			//    registration lands mid-hook (worst case: one extra passthrough
			//    cast during that instant).
			RE::TESGlobal* mult   = g_multiplier.load(std::memory_order_acquire);
			RE::TESGlobal* offset = g_offset.load(std::memory_order_acquire);
			if (!mult || !offset) {
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
			//    racial powers, perk dummies, third-party effects (other mods,
			//    other mods, an overhaul mod's GM controllers) reapplied in a loop.
			if (magicItem->GetCastingType() == RE::MagicSystem::CastingType::kConstantEffect) {
				return;
			}

			// 3. The discriminator. NOT castingType, and NOT castingTimer.
			//
			//    ACHADO 2 (Part 1.5, confirmed on BOTH load orders): the engine
			//    floors castingTimer at 0.0001 even for pure concentration with a
			//    zero charge time in the record.
			//
			//      a modded spell (an overhaul mod)  SPIT.chargeTime=0.0000  timer after=0.0001
			//      Flames    (vanilla)   SPIT.chargeTime=0.0000  timer after=0.0001
			//                            ^ same FormID 0x00012FCD, an overhaul mod only
			//                              renames it. The floor is the ENGINE's,
			//                              not an overhaul mod's - which is exactly why
			//                              the second load order was tested.
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
			//    ships in Skyrim.esm. Absorbing Grasp (an overhaul mod, charge=0.3)
			//    validated it in the trace. Magic Redone / Expanded Grimoire use
			//    the same pattern. Reading the record field means this works on
			//    any load order without knowing those mods exist.
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
			const float before   = a_this->castingTimer;
			const float multVal   = mult->value;
			const float offsetVal = offset->value;

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
		// SINGLE CHANNEL - v1 DECISION (the one semantics choice worth a review):
		//   v1 is one channel, first-registrant-wins. Re-registering the SAME pair
		//   is an idempotent refresh (the consumer may re-register on every game
		//   load) and is accepted silently. A DISTINCT second registrant while a
		//   channel is already held is warned and REJECTED - the held channel is
		//   left untouched and the native returns false. This is standard SKSE
		//   framework posture (external plugin was the single owner of this same hook). Rich
		//   arbitration between several cast-time mods (chaining, summing,
		//   priority) is a FUTURE improvement, intentionally not built here and not
		//   a blocker for going public.
		//
		// Error convention (Stage B.3): reports failure by return value, never by
		// throwing. Returns true when the CALLER's channel is the active one after
		// this call; false on a null argument or a rejected second registrant.
		// -------------------------------------------------------------------
		bool RegisterCastTimeChannel(RE::StaticFunctionTag*, RE::TESGlobal* a_multiplier, RE::TESGlobal* a_offset)
		{
			if (!a_multiplier || !a_offset) {
				spdlog::warn("CastTime: RegisterCastTimeChannel got a null global (multiplier={}, offset={}) - ignored.",
					a_multiplier ? "ok" : "NULL",
					a_offset ? "ok" : "NULL");
				return false;
			}

			std::lock_guard<std::mutex> lock(g_registerLock);

			RE::TESGlobal* curMult   = g_multiplier.load(std::memory_order_acquire);
			RE::TESGlobal* curOffset = g_offset.load(std::memory_order_acquire);

			if (curMult && curOffset) {
				// A channel is already held.
				if (curMult == a_multiplier && curOffset == a_offset) {
					// Same pair - idempotent refresh (e.g. re-register on reload).
					spdlog::debug("CastTime: channel re-registered with the same globals - no-op.");
					return true;
				}

				// Distinct second registrant - single-channel v1 policy.
				spdlog::warn("CastTime: a second, DIFFERENT cast time channel tried to register "
							 "(new multiplier=0x{:08X}, offset=0x{:08X}); the first channel "
							 "(multiplier=0x{:08X}, offset=0x{:08X}) keeps ownership. Ignoring the new one.",
					a_multiplier->GetFormID(), a_offset->GetFormID(),
					curMult->GetFormID(), curOffset->GetFormID());
				return false;
			}

			// First registration. Store offset first, multiplier last: the hook's
			// guard checks both, and publishing the multiplier last (release) means
			// a hook that observes a set multiplier also observes the offset.
			g_offset.store(a_offset, std::memory_order_release);
			g_multiplier.store(a_multiplier, std::memory_order_release);

			spdlog::info("CastTime: cast time channel registered (multiplier=0x{:08X}, offset=0x{:08X}) - module ACTIVE.",
				a_multiplier->GetFormID(), a_offset->GetFormID());
			return true;
		}
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
