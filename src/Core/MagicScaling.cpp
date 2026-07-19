// MagicScaling.cpp
// Lodestone - Shared SKSE framework
//
// Production implementation of the magic scaling capability. See MagicScaling.h
// for the contract and the design decisions.
//
// This SUPERSEDES the Part 1.5 trace, which hooked eight effect
// vtables plus nine functions on the caster and wrote nothing. Its job was to
// find where the engine applies each quantity, and every rule below comes from a
// specific measurement it produced rather than from reading the headers. What
// the trace measured, comparing a low attribute value against a high one in
// game:
//
//   [AdjustForPerks] 'Flames'          mag  8.0000 -> 11.0872  dur 1.1 -> 1.5245
//   [AdjustForPerks] 'Restore Health'  mag 10.0000 -> 13.8590  dur 1.0 -> 1.3859
//   [AdjustForPerks] 'Restore Stamina' mag 10.0000 -> 13.8590  dur 1.0 -> 1.3859
//   [CalculateCost]  'Flames'          13.0255 -> 10.0140
//   [CalculateCost]  'Healing'         11.0995 ->  8.5333
//
// Two facts drove the design. The magnitude factor (1.3859) and the cost factor
// (0.7688) are not related to each other, so the three channels have to be
// independently registerable. And the same 1.3859 landed on two different spells
// and three different effects, which is what makes a single multiplier per
// channel the right shape rather than something per-spell.
//
// REJECTED TARGETS, all ruled out by measurement rather than by argument:
//   SpellItem::AdjustCost (vfunc 0x63) - never runs on the player's cast path.
//     It fired for exactly one modded spell across every session.
//   Seven MagicCaster virtuals (0x03, 0x04, 0x05, 0x06, 0x09, 0x10, 0x11) - all
//     report currentSpellCost already final. The caster never computes it.
//   BGSEntryPoint::HandleEntryPoint - the address is a function ID, so it needs
//     an inline hook, and its signature is variadic. CalculateCost gives the
//     same number with a signature that is written down in the headers.
//
// AdjustForPerks was itself wrongly dismissed on the first pass, and the reason
// is worth keeping: the runs that missed it cast at nothing (a concentration
// spell creates no effect without a target) and cast at an attribute value whose
// multiplier was 1. Neither run could have shown a change. The hook was right
// from the start; the test was not exercising it.
//
// EXCEPTION SAFETY: a C++ exception escaping a hook into the engine is undefined
// behavior. Every thunk wraps its body and calls the original exactly once.
//
// Phase L3 (Part 2)

#include "MagicScaling.h"

#include <safetyhook.hpp>

#include <atomic>
#include <mutex>

namespace Lodestone::Core::MagicScaling
{
	namespace
	{
		// -------------------------------------------------------------------
		// A channel - one registered pair of globals
		//
		// THREADING mirrors the cast time module: the hooks run on the game
		// thread, registration runs on a Papyrus VM thread, so the pointers are
		// atomic and the hot path reads them lock-free. On x64 an acquire load of
		// an aligned pointer is a plain mov. The mutex serialises registrants
		// against each other only - no hook ever touches it.
		// -------------------------------------------------------------------
		struct Channel
		{
			std::atomic<RE::TESGlobal*> multiplier{ nullptr };
			std::atomic<RE::TESGlobal*> offset{ nullptr };

			// Reads the pair into locals so a formula sees a consistent snapshot
			// even if a registration lands mid-hook. Worst case in that instant is
			// one extra passthrough, which is not a state anyone can observe.
			bool Read(float& a_mult, float& a_offset) const
			{
				RE::TESGlobal* m = multiplier.load(std::memory_order_acquire);
				RE::TESGlobal* o = offset.load(std::memory_order_acquire);
				if (!m || !o) {
					return false;
				}
				a_mult   = m->value;
				a_offset = o->value;
				return true;
			}
		};

		Channel    g_magnitude;
		Channel    g_duration;
		Channel    g_cost;
		std::mutex g_registerLock;

		// value = (value * multiplier) + offset, clamped at zero.
		//
		// The globals are usually MCM-tunable, so a negative result is reachable
		// from a user setting rather than only from a bug. No floor above zero: a
		// consumer scaling a cost to nothing is a legitimate outcome, and
		// inventing a minimum here would be this module deciding balance.
		float Apply(float a_value, float a_mult, float a_offset)
		{
			const float result = (a_value * a_mult) + a_offset;
			return (result < 0.0f) ? 0.0f : result;
		}

		bool ShouldLogEvents()
		{
			auto* logger = spdlog::default_logger_raw();
			return logger && logger->should_log(spdlog::level::debug);
		}

		// Ordinary castable spells only - see the SCOPE note in the header. The
		// trace caught armour enchantments and quest abilities coming through the
		// same hook; a consumer asking for spell magnitude does not mean those.
		bool IsOrdinarySpell(RE::MagicItem* a_item)
		{
			return a_item && a_item->GetSpellType() == RE::MagicSystem::SpellType::kSpell;
		}

		// -------------------------------------------------------------------
		// Registration, shared by all three natives
		//
		// SINGLE CHANNEL - v1, first-registrant-wins. Same pair re-registered is
		// an idempotent refresh; a different second registrant is warned and
		// rejected, and the held channel is left untouched.
		//
		// Offset is published first and multiplier last (release), so a hook that
		// observes a set multiplier also observes the offset.
		// -------------------------------------------------------------------
		bool RegisterChannel(Channel& a_channel, const char* a_name,
			RE::TESGlobal* a_multiplier, RE::TESGlobal* a_offset)
		{
			if (!a_multiplier || !a_offset) {
				spdlog::warn("MagicScaling: Register{}Channel got a null global (multiplier={}, offset={}) - ignored.",
					a_name,
					a_multiplier ? "ok" : "NULL",
					a_offset ? "ok" : "NULL");
				return false;
			}

			std::lock_guard<std::mutex> lock(g_registerLock);

			RE::TESGlobal* curMult   = a_channel.multiplier.load(std::memory_order_acquire);
			RE::TESGlobal* curOffset = a_channel.offset.load(std::memory_order_acquire);

			if (curMult && curOffset) {
				if (curMult == a_multiplier && curOffset == a_offset) {
					spdlog::debug("MagicScaling: {} channel re-registered with the same globals - no-op.", a_name);
					return true;
				}

				spdlog::warn("MagicScaling: a second, DIFFERENT {} channel tried to register "
							 "(new multiplier=0x{:08X}, offset=0x{:08X}); the first channel "
							 "(multiplier=0x{:08X}, offset=0x{:08X}) keeps ownership. Ignoring the new one.",
					a_name,
					a_multiplier->GetFormID(), a_offset->GetFormID(),
					curMult->GetFormID(), curOffset->GetFormID());
				return false;
			}

			a_channel.offset.store(a_offset, std::memory_order_release);
			a_channel.multiplier.store(a_multiplier, std::memory_order_release);

			spdlog::info("MagicScaling: {} channel registered (multiplier=0x{:08X}, offset=0x{:08X}) - ACTIVE.",
				a_name, a_multiplier->GetFormID(), a_offset->GetFormID());
			return true;
		}

		// -------------------------------------------------------------------
		// Originals for AdjustForPerks, keyed by vtable address
		//
		// Several effect vtables are swapped and most share one implementation,
		// but one carries its own override, so the thunk resolves the original
		// from the object's own vtable pointer rather than assuming. Filled in two
		// passes by Install(): every original is read before the first slot is
		// written, so no thunk can fire against a half-filled table.
		// -------------------------------------------------------------------
		struct EffectType
		{
			REL::VariantID vtable;
			std::uintptr_t address{ 0 };
			std::uintptr_t original{ 0 };
		};

		EffectType g_effectTypes[] = {
			{ RE::VTABLE_ActiveEffect[0] },
			{ RE::VTABLE_ValueModifierEffect[0] },
			{ RE::VTABLE_DualValueModifierEffect[0] },
			{ RE::VTABLE_PeakValueModifierEffect[0] },
			{ RE::VTABLE_AccumulatingValueModifierEffect[0] },
			{ RE::VTABLE_TargetValueModifierEffect[0] },
			{ RE::VTABLE_ValueAndConditionsEffect[0] },
			{ RE::VTABLE_ScriptEffect[0] },
		};

		std::uintptr_t FindOriginal(std::uintptr_t a_vtable)
		{
			for (const auto& t : g_effectTypes) {
				if (t.address == a_vtable) {
					return t.original;
				}
			}
			return 0;
		}

		// -------------------------------------------------------------------
		// Hook: ActiveEffect::AdjustForPerks - vfunc 0x00
		//
		// Carries both magnitude and duration. The original runs first and is what
		// applies the engine's own perk work - the trace measured magnitude moving
		// 8.0 -> 11.0872 across this call - so the values read afterwards are the
		// finished ones, and scaling them is additive to the engine rather than a
		// fight with it.
		//
		// Filters are ordered cheapest first. This fires for every effect applied
		// to every actor, and the trace showed the overwhelming majority are other
		// mods' controller abilities carrying no numbers at all.
		// -------------------------------------------------------------------
		void AdjustForPerksThunk(RE::ActiveEffect* a_this, RE::Actor* a_caster, RE::MagicTarget* a_target)
		{
			const auto vtable   = a_this ? *reinterpret_cast<std::uintptr_t*>(a_this) : 0;
			const auto original = FindOriginal(vtable);
			if (!original) {
				// Nothing safe is left: calling the wrong original would be worse
				// than calling none.
				return;
			}

			reinterpret_cast<decltype(&AdjustForPerksThunk)>(original)(a_this, a_caster, a_target);

			try {
				if (!a_this) {
					return;
				}

				// 1. Either channel registered? Cuts 100% when no consumer is
				//    present, for the cost of four pointer compares.
				float magMult = 0.0f, magOffset = 0.0f;
				float durMult = 0.0f, durOffset = 0.0f;
				const bool haveMag = g_magnitude.Read(magMult, magOffset);
				const bool haveDur = g_duration.Read(durMult, durOffset);
				if (!haveMag && !haveDur) {
					return;
				}

				// 2. Player only - by decision. GetCasterActor() would serve NPCs
				//    just as well when a channel for them exists.
				if (!a_caster || !a_caster->IsPlayerRef()) {
					return;
				}

				// 3. Ordinary spells only. This is what keeps armour enchantments
				//    and quest abilities out of a consumer's spell scaling.
				if (!IsOrdinarySpell(a_this->spell)) {
					return;
				}

				const float beforeMag = a_this->magnitude;
				const float beforeDur = a_this->duration;

				// 4. Scale only what exists. An effect with no magnitude keeps
				//    none - applying an offset to zero would invent magnitude on
				//    effects that never had any.
				if (haveMag && beforeMag != 0.0f) {
					a_this->magnitude = Apply(beforeMag, magMult, magOffset);
				}

				if (haveDur && beforeDur != 0.0f) {
					a_this->duration = Apply(beforeDur, durMult, durOffset);
				}

				if (ShouldLogEvents() &&
					(a_this->magnitude != beforeMag || a_this->duration != beforeDur)) {
					spdlog::debug("MagicScaling: '{}' (0x{:08X}) mag {:.4f} -> {:.4f}, dur {:.4f} -> {:.4f}",
						a_this->spell && a_this->spell->GetName() ? a_this->spell->GetName() : "<unnamed>",
						a_this->spell ? a_this->spell->GetFormID() : 0,
						beforeMag, a_this->magnitude,
						beforeDur, a_this->duration);
				}
			} catch (...) {
				// Swallow. Scaled magic is a gameplay nicety; taking the game down
				// over it is not a trade this plugin makes. The original already
				// ran, so vanilla behavior is intact.
			}
		}

		// -------------------------------------------------------------------
		// Hook: MagicItem::CalculateCost - RELOCATION_ID(11213, 11321)
		//
		// Non-virtual, so this is an inline hook rather than a vtable swap (see
		// the mechanism rule in CMakeLists). The original returns the finished
		// cost - the trace read 13.0255 against 10.0140 for the same spell at two
		// attribute values, which is what identified this function as the seam
		// after seven caster virtuals all reported the value already final.
		// -------------------------------------------------------------------
		SafetyHookInline g_calculateCostHook{};

		float CalculateCostThunk(RE::MagicItem* a_this, RE::Actor* a_caster)
		{
			const float cost = g_calculateCostHook.call<float, RE::MagicItem*, RE::Actor*>(a_this, a_caster);

			try {
				float mult = 0.0f, offset = 0.0f;
				if (!g_cost.Read(mult, offset)) {
					return cost;
				}

				if (!a_caster || !a_caster->IsPlayerRef()) {
					return cost;
				}

				if (!IsOrdinarySpell(a_this)) {
					return cost;
				}

				// A spell that costs nothing keeps costing nothing, for the same
				// reason a zero magnitude is left alone.
				if (cost == 0.0f) {
					return cost;
				}

				const float scaled = Apply(cost, mult, offset);

				if (ShouldLogEvents()) {
					spdlog::debug("MagicScaling: '{}' (0x{:08X}) cost {:.4f} -> {:.4f}",
						a_this && a_this->GetName() ? a_this->GetName() : "<unnamed>",
						a_this ? a_this->GetFormID() : 0,
						cost, scaled);
				}

				return scaled;
			} catch (...) {
				// Fall back to the engine's own number.
			}

			return cost;
		}

		// -------------------------------------------------------------------
		// Natives
		//
		// Error convention (Stage B.3): failure by return value, never by
		// throwing. Returns true when the CALLER's pair is the active one after
		// the call; false on a null argument or a rejected second registrant.
		// -------------------------------------------------------------------
		bool RegisterMagicMagnitudeChannel(RE::StaticFunctionTag*, RE::TESGlobal* a_multiplier, RE::TESGlobal* a_offset)
		{
			return RegisterChannel(g_magnitude, "magnitude", a_multiplier, a_offset);
		}

		bool RegisterMagicDurationChannel(RE::StaticFunctionTag*, RE::TESGlobal* a_multiplier, RE::TESGlobal* a_offset)
		{
			return RegisterChannel(g_duration, "duration", a_multiplier, a_offset);
		}

		bool RegisterMagicCostChannel(RE::StaticFunctionTag*, RE::TESGlobal* a_multiplier, RE::TESGlobal* a_offset)
		{
			return RegisterChannel(g_cost, "cost", a_multiplier, a_offset);
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("MagicScaling: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("RegisterMagicMagnitudeChannel", "Lodestone", RegisterMagicMagnitudeChannel);
		a_vm->RegisterFunction("RegisterMagicDurationChannel", "Lodestone", RegisterMagicDurationChannel);
		a_vm->RegisterFunction("RegisterMagicCostChannel", "Lodestone", RegisterMagicCostChannel);

		spdlog::info("MagicScaling: natives registered (RegisterMagicMagnitudeChannel, "
					 "RegisterMagicDurationChannel, RegisterMagicCostChannel).");
		return true;
	}

	void Install()
	{
		try {
			// PASS 1 - read every original before anything is swapped.
			for (auto& t : g_effectTypes) {
				REL::Relocation<std::uintptr_t> vtbl{ t.vtable };
				t.address  = vtbl.address();
				t.original = *reinterpret_cast<std::uintptr_t*>(t.address);
			}

			// PASS 2 - swap. From the first write on, thunks may fire, and every
			// one of them finds its original.
			for (const auto& t : g_effectTypes) {
				REL::Relocation<std::uintptr_t> vtbl{ t.vtable };
				vtbl.write_vfunc(0x00, AdjustForPerksThunk);
			}

			spdlog::info("MagicScaling: magnitude and duration hooks installed on {} effect vtables "
						 "(AdjustForPerks @0x00). No channel registered yet - passthrough until a consumer "
						 "calls Lodestone.RegisterMagicMagnitudeChannel or RegisterMagicDurationChannel.",
				std::size(g_effectTypes));
		} catch (const std::exception& e) {
			spdlog::error("MagicScaling: failed to install the magnitude hooks: {} - "
						  "magnitude and duration will not be scaled.", e.what());
		} catch (...) {
			spdlog::error("MagicScaling: failed to install the magnitude hooks (unknown exception) - "
						  "magnitude and duration will not be scaled.");
		}

		try {
			REL::Relocation<std::uintptr_t> target{ REL::RelocationID(11213, 11321) };

			g_calculateCostHook = safetyhook::create_inline(
				reinterpret_cast<void*>(target.address()),
				reinterpret_cast<void*>(&CalculateCostThunk));

			if (g_calculateCostHook) {
				spdlog::info("MagicScaling: cost hook installed on MagicItem::CalculateCost. "
							 "No channel registered yet - passthrough until a consumer calls "
							 "Lodestone.RegisterMagicCostChannel.");
			} else {
				spdlog::error("MagicScaling: could not hook MagicItem::CalculateCost - cost will not be scaled.");
			}
		} catch (const std::exception& e) {
			spdlog::error("MagicScaling: failed to install the cost hook: {} - cost will not be scaled.", e.what());
		} catch (...) {
			spdlog::error("MagicScaling: failed to install the cost hook (unknown exception) - "
						  "cost will not be scaled.");
		}
	}
}
