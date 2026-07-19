// MagicScaling.cpp
// Lodestone - Shared SKSE framework
//
// Phase L3, Part 1.5 - trace only. See MagicScaling.h for the questions this
// build exists to answer. It observes; it never writes a value back.
//
// REVISION 2. The first trace run answered two of its questions with a clear
// NO, which is a result, not a failure:
//
//   - ActiveEffect::AdjustForPerks fired about a dozen times, all of them
//     abilities being applied during load (armor fortifies, other mods'
//     controller effects). It did NOT fire for a cast, at either of the two
//     attribute values tested.
//   - SpellItem::AdjustCost fired ZERO times, including while scrolling the
//     spell list.
//
// So neither is on the cast path, and the module cannot be built on them. This
// revision follows the caster instead, on RE/M/MagicCaster.h:
//
//     virtual bool CheckCast(MagicItem*, bool dualCast, float* alchStrength,
//                            MagicSystem::CannotCastReason*, bool useBaseValueForCost);  // 0A
//     virtual void AdjustActiveEffect(ActiveEffect*, float power, bool);                 // 1C
//     float currentSpellCost;                                                            // 38
//
// That is the SAME vtable the cast time module has hooked and had validated in
// game (ActorMagicCaster, index 0x14), which makes it the least speculative
// surface available: the technique is proven on this exact vtable, only the
// slot changes.
//
// Two trace defects from revision 1 are also fixed here:
//   - The census printed each vtable's IMPLEMENTATION address while the event
//     lines printed the object's VTABLE address. The two are not comparable, so
//     an event could not be tied back to a class. The census now prints both,
//     and events print the class NAME.
//   - Events were logged for a player caster only. Measured volume turned out
//     to be roughly a dozen events per session, not the hundreds the cast time
//     hook sees, so the filter was hiding evidence for no benefit. Everything
//     is logged now, except on the one path that could plausibly flood.
//
// Phase L3 (Part 1.5, revision 2)

#include "MagicScaling.h"

#include <safetyhook.hpp>

#include <atomic>

namespace Lodestone::Core::MagicScaling
{
	namespace
	{
		// -------------------------------------------------------------------
		// Trace logging
		//
		// info level, not debug: Log.cpp drops to `info` when NDEBUG is set, so a
		// debug-level trace would be invisible in the Release build that actually
		// gets played. Same choice the L2 trace made. Production code goes back to
		// debug for per-event lines - a trace is temporary by definition.
		// -------------------------------------------------------------------
		bool ShouldLog()
		{
			auto* logger = spdlog::default_logger_raw();
			return logger && logger->should_log(spdlog::level::info);
		}

		// Address printed relative to the module base. An absolute address is
		// meaningless across runs (ASLR); an RVA can be compared to a disassembly
		// and between the two load orders this trace is run on.
		std::uintptr_t Rva(std::uintptr_t a_addr)
		{
			const auto base = REL::Module::get().base();
			return (a_addr && a_addr > base) ? (a_addr - base) : 0;
		}

		std::atomic<std::uint64_t> g_perkEvents{ 0 };
		std::atomic<std::uint64_t> g_adjustEffectEvents{ 0 };
		std::atomic<std::uint64_t> g_checkCastEvents{ 0 };
		std::atomic<std::uint64_t> g_costEvents{ 0 };

		const char* SafeName(RE::TESForm* a_form)
		{
			if (!a_form) {
				return "<null>";
			}
			const char* name = a_form->GetName();
			return (name && *name) ? name : "<unnamed>";
		}

		std::uintptr_t ReadVfunc(const REL::VariantID& a_vtable, std::size_t a_idx)
		{
			REL::Relocation<std::uintptr_t> vtbl{ a_vtable };
			return *reinterpret_cast<std::uintptr_t*>(vtbl.address() + (sizeof(void*) * a_idx));
		}

		// -------------------------------------------------------------------
		// The effect types we swap AdjustForPerks on
		//
		// Each row carries the class NAME, so an event can report which class it
		// came from instead of a bare address the reader has to correlate by hand.
		// `original` is the body that vtable's slot 0x00 pointed at before the
		// swap - several rows share one body, and one (DualValueModifierEffect)
		// has its own, which the per-vtable lookup handles without a special case.
		// -------------------------------------------------------------------
		struct EffectType
		{
			const char*    name;
			REL::VariantID vtable;
			std::uintptr_t address{ 0 };
			std::uintptr_t original{ 0 };
		};

		EffectType g_types[] = {
			{ "ActiveEffect", RE::VTABLE_ActiveEffect[0] },
			{ "ValueModifierEffect", RE::VTABLE_ValueModifierEffect[0] },
			{ "DualValueModifierEffect", RE::VTABLE_DualValueModifierEffect[0] },
			{ "PeakValueModifierEffect", RE::VTABLE_PeakValueModifierEffect[0] },
			{ "AccumulatingValueModifierEffect", RE::VTABLE_AccumulatingValueModifierEffect[0] },
			{ "TargetValueModifierEffect", RE::VTABLE_TargetValueModifierEffect[0] },
			{ "ValueAndConditionsEffect", RE::VTABLE_ValueAndConditionsEffect[0] },
			{ "ScriptEffect", RE::VTABLE_ScriptEffect[0] },
		};

		const EffectType* FindType(std::uintptr_t a_vtable)
		{
			for (const auto& t : g_types) {
				if (t.address == a_vtable) {
					return &t;
				}
			}
			return nullptr;
		}

		// An effect whose class is NOT one we swapped still shows up as the second
		// argument of AdjustActiveEffect. Naming it "<not hooked>" there is a real
		// signal: it means the AdjustForPerks list is missing the class that
		// actually carries a cast's magnitude.
		const char* TypeName(std::uintptr_t a_vtable)
		{
			const auto* t = FindType(a_vtable);
			return t ? t->name : "<not hooked>";
		}

		// Revision 2 printed the class name and nothing else, so the three
		// "<not hooked>" effects it caught could not be identified - the name is
		// only as good as the list it comes from. The RVA goes back in beside it:
		// an unknown class can then be matched against Offsets_VTABLE.h and added.
		std::uintptr_t g_lastCheckCastSpell{ 0 };
		float          g_lastCheckCastCost{ -1.0f };

		std::uintptr_t VtableOf(void* a_obj)
		{
			return a_obj ? *reinterpret_cast<std::uintptr_t*>(a_obj) : 0;
		}

		// -------------------------------------------------------------------
		// Noise filter
		//
		// The revision 3 run produced 1026 lines, and almost none of them were
		// about this module. Two shapes account for nearly all of it:
		//
		//   - Effects with magnitude 0 and duration 0. Other mods' controller and
		//     hotkey abilities, driven entirely from script. There is no number
		//     there to scale, so they can never be evidence either way.
		//   - Abilities sitting at the placeholder cost of 1.0. Every constant
		//     effect in the load order reports it, on every menu refresh.
		//
		// Dropping both is safe in a way that filtering by name or by mod would
		// not be: the test is a PROPERTY of the values this module exists to
		// scale, not a guess about which mods matter. An effect that starts
		// carrying a real magnitude starts being logged, whoever it belongs to.
		//
		// Counters still tally every call, so if the totals ever diverge from the
		// logged lines in a way that looks wrong, the filter is the first suspect
		// and the numbers are right there to say so.
		// -------------------------------------------------------------------
		bool IsNumericallyInteresting(float a_magnitude, float a_duration)
		{
			return a_magnitude != 0.0f || a_duration != 0.0f;
		}

		constexpr float kPlaceholderCost = 1.0f;

		bool IsPlayer(RE::Actor* a_actor)
		{
			return a_actor && a_actor->IsPlayerRef();
		}

		void RunCensus()
		{
			spdlog::info("MagicScalingTrace: === vtable census ===");

			for (auto& t : g_types) {
				REL::Relocation<std::uintptr_t> vtbl{ t.vtable };
				t.address  = vtbl.address();
				t.original = ReadVfunc(t.vtable, 0x00);

				spdlog::info("MagicScalingTrace:   {:<32} vtable=RVA 0x{:X}  AdjustForPerks=RVA 0x{:X}",
					t.name, Rva(t.address), Rva(t.original));
			}

			REL::Relocation<std::uintptr_t> caster{ RE::VTABLE_ActorMagicCaster[0] };
			spdlog::info("MagicScalingTrace:   {:<32} vtable=RVA 0x{:X}", "ActorMagicCaster", Rva(caster.address()));
			spdlog::info("MagicScalingTrace:     CheckCast (0x0A)           impl=RVA 0x{:X}",
				Rva(ReadVfunc(RE::VTABLE_ActorMagicCaster[0], 0x0A)));
			spdlog::info("MagicScalingTrace:     AdjustActiveEffect (0x1C)  impl=RVA 0x{:X}",
				Rva(ReadVfunc(RE::VTABLE_ActorMagicCaster[0], 0x1C)));
			spdlog::info("MagicScalingTrace:   {:<32} AdjustCost(0x63)=RVA 0x{:X}", "SpellItem",
				Rva(ReadVfunc(RE::VTABLE_SpellItem[0], 0x63)));

			spdlog::info("MagicScalingTrace: === end census ===");
		}

		// -------------------------------------------------------------------
		// ActiveEffect::AdjustForPerks - vfunc 0x00, vtable swap
		//
		// Kept from revision 1 even though it proved inert on the cast path: it is
		// cheap, and knowing WHEN it does fire is what rules it in or out for
		// good. Revision 1 could not do that, because it only logged player-caster
		// events and could not name the class.
		//
		// A vtable swap rather than a branch hook. Revision 1 branch-hooked the
		// shared body and crashed the game on save load: Trampoline::write_branch
		// does not relocate a prologue - it assumes the address already holds a
		// 5-byte rel32 branch, decodes that displacement, and returns the branch's
		// original target. Pointed at a function body it returns an address in no
		// loaded module, which the thunk then calls. The guards now in
		// BookFramework and SpellTomes exist for the same reason.
		// -------------------------------------------------------------------
		struct AdjustForPerksHook
		{
			static void thunk(RE::ActiveEffect* a_this, RE::Actor* a_caster, RE::MagicTarget* a_target)
			{
				float beforeMag = 0.0f;
				float beforeDur = 0.0f;
				if (a_this) {
					beforeMag = a_this->magnitude;
					beforeDur = a_this->duration;
				}

				// The object's vtable pointer names its type, which names the
				// original to call. Nothing safe is left if it is missing: calling
				// the wrong original would be worse than calling none.
				const auto  vtable = VtableOf(a_this);
				const auto* type   = FindType(vtable);
				if (!type || !type->original) {
					return;
				}

				reinterpret_cast<decltype(&thunk)>(type->original)(a_this, a_caster, a_target);

				try {
					g_perkEvents.fetch_add(1, std::memory_order_relaxed);

					if (!a_this || !ShouldLog() || !IsPlayer(a_caster) ||
						!IsNumericallyInteresting(a_this->magnitude, a_this->duration)) {
						return;
					}

					spdlog::info("MagicScalingTrace: [AdjustForPerks] {} (vtable=RVA 0x{:X}) | spell='{}' (0x{:08X}) effect='{}' | "
								 "caster='{}' player={} | mag {:.4f} -> {:.4f} | dur {:.4f} -> {:.4f} | changed={}",
						type->name,
						Rva(vtable),
						SafeName(a_this->spell),
						a_this->spell ? a_this->spell->GetFormID() : 0,
						SafeName(a_this->GetBaseObject()),
						a_caster ? SafeName(a_caster) : "<null>",
						a_caster ? (a_caster->IsPlayerRef() ? "YES" : "no") : "?",
						beforeMag, a_this->magnitude,
						beforeDur, a_this->duration,
						((beforeMag != a_this->magnitude) || (beforeDur != a_this->duration)) ? "YES" : "no");
				} catch (...) {
					// A trace is never worth a crash. The original already ran.
				}
			}

			static inline constexpr std::size_t idx{ 0x00 };
		};

		// -------------------------------------------------------------------
		// MagicCaster::AdjustActiveEffect - vfunc 0x1C, ActorMagicCaster vtable
		//
		// THE MAGNITUDE CANDIDATE. It takes the effect and a `power` float, and it
		// hangs off the caster - so unlike AdjustForPerks it is on the path a cast
		// actually walks, and it knows who is casting.
		//
		// What this has to show: that it fires on a cast, that the effect it
		// receives is the one carrying the spell's magnitude, and whether the
		// original changes that magnitude. If it does, this is where the module
		// scales - after the original, exactly as the cast time module scales
		// castingTimer after the original writes it.
		// -------------------------------------------------------------------
		struct AdjustActiveEffectHook
		{
			static void thunk(RE::MagicCaster* a_this, RE::ActiveEffect* a_effect, float a_power, bool a_arg3)
			{
				float beforeMag = 0.0f;
				float beforeDur = 0.0f;
				if (a_effect) {
					beforeMag = a_effect->magnitude;
					beforeDur = a_effect->duration;
				}

				func(a_this, a_effect, a_power, a_arg3);

				try {
					g_adjustEffectEvents.fetch_add(1, std::memory_order_relaxed);

					if (!a_effect || !ShouldLog() ||
						!IsNumericallyInteresting(a_effect->magnitude, a_effect->duration)) {
						return;
					}

					auto* actor = a_this ? a_this->GetCasterAsActor() : nullptr;
					if (!IsPlayer(actor)) {
						return;
					}

					spdlog::info("MagicScalingTrace: [AdjustActiveEffect] {} (vtable=RVA 0x{:X}) | spell='{}' (0x{:08X}) effect='{}' | "
								 "caster='{}' player={} | power={:.4f} arg3={} | mag {:.4f} -> {:.4f} | "
								 "dur {:.4f} -> {:.4f} | changed={}",
						TypeName(VtableOf(a_effect)),
						Rva(VtableOf(a_effect)),
						SafeName(a_effect->spell),
						a_effect->spell ? a_effect->spell->GetFormID() : 0,
						SafeName(a_effect->GetBaseObject()),
						actor ? SafeName(actor) : "<null>",
						actor ? (actor->IsPlayerRef() ? "YES" : "no") : "?",
						a_power, a_arg3,
						beforeMag, a_effect->magnitude,
						beforeDur, a_effect->duration,
						((beforeMag != a_effect->magnitude) || (beforeDur != a_effect->duration)) ? "YES" : "no");
				} catch (...) {
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x1C };
		};

		// -------------------------------------------------------------------
		// MagicCaster::CheckCast - vfunc 0x0A, ActorMagicCaster vtable
		//
		// THE COST CANDIDATE, since AdjustCost turned out never to run. CheckCast
		// is what decides whether a cast may proceed, cost included - note its own
		// `useBaseValueForCost` argument - and the caster carries currentSpellCost
		// at 0x38, which is read before and after the original here.
		//
		// PLAYER ONLY, unlike the other two. This is the one hook in this trace
		// that could plausibly run every frame for every NPC deciding whether to
		// cast, and a trace nobody can read is worse than no trace. The counter
		// still tallies every call, so if the totals diverge wildly from the
		// logged lines, that itself is the volume answer.
		// -------------------------------------------------------------------
		struct CheckCastHook
		{
			static bool thunk(RE::MagicCaster* a_this, RE::MagicItem* a_spell, bool a_dualCast,
				float* a_alchStrength, RE::MagicSystem::CannotCastReason* a_reason, bool a_useBaseValueForCost)
			{
				const float beforeCost = a_this ? a_this->currentSpellCost : 0.0f;

				const bool result = func(a_this, a_spell, a_dualCast, a_alchStrength, a_reason, a_useBaseValueForCost);

				try {
					g_checkCastEvents.fetch_add(1, std::memory_order_relaxed);

					if (!a_this || !ShouldLog()) {
						return result;
					}

					auto* actor = a_this->GetCasterAsActor();
					if (!actor || !actor->IsPlayerRef()) {
						return result;
					}

					// Revision 2 logged every call and buried the run: CheckCast
					// fires roughly four times a second for the whole time a spell
					// is readied, always with the same numbers. Only transitions
					// carry information, so a repeat of the same spell at the same
					// cost is dropped. The counter still tallies every call.
					// Placeholder-cost abilities carry no cost to scale.
					if (a_this->currentSpellCost == kPlaceholderCost) {
						return result;
					}

					const auto spellID = a_spell ? static_cast<std::uintptr_t>(a_spell->GetFormID()) : 0;
					if (spellID == g_lastCheckCastSpell && a_this->currentSpellCost == g_lastCheckCastCost) {
						return result;
					}
					g_lastCheckCastSpell = spellID;
					g_lastCheckCastCost  = a_this->currentSpellCost;

					spdlog::info("MagicScalingTrace: [CheckCast] spell='{}' (0x{:08X}) | dualCast={} useBaseValue={} | "
								 "currentSpellCost {:.4f} -> {:.4f} | changed={} | allowed={}",
						SafeName(a_spell),
						a_spell ? a_spell->GetFormID() : 0,
						a_dualCast, a_useBaseValueForCost,
						beforeCost, a_this->currentSpellCost,
						(beforeCost != a_this->currentSpellCost) ? "YES" : "no",
						result);
				} catch (...) {
				}

				return result;
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x0A };
		};

		// -------------------------------------------------------------------
		// Cost-write probes - MagicCaster vfuncs 0x04, 0x06, 0x09, 0x10
		//
		// currentSpellCost (0x38) is confirmed to hold the value the module needs:
		// it read 13.0255 at the low attribute value and 10.0140 at the high one,
		// which are the two numbers the existing perk-driven system produces for
		// the same spell. But CheckCast reported changed=no every single time - the
		// cost is already correct before CheckCast is entered, so something
		// UPSTREAM writes it, and that writer is where the module has to scale.
		// Scaling inside CheckCast would be both late and repeated, since it runs
		// about four times a second for as long as a spell is readied.
		//
		// These four probes exist only to find the writer. Each logs
		// currentSpellCost across the original; whichever one reports changed=YES
		// is the seam. They are the cheapest possible way to answer it - no
		// disassembly, no address IDs, all four addressable from the same vtable
		// the cast time module already swaps.
		//
		// 0x14 (SetCastingTimerForCharge) is deliberately NOT probed: the cast time
		// module already owns that slot, and two modules swapping one slot makes
		// install order matter for no gain.
		// -------------------------------------------------------------------
		void LogCost(const char* a_where, RE::MagicCaster* a_this, float a_before)
		{
			if (!a_this || !ShouldLog()) {
				return;
			}

			auto* actor = a_this->GetCasterAsActor();
			if (!IsPlayer(actor)) {
				return;
			}

			const float after = a_this->currentSpellCost;

			// Both sides at the placeholder means an ability passed through; there
			// was never a cost here to write.
			if (a_before == kPlaceholderCost && after == kPlaceholderCost) {
				return;
			}

			spdlog::info("MagicScalingTrace: [{}] spell='{}' | currentSpellCost {:.4f} -> {:.4f} | changed={}",
				a_where,
				SafeName(a_this->currentSpell),
				a_before, after,
				(a_before != after) ? "YES" : "no");
		}

		struct StartChargeImplHook
		{
			static bool thunk(RE::MagicCaster* a_this)
			{
				const float before = a_this ? a_this->currentSpellCost : 0.0f;
				const bool  result = func(a_this);
				try {
					LogCost("StartChargeImpl", a_this, before);
				} catch (...) {
				}
				return result;
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x04 };
		};

		struct StartCastImplHook
		{
			static void thunk(RE::MagicCaster* a_this)
			{
				const float before = a_this ? a_this->currentSpellCost : 0.0f;
				func(a_this);
				try {
					LogCost("StartCastImpl", a_this, before);
				} catch (...) {
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x06 };
		};

		struct SpellCastHook
		{
			static void thunk(RE::MagicCaster* a_this, bool a_doCast, std::uint32_t a_arg2, RE::MagicItem* a_spell)
			{
				const float before = a_this ? a_this->currentSpellCost : 0.0f;
				func(a_this, a_doCast, a_arg2, a_spell);
				try {
					LogCost("SpellCast", a_this, before);
				} catch (...) {
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x09 };
		};

		// The three slots left between SetCurrentSpellImpl (which still reports the
		// placeholder 1.0) and SpellCast (which already reports the final, perk
		// adjusted cost). This CLOSES the sweep of this vtable: if none of them
		// writes currentSpellCost, the writer is not a MagicCaster virtual at all,
		// and the next step is the perk entry point path rather than another slot.
		struct RequestCastImplHook
		{
			static void thunk(RE::MagicCaster* a_this)
			{
				const float before = a_this ? a_this->currentSpellCost : 0.0f;
				func(a_this);
				try {
					LogCost("RequestCastImpl", a_this, before);
				} catch (...) {
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x03 };
		};

		struct StartReadyImplHook
		{
			static void thunk(RE::MagicCaster* a_this)
			{
				const float before = a_this ? a_this->currentSpellCost : 0.0f;
				func(a_this);
				try {
					LogCost("StartReadyImpl", a_this, before);
				} catch (...) {
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x05 };
		};

		struct SelectSpellImplHook
		{
			static void thunk(RE::MagicCaster* a_this)
			{
				const float before = a_this ? a_this->currentSpellCost : 0.0f;
				func(a_this);
				try {
					LogCost("SelectSpellImpl", a_this, before);
				} catch (...) {
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x11 };
		};

		struct SetCurrentSpellImplHook
		{
			static void thunk(RE::MagicCaster* a_this, RE::MagicItem* a_spell)
			{
				const float before = a_this ? a_this->currentSpellCost : 0.0f;
				func(a_this, a_spell);
				try {
					LogCost("SetCurrentSpellImpl", a_this, before);
				} catch (...) {
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x10 };
		};

		// -------------------------------------------------------------------
		// ValueModifierEffect::ModifyActorValue - vfunc 0x20
		//
		// THE MAGNITUDE LEAD, and the first one in this trace that is not a guess
		// at a slot. Revision 3 established the class from the game itself: with a
		// real target, both spells tested came through as ValueModifierEffect -
		//
		//   [AdjustActiveEffect] ValueModifierEffect | effect='Flames'
		//                        | power=1.0000 | mag 8.0000 -> 8.0000 dur 1.1000
		//   [AdjustActiveEffect] ValueModifierEffect | effect='Restore Health'
		//                        | power=1.0000 | mag 10.0000 -> 10.0000
		//
		// 8.0 and 10.0 are the record values, unmodified - and at the attribute
		// value tested (zero) the consumer's multiplier is 1, so no scaling was
		// expected anywhere. That run could not answer the question; it only
		// proved the effect reaches these hooks at all.
		//
		// ModifyActorValue is where the effect finally applies a number TO the
		// actor, and that number arrives as an argument rather than as a field
		// read back afterwards. If the perk scaling exists anywhere downstream of
		// the record value, a_value is where it shows up: compare a_value against
		// the effect's own magnitude, and the gap between them IS the multiplier
		// the engine applied.
		//
		// Hooked on ValueModifierEffect only - the class the log named. The
		// related classes have their own vtables and can be added if the evidence
		// asks for it, rather than in advance.
		// -------------------------------------------------------------------
		struct ModifyActorValueHook
		{
			static void thunk(RE::ValueModifierEffect* a_this, RE::Actor* a_actor, float a_value, RE::ActorValue a_av)
			{
				func(a_this, a_actor, a_value, a_av);

				try {
					if (!a_this || !ShouldLog() || a_value == 0.0f) {
						return;
					}

					// The caster is what the consumer scales from, not the target:
					// a player-cast spell landing on a bandit is still the player's
					// magnitude.
					auto  casterPtr = a_this->GetCasterActor();
					auto* caster    = casterPtr.get();
					if (!IsPlayer(caster)) {
						return;
					}

					spdlog::info("MagicScalingTrace: [ModifyActorValue] spell='{}' effect='{}' | target='{}' | "
								 "av={} | value={:.4f} | effect magnitude={:.4f} | ratio={:.4f}",
						SafeName(a_this->spell),
						SafeName(a_this->GetBaseObject()),
						a_actor ? SafeName(a_actor) : "<null>",
						static_cast<std::uint32_t>(a_av),
						a_value,
						a_this->magnitude,
						(a_this->magnitude != 0.0f) ? (a_value / a_this->magnitude) : 0.0f);
				} catch (...) {
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x20 };
		};

		// -------------------------------------------------------------------
		// MagicItem::CalculateCost - RELOCATION_ID(11213, 11321)
		//
		// THE COST CANDIDATE, and the first inline hook in this project.
		//
		// Seven MagicCaster virtuals were probed for the write to currentSpellCost
		// and all seven reported changed=no - the value is already final when the
		// caster first sees it. So the writer is not a virtual on that vtable, and
		// probing further slots was abandoned rather than continued on hope.
		//
		// CalculateCost is where the number is produced. It is non-virtual, so
		// there is no slot to swap, and Trampoline::write_branch cannot detour a
		// function body (see the CMakeLists note). SafetyHook can: it relocates
		// the displaced prologue properly.
		//
		// The ID is one of the few cost-related addresses that ships in the real
		// 3.5.3 headers (RE/Offsets.h), not something taken from an outside source
		// - which matters, because an inline hook on a WRONG address succeeds
		// silently instead of failing loudly.
		//
		// The original runs first and its return value is the cost. If that value
		// tracks the consumer's attribute the way currentSpellCost did (13.0255 at
		// zero, 10.0140 at 85), this is the seam, and the module scales the return
		// value here - the same shape as scaling castingTimer after the original
		// wrote it.
		// -------------------------------------------------------------------
		SafetyHookInline g_calculateCostHook{};

		float CalculateCostThunk(RE::MagicItem* a_this, RE::Actor* a_caster)
		{
			const float cost = g_calculateCostHook.call<float, RE::MagicItem*, RE::Actor*>(a_this, a_caster);

			try {
				if (!a_this || !ShouldLog() || !IsPlayer(a_caster) || cost == kPlaceholderCost) {
					return cost;
				}

				spdlog::info("MagicScalingTrace: [CalculateCost] spell='{}' (0x{:08X}) | returned={:.4f}",
					SafeName(a_this), a_this->GetFormID(), cost);
			} catch (...) {
			}

			return cost;
		}

		// -------------------------------------------------------------------
		// SpellItem::AdjustCost - vfunc 0x63, SpellItem vtable
		//
		// Kept only to confirm the zero. Revision 1 logged not one call, which is
		// surprising enough for a function whose entire name is about adjusting
		// cost that it is worth a second run before being written off. If it stays
		// silent while CheckCast is busy, the answer is settled and this hook goes
		// away with the rest of the trace.
		// -------------------------------------------------------------------
		struct AdjustCostHook
		{
			static void thunk(RE::MagicItem* a_this, float& a_cost, RE::Actor* a_actor)
			{
				const float before = a_cost;

				func(a_this, a_cost, a_actor);

				try {
					g_costEvents.fetch_add(1, std::memory_order_relaxed);

					if (!a_this || !ShouldLog()) {
						return;
					}

					spdlog::info("MagicScalingTrace: [AdjustCost] spell='{}' (0x{:08X}) | caster='{}' player={} | "
								 "cost {:.4f} -> {:.4f} | changed={}",
						SafeName(a_this),
						a_this->GetFormID(),
						a_actor ? SafeName(a_actor) : "<null>",
						a_actor ? (a_actor->IsPlayerRef() ? "YES" : "no") : "?",
						before, a_cost,
						(before != a_cost) ? "YES" : "no");
				} catch (...) {
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x63 };
		};
	}

	void Install()
	{
		try {
			RunCensus();

			// Two passes. Every original is read by the census BEFORE any slot is
			// written, so no thunk can fire against a half-filled table - entry by
			// entry would leave a window where a swapped vtable is live but its
			// original is not recorded yet.
			for (const auto& t : g_types) {
				REL::Relocation<std::uintptr_t> vtbl{ t.vtable };
				vtbl.write_vfunc(AdjustForPerksHook::idx, AdjustForPerksHook::thunk);
			}

			spdlog::info("MagicScalingTrace: AdjustForPerks (0x00) swapped on {} effect vtables.",
				std::size(g_types));
		} catch (const std::exception& e) {
			spdlog::error("MagicScalingTrace: failed to install the AdjustForPerks hooks: {}", e.what());
		} catch (...) {
			spdlog::error("MagicScalingTrace: failed to install the AdjustForPerks hooks (unknown exception).");
		}

		try {
			// [0] is the MagicCaster branch of ActorMagicCaster's multiple
			// inheritance - the same branch the cast time module swaps 0x14 on.
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_ActorMagicCaster[0] };

			CheckCastHook::func          = vtbl.write_vfunc(CheckCastHook::idx, CheckCastHook::thunk);
			AdjustActiveEffectHook::func = vtbl.write_vfunc(AdjustActiveEffectHook::idx, AdjustActiveEffectHook::thunk);

			RequestCastImplHook::func     = vtbl.write_vfunc(RequestCastImplHook::idx, RequestCastImplHook::thunk);
			StartReadyImplHook::func      = vtbl.write_vfunc(StartReadyImplHook::idx, StartReadyImplHook::thunk);
			SelectSpellImplHook::func     = vtbl.write_vfunc(SelectSpellImplHook::idx, SelectSpellImplHook::thunk);
			StartChargeImplHook::func     = vtbl.write_vfunc(StartChargeImplHook::idx, StartChargeImplHook::thunk);
			StartCastImplHook::func       = vtbl.write_vfunc(StartCastImplHook::idx, StartCastImplHook::thunk);
			SpellCastHook::func           = vtbl.write_vfunc(SpellCastHook::idx, SpellCastHook::thunk);
			SetCurrentSpellImplHook::func = vtbl.write_vfunc(SetCurrentSpellImplHook::idx, SetCurrentSpellImplHook::thunk);

			spdlog::info("MagicScalingTrace: ActorMagicCaster swapped - CheckCast (0x0A), AdjustActiveEffect (0x1C), "
						 "and the cost-write probes (0x03, 0x04, 0x05, 0x06, 0x09, 0x10, 0x11).");
		} catch (const std::exception& e) {
			spdlog::error("MagicScalingTrace: failed to install the caster hooks: {}", e.what());
		} catch (...) {
			spdlog::error("MagicScalingTrace: failed to install the caster hooks (unknown exception).");
		}

		try {
			REL::Relocation<std::uintptr_t> target{ REL::RelocationID(11213, 11321) };

			g_calculateCostHook = safetyhook::create_inline(
				reinterpret_cast<void*>(target.address()),
				reinterpret_cast<void*>(&CalculateCostThunk));

			if (g_calculateCostHook) {
				spdlog::info("MagicScalingTrace: inline hook installed on MagicItem::CalculateCost (RVA 0x{:X}).",
					Rva(target.address()));
			} else {
				spdlog::error("MagicScalingTrace: SafetyHook refused to hook MagicItem::CalculateCost - "
							  "cost will not be traced.");
			}
		} catch (const std::exception& e) {
			spdlog::error("MagicScalingTrace: failed to install the CalculateCost hook: {}", e.what());
		} catch (...) {
			spdlog::error("MagicScalingTrace: failed to install the CalculateCost hook (unknown exception).");
		}

		try {
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_ValueModifierEffect[0] };

			ModifyActorValueHook::func = vtbl.write_vfunc(ModifyActorValueHook::idx, ModifyActorValueHook::thunk);

			spdlog::info("MagicScalingTrace: ValueModifierEffect swapped - ModifyActorValue (0x20).");
		} catch (const std::exception& e) {
			spdlog::error("MagicScalingTrace: failed to install the ModifyActorValue hook: {}", e.what());
		} catch (...) {
			spdlog::error("MagicScalingTrace: failed to install the ModifyActorValue hook (unknown exception).");
		}

		try {
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_SpellItem[0] };

			AdjustCostHook::func = vtbl.write_vfunc(AdjustCostHook::idx, AdjustCostHook::thunk);

			spdlog::info("MagicScalingTrace: SpellItem swapped - AdjustCost (0x63).");
		} catch (const std::exception& e) {
			spdlog::error("MagicScalingTrace: failed to install the AdjustCost hook: {}", e.what());
		} catch (...) {
			spdlog::error("MagicScalingTrace: failed to install the AdjustCost hook (unknown exception).");
		}

		spdlog::info("MagicScalingTrace: trace active - it logs only and changes nothing.");
	}
}
