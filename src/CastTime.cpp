// CastTime.cpp
// Intelligence Matters - SKSE plugin
//
// Production implementation of Stage C.1: the external plugin replacement.
//
// Every rule below comes from a specific line of the Part 1.5 trace log, run on
// TWO independent load orders (an overhaul mod active, and a clean vanilla new game).
// Nothing here is inferred. The evidence is quoted inline at each decision.
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
// Phase 16 - Stage C.1 / Part 2

#include "CastTime.h"

namespace IMPlugin::CastTime
{
	namespace
	{
		// -------------------------------------------------------------------
		// Globals - the Papyrus -> native channel
		//
		// Cached once on kDataLoaded. Form pointers are stable for the lifetime
		// of the process once plugin data is loaded, so this is a lookup per
		// GAME LOAD, not per cast. The per-cast cost is two float reads.
		//
		// Both null => the CastTime module ESP is not installed => passthrough,
		// and the hook costs one pointer compare. That is the "silent
		// degradation" property the globals channel was chosen for.
		// -------------------------------------------------------------------

		constexpr auto kCastTimeFile = "IntelligenceMatters_CastTime.esp";

		// CAUTION, confirmed in TESDataHandler.cpp:136-139 - the FormID is
		// assembled by ADDITION, not by OR:
		//
		//     formID  = file->compileIndex << 24;            // 0xFE << 24
		//     formID += file->smallFileCompileIndex << 12;   // 0x001 << 12
		//     formID += a_localFormID;                       // low 12 bits ONLY
		//
		// CastTime.esp is an ESL (FE 001), so the full IDs seen in xEdit are
		// FE001002 / FE001003. Passing those whole would silently produce
		// garbage. Only the low 12 bits go in.
		constexpr RE::FormID kMultiplierLocalID = 0x002;  // IM_CT_Multiplier -> FE001002
		constexpr RE::FormID kOffsetLocalID     = 0x003;  // IM_CT_Offset     -> FE001003

		RE::TESGlobal* g_multiplier = nullptr;
		RE::TESGlobal* g_offset     = nullptr;

		// Resolves both globals. Leaves them null on any failure, which is a
		// valid, expected state (module not installed) - not an error.
		void ResolveGlobals()
		{
			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			if (!dataHandler) {
				spdlog::error("CastTime: no TESDataHandler - cast time will not be modified.");
				return;
			}

			g_multiplier = dataHandler->LookupForm<RE::TESGlobal>(kMultiplierLocalID, kCastTimeFile);
			g_offset     = dataHandler->LookupForm<RE::TESGlobal>(kOffsetLocalID, kCastTimeFile);

			// This pair of lines is the most important thing this module logs.
			// It is the difference between "cast time is broken" and "the
			// CastTime module is not installed" - which, without it, is
			// indistinguishable from the outside and unanswerable in a support
			// thread.
			if (g_multiplier && g_offset) {
				spdlog::info("CastTime: globals resolved from {} (IM_CT_Multiplier=0x{:08X}, IM_CT_Offset=0x{:08X}) - module ACTIVE.",
					kCastTimeFile, g_multiplier->GetFormID(), g_offset->GetFormID());
			} else {
				// Partial resolution means the ESP is loaded but its records do
				// not match what this build expects - a real problem, unlike a
				// clean "both null".
				if (g_multiplier || g_offset) {
					spdlog::warn("CastTime: {} is loaded but only ONE of the two globals resolved "
								 "(multiplier={}, offset={}). Treating as inactive.",
						kCastTimeFile,
						g_multiplier ? "ok" : "MISSING",
						g_offset ? "ok" : "MISSING");
					g_multiplier = nullptr;
					g_offset = nullptr;
				} else {
					spdlog::info("CastTime: {} not found - module INACTIVE, cast time untouched (passthrough).",
						kCastTimeFile);
				}
			}
		}

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
			// 0. Module installed? Two pointer compares, cuts 100% when the
			//    CastTime ESP is absent.
			if (!g_multiplier || !g_offset) {
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
			const float before = a_this->castingTimer;
			const float mult   = g_multiplier->value;
			const float offset = g_offset->value;

			float after = (before * mult) + offset;

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
					before, after, mult, offset);
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
	}

	void Install()
	{
		try {
			// Globals first: if the hook installs and the lookup then throws, the
			// hook would be live with null globals. That is harmless by design
			// (passthrough), but this ordering means the log tells the truth
			// about state in every case.
			ResolveGlobals();

			// [0] = the MagicCaster branch of ActorMagicCaster's multiple
			// inheritance (MagicCaster@00, SimpleAnimationGraphManagerHolder@48,
			// BSTEventSink@60). Index 0x14 lives in that branch.
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_ActorMagicCaster[0] };

			SetCastingTimerForChargeHook::func =
				vtbl.write_vfunc(SetCastingTimerForChargeHook::idx, SetCastingTimerForChargeHook::thunk);

			spdlog::info("CastTime: hook installed on ActorMagicCaster vtable (SetCastingTimerForCharge @0x14).");
		} catch (const std::exception& e) {
			spdlog::error("CastTime: failed to install: {} - cast time will not be modified.", e.what());
		} catch (...) {
			spdlog::error("CastTime: failed to install (unknown exception) - cast time will not be modified.");
		}
	}
}
