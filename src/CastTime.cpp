// CastTime.cpp
// Intelligence Matters - SKSE plugin
//
// TRACE-ONLY implementation. See the warning banner in CastTime.h.
//
// EVIDENCE BEHIND THE HOOK TARGET (Part 1, read from CommonLibSSE-NG headers):
//
//   RE/M/MagicCaster.h
//     virtual void SetCastingTimerForCharge();   // 14   <-- hook target
//     virtual bool StartChargeImpl();            // 04   <-- concurrent path?
//     virtual Actor* GetCasterAsActor() const;   // 0C   <-- gives us the actor
//     MagicItem* currentSpell;                   // 28
//     float      castingTimer;                   // 34
//
//   RE/A/ActorMagicCaster.h overrides both 0x04 and 0x14, and holds Actor* actor.
//
//   RE/Offsets_VTABLE.h:1862
//     VTABLE_ActorMagicCaster{ VariantID(257613, 205828, 0x16aef00), ... }
//     [0] is the MagicCaster branch of the multiple inheritance
//     (MagicCaster@00, SimpleAnimationGraphManagerHolder@48, BSTEventSink@60).
//     Indices 0x04 and 0x14 both live in the MagicCaster portion, ahead of the
//     VR-only virtuals at 0x1D/0x1E - so the index is stable on SE / AE / VR.
//
// WHY NOT SpellItem::GetChargeTime (vfunc 0x64): it is const, lives on the
// spell, and takes no caster. There is no way to know who is casting, which
// kills the future NPC path. Rejected in Part 1.
//
// EXCEPTION SAFETY: a C++ exception escaping a hook into the engine is
// undefined behavior and can crash the game. Both thunks wrap their body.
// This mirrors the Stage B.3 convention for natives, applied to hooks: nothing
// this plugin installs is allowed to throw across an engine boundary.
//
// Phase 16 - Stage C.1 / Part 1.5

#include "CastTime.h"

namespace IMPlugin::CastTime
{
	namespace
	{
		// -------------------------------------------------------------------
		// Log helpers
		// -------------------------------------------------------------------

		// TESForm::GetName() is not documented to be non-null. Formatting a null
		// const char* is undefined behavior, and this code runs inside an engine
		// hook - not a place to find out. Cheap guard, no downside.
		const char* SafeName(const char* a_name)
		{
			return (a_name && *a_name) ? a_name : "<unnamed>";
		}

		const char* CastingTypeName(RE::MagicSystem::CastingType a_type)
		{
			switch (a_type) {
			case RE::MagicSystem::CastingType::kConstantEffect:
				return "ConstantEffect";
			case RE::MagicSystem::CastingType::kFireAndForget:
				return "FireAndForget";
			case RE::MagicSystem::CastingType::kConcentration:
				return "Concentration";
			case RE::MagicSystem::CastingType::kScroll:
				return "Scroll";
			default:
				return "Unknown";
			}
		}

		const char* CastingSourceName(RE::MagicSystem::CastingSource a_source)
		{
			switch (a_source) {
			case RE::MagicSystem::CastingSource::kLeftHand:
				return "LeftHand";
			case RE::MagicSystem::CastingSource::kRightHand:
				return "RightHand";
			case RE::MagicSystem::CastingSource::kOther:
				return "Other";
			case RE::MagicSystem::CastingSource::kInstant:
				return "Instant";
			// NOTE: no kNone case here on purpose. The enumerator exists on
			// CommonLibSSE-NG HEAD but NOT on 3.5.3, which is what the vcpkg
			// port pins. Naming it breaks the build (C2065). `default` covers
			// it, and the raw numeric value is logged alongside this name, so
			// nothing is lost either way.
			default:
				return "Unknown";
			}
		}

		// Dumps everything Part 2 needs to decide, in one line per event.
		//
		// a_tag        which hook fired (answers question 5 by ordering)
		// a_before     castingTimer as read BEFORE calling the original
		// a_after      castingTimer as read AFTER calling the original
		//
		// The before/after pair is the whole point: it is what proves (or
		// disproves) that the original leaves the vanilla charge time in
		// castingTimer, which is the assumption Part 2 is built on.
		void Trace(const char* a_tag, RE::MagicCaster* a_this, float a_before, float a_after)
		{
			// --- actor ---
			const char* actorName = "<null>";
			std::uint32_t actorID = 0;
			bool          isPlayer = false;

			if (auto* actor = a_this->GetCasterAsActor()) {
				actorName = SafeName(actor->GetName());
				actorID = actor->GetFormID();
				isPlayer = actor->IsPlayerRef();  // formID == 0x14
			}

			// --- spell ---
			const char* spellName = "<null>";
			std::uint32_t spellID = 0;
			const char*   castingType = "n/a";
			std::uint32_t castingTypeVal = 0xFFFFFFFF;  // raw enum value; name above is convenience only
			float         virtualCharge = -1.0f;  // MagicItem::GetChargeTime() (vfunc 0x64)
			float         spitCharge = -1.0f;     // SpellItem::Data::chargeTime (SPIT 0x0C, raw record)
			const char*   school = "n/a";

			if (auto* magicItem = a_this->currentSpell) {
				spellName = SafeName(magicItem->GetName());
				spellID = magicItem->GetFormID();
				castingTypeVal = static_cast<std::uint32_t>(magicItem->GetCastingType());
				castingType = CastingTypeName(magicItem->GetCastingType());
				virtualCharge = magicItem->GetChargeTime();

				// The raw SPIT field. Logged alongside the virtual getter on
				// purpose: if the two ever disagree, that difference IS the
				// answer to question 4 (are perks/dual cast already folded in).
				if (auto* spell = magicItem->As<RE::SpellItem>()) {
					spitCharge = spell->data.chargeTime;
				}

				// Not needed for C.1. Logged because the NPC path (C.5) will be
				// driven by the school skill, and this confirms for free that
				// the association is reachable from inside the hook.
				switch (magicItem->GetAssociatedSkill()) {
				case RE::ActorValue::kAlteration:
					school = "Alteration";
					break;
				case RE::ActorValue::kConjuration:
					school = "Conjuration";
					break;
				case RE::ActorValue::kDestruction:
					school = "Destruction";
					break;
				case RE::ActorValue::kIllusion:
					school = "Illusion";
					break;
				case RE::ActorValue::kRestoration:
					school = "Restoration";
					break;
				default:
					school = "None";
					break;
				}
			}

			const auto source = a_this->GetCastingSource();

			spdlog::info(
				"[{}] actor='{}' (0x{:08X}, player={}) | spell='{}' (0x{:08X}) | "
				"type={}({}) school={} source={}({}) dual={} | "
				"timer: before={:.4f} after={:.4f} | GetChargeTime()={:.4f} SPIT.chargeTime={:.4f}",
				a_tag,
				actorName, actorID, isPlayer,
				spellName, spellID,
				castingType, castingTypeVal, school,
				CastingSourceName(source), static_cast<std::uint32_t>(source),
				a_this->GetIsDualCasting(),
				a_before, a_after,
				virtualCharge, spitCharge);
		}

		// -------------------------------------------------------------------
		// Hook: MagicCaster::SetCastingTimerForCharge - vfunc 0x14
		//
		// The primary candidate. Calls the original first, then reports what it
		// left behind in castingTimer.
		// -------------------------------------------------------------------
		struct SetCastingTimerForChargeHook
		{
			static void thunk(RE::MagicCaster* a_this)
			{
				const float before = a_this ? a_this->castingTimer : -1.0f;

				// Original runs FIRST and unconditionally. If anything below
				// this line breaks, vanilla behavior is still intact.
				func(a_this);

				if (!a_this) {
					return;
				}

				try {
					Trace("SetCastingTimerForCharge", a_this, before, a_this->castingTimer);
				} catch (...) {
					// Swallow. A trace must never be able to crash the game.
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x14 };
		};

		// -------------------------------------------------------------------
		// Hook: MagicCaster::StartChargeImpl - vfunc 0x04
		//
		// Answers question 5. If this fires and 0x14 does not, or if it changes
		// castingTimer on its own, then 0x14 is the wrong target and Part 2
		// has to move. Logged for ORDERING as much as for values - the
		// timestamps tell us who runs first.
		// -------------------------------------------------------------------
		struct StartChargeImplHook
		{
			static bool thunk(RE::MagicCaster* a_this)
			{
				const float before = a_this ? a_this->castingTimer : -1.0f;

				const bool result = func(a_this);

				if (!a_this) {
					return result;
				}

				try {
					Trace("StartChargeImpl", a_this, before, a_this->castingTimer);
				} catch (...) {
				}

				return result;
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x04 };
		};
	}

	void Install()
	{
		try {
			// [0] = the MagicCaster branch of ActorMagicCaster's multiple
			// inheritance. Both indices below live in that branch.
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_ActorMagicCaster[0] };

			SetCastingTimerForChargeHook::func =
				vtbl.write_vfunc(SetCastingTimerForChargeHook::idx, SetCastingTimerForChargeHook::thunk);

			StartChargeImplHook::func =
				vtbl.write_vfunc(StartChargeImplHook::idx, StartChargeImplHook::thunk);

			spdlog::info("CastTime: TRACE hooks installed on ActorMagicCaster vtable "
						 "(SetCastingTimerForCharge @0x14, StartChargeImpl @0x04).");
			spdlog::info("CastTime: this build ONLY logs - cast time is untouched. external plugin still drives it.");
		} catch (const std::exception& e) {
			spdlog::error("CastTime: failed to install trace hooks: {}", e.what());
		} catch (...) {
			spdlog::error("CastTime: failed to install trace hooks (unknown exception).");
		}
	}
}
