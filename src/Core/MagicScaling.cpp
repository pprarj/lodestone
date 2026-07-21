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
// CONFIRMED END TO END on 2026-07-20, with a live consumer driving all three
// channels and no third-party attribute plugin anywhere in the path. Firebolt,
// player attribute 0 against 85, read off the effect application site rather
// than off a tooltip:
//
//   magnitude  25.0000 -> 34.6475   (1.3859, exact)
//   Flames sustained damage, per-tick mean over 179 and 137 ticks:
//              8.012/s -> 11.096/s  (1.3849 measured against 1.3859 expected)
//   cost       36.4947 -> 28.0571   (0.7688, exact)
//
// The Part 1.5 magnitude figures above are therefore sound, not an artifact of
// the attribute plugin that was active when they were taken - a suspicion that
// was raised and is now settled by measurement.
//
// HOW THIS WAS ALMOST MISSED, because the same trap is still there. Magnitude
// was reported broken on the strength of floating combat damage numbers. Flames
// applies about 0.136 per 17ms tick, and 0.189 once scaled; both render as "1".
// A health-delta test did not settle it either, because timing a cast by hand
// carried an error the same size as the effect. What settled it was summing the
// per-tick values out of this module's own log, which is timestamped. When a
// test shows no difference, check first that it could have shown one.
//
// WHAT THIS MODULE DOES NOT DO: the spell menu. Magnitude and duration in the
// tooltip do not move, and that is a consequence of the seam, NOT a vanilla UI
// limitation - measured 2026-07-20 with a vanilla perk:
//
//   no perk               description "25 points"
//   Augmented Flames on   description "31 points"   (25 * 1.25)
//
// So the description does consult perk entry points. AdjustForPerks runs when an
// active effect is instantiated on a target, which the menu never does, so
// nothing written there can reach it. Cost shows in the menu only because
// CalculateCost happens to be a function the menu calls; there is no
// CalculateMagnitude to match it, and that asymmetry is the entire reason two
// channels display and one does not.
//
// The seam that would fix it is Actor::ForEachPerkEntry (vfunc 0x100), where the
// engine asks an actor which perk entries answer for an entry point. Traced
// 2026-07-20: opening the spell menu queries kModSpellCost, kModSpellMagnitude
// and kModSpellDuration there, interleaved with this module's own CalculateCost
// lines, and re-queries on every menu open. A plugin contributes an entry the
// actor does not hold as a real perk by hooking it - the technique Perk Entry
// Point Extender uses in production. Not implemented here yet; it is a behavior
// change, because scaling would then compose with real perks instead of
// applying after them.
//
// REJECTED TARGETS, all ruled out by measurement rather than by argument:
//   SpellItem::AdjustCost (vfunc 0x63) - never runs on the player's cast path.
//     It fired for exactly one modded spell across every session.
//   Seven MagicCaster virtuals (0x03, 0x04, 0x05, 0x06, 0x09, 0x10, 0x11) - all
//     report currentSpellCost already final. The caster never computes it.
//   BGSEntryPoint::HandleEntryPoint - the address is a function ID, so it needs
//     an inline hook, and its signature is variadic. CalculateCost gives the
//     same number with a signature that is written down in the headers.
//     NOTE: this rejection is about COST only. It does not carry over to the
//     tooltip problem described below, where the answer turned out to be
//     Actor::ForEachPerkEntry - a plain virtual, so a vtable swap, with no
//     variadic argument to forward.
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

		std::uintptr_t FindOriginalIn(const EffectType* a_table, std::size_t a_count, std::uintptr_t a_vtable)
		{
			for (std::size_t i = 0; i < a_count; ++i) {
				if (a_table[i].address == a_vtable) {
					return a_table[i].original;
				}
			}
			return 0;
		}

		std::uintptr_t FindOriginal(std::uintptr_t a_vtable)
		{
			return FindOriginalIn(g_effectTypes, std::size(g_effectTypes), a_vtable);
		}

		// Reads every original before swapping any slot, for the same reason
		// Install() does it in two passes: from the first write on a thunk may
		// fire, and every one of them has to find its original.
		void InstallVfunc(EffectType* a_table, std::size_t a_count, std::size_t a_index, void* a_thunk)
		{
			for (std::size_t i = 0; i < a_count; ++i) {
				REL::Relocation<std::uintptr_t> vtbl{ a_table[i].vtable };
				a_table[i].address  = vtbl.address();
				a_table[i].original = *reinterpret_cast<std::uintptr_t*>(a_table[i].address + (a_index * sizeof(std::uintptr_t)));
			}

			for (std::size_t i = 0; i < a_count; ++i) {
				REL::Relocation<std::uintptr_t> vtbl{ a_table[i].vtable };
				vtbl.write_vfunc(a_index, reinterpret_cast<std::uintptr_t>(a_thunk));
			}
		}

		// -------------------------------------------------------------------
		// Hook: ActiveEffect::AdjustForPerks - vfunc 0x00
		//
		// DURATION ONLY as of stage E3. Magnitude used to be applied here too,
		// and moved to the perk entry seam because this one is invisible to the
		// spell menu: an active effect is instantiated on a target, which the
		// menu never does. The original runs first and is what applies the
		// engine's own perk work, so the value read afterwards is the finished
		// one, and scaling it is additive to the engine rather than a fight
		// with it.
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

				// STAGE E3 - MAGNITUDE IS NO LONGER APPLIED HERE.
				//
				// It is contributed as a perk entry at kModSpellMagnitude
				// instead, which is the only seam the spell menu reads. Writing
				// it in both places applied the multiplier twice - measured as
				// 1.3859 squared on damage while E2 had both live.
				//
				// Duration still applies here, because its channel has not moved
				// yet. The menu's duration column is wrong for exactly the reason
				// magnitude's was, and moving it is the next step rather than
				// this one: one variable at a time is what this module learned
				// the expensive way.
				float      durMult = 0.0f, durOffset = 0.0f;
				const bool haveDur = g_duration.Read(durMult, durOffset);
				if (!haveDur) {
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

				const float observedMag = a_this->magnitude;
				const float beforeDur   = a_this->duration;

				// 4. Scale only what exists. An effect with no duration keeps
				//    none - applying an offset to zero would invent duration on
				//    effects that never had any.
				if (beforeDur != 0.0f) {
					a_this->duration = Apply(beforeDur, durMult, durOffset);
				}

				// Logged unconditionally once past the filters, and the channel
				// state is logged with it. This used to print only when a value
				// actually moved, which reads as the tidier choice and is not: a
				// trace that is silent when nothing changed cannot tell "the
				// thunk ran and wrote nothing" apart from "the thunk never ran
				// for this effect". Those two have completely different causes,
				// and a whole diagnostic was spent distinguishing them from
				// evidence this line could have carried in the first place.
				// beforeMag and the mult/offset pair are here for the same
				// reason - a reader should not need a second session to find out
				// whether Read succeeded.
				if (ShouldLogEvents()) {
					// The magnitude is logged as OBSERVED, not as changed - this
					// hook no longer touches it. Reading it here is still worth
					// the line, because it is what the perk entry seam produced,
					// so the two stages can be checked against each other from
					// one log.
					spdlog::debug("MagicScaling: '{}' (0x{:08X}) mag {:.4f} (observed; applied at the perk entry seam), "
								  "dur {:.4f} -> {:.4f} [durMult={:.4f} durOffset={:.4f}]",
						a_this->spell && a_this->spell->GetName() ? a_this->spell->GetName() : "<unnamed>",
						a_this->spell ? a_this->spell->GetFormID() : 0,
						observedMag,
						beforeDur, a_this->duration,
						durMult, durOffset);
				}
			} catch (...) {
				// Swallow. Scaled magic is a gameplay nicety; taking the game down
				// over it is not a trade this plugin makes. The original already
				// ran, so vanilla behavior is intact.
			}
		}

		// -------------------------------------------------------------------
		// STAGE E1 - the spell menu seam. EXPERIMENT, fixed multiplier.
		//
		// WHY THIS EXISTS. AdjustForPerks scales the effect that lands on a
		// target, which is why damage moves and the menu never does: the menu
		// instantiates no active effect. Cost displays only because
		// CalculateCost happens to be a function the menu calls, and there is no
		// CalculateMagnitude to match it. See the header block.
		//
		// Actor::ForEachPerkEntry (vfunc 0x100) is where the engine asks an actor
		// which perk entries answer for an entry point. Traced 2026-07-20:
		// opening the spell menu queries kModSpellMagnitude and kModSpellDuration
		// there, interleaved with this module's own CalculateCost lines, and
		// re-queries on every open. Contributing an entry the actor does not hold
		// as a real perk is what makes the number move in both places at once.
		//
		// E1 PROVES ONE THING AND CHANGES NOTHING ELSE. The multiplier below is a
		// constant, not the channel, and AdjustForPerks still does the real
		// scaling exactly as before. If the tooltip shows the constant AND the
		// damage follows it, construction and composition are sound and E2 can
		// wire the channel in. If it does not, nothing that works today broke.
		//
		// THE ENTRY IS BUILT, NOT SUBCLASSED. The storage is ours and the vtable
		// pointers are the engine's, so the engine's own implementations run
		// against our fields. Deriving in C++ would mean emitting a vtable for
		// every virtual in BGSPerkEntry, and any one of them getting subtly
		// different behavior from the engine's is a difference that would not
		// show up until it mattered. Layouts are declared in the headers, so this
		// is construction rather than guesswork:
		//
		//   BGSEntryPointPerkEntry   vtable @00, header @08, entryData @10,
		//                            functionData @18, conditions @20, perk @28
		//   BGSEntryPointFunctionDataOneValue   vtable @00, data (float) @08
		//
		// conditions stays zeroed - an entry with no conditions always applies,
		// which is what we want and what a great many vanilla perks already are.
		//
		// VR: 0x100 is the SE/AE index. Actor.h declares ForEachPerkEntry as
		// SKYRIM_REL_VR_VIRTUAL, so a VR build needs the index resolved per
		// runtime before this can ship.
		// -------------------------------------------------------------------
		// The entry starts neutral. E1 used a fixed 2.0 to prove the number could
		// be written at all; E2 drives it from the channel instead, so the value
		// here only ever applies before the first read, and 1.0 changes nothing.
		constexpr float kNeutralMultiplier = 1.0f;

		// BGSEntryPointPerkEntry::EntryData::numArgs is NOT an argument count,
		// whatever the CommonLib field name suggests. In the PERK record it is
		// the third byte of DATA, which the record format calls the perk
		// condition tab count - how many condition tabs the entry carries.
		//
		// E1c declared 3 with a null conditions array and the engine walked into
		// the first of three tabs that were not there: access violation reading
		// address 0, inside condition evaluation, reached straight from our
		// visit. The field has to agree with the array, always. E1d therefore
		// declares 0 and carries none, and CheckConsistency below refuses to
		// hand the engine an entry where the two disagree.
		constexpr std::uint8_t kNoConditionTabs = 0;

		alignas(RE::BGSEntryPointPerkEntry) std::byte            g_syntheticEntryStorage[sizeof(RE::BGSEntryPointPerkEntry)]{};
		alignas(RE::BGSEntryPointFunctionDataOneValue) std::byte g_syntheticDataStorage[sizeof(RE::BGSEntryPointFunctionDataOneValue)]{};

		RE::BGSEntryPointPerkEntry*            g_syntheticEntry{ nullptr };
		RE::BGSEntryPointFunctionDataOneValue* g_syntheticData{ nullptr };

		EffectType g_actorTypes[] = {
			{ RE::VTABLE_Character[0] },
			{ RE::VTABLE_PlayerCharacter[0] },
		};

		EffectType g_perkEntryTypes[] = {
			{ RE::VTABLE_BGSEntryPointPerkEntry[0] },
		};

		// Hook: BGSEntryPointPerkEntry::CheckConditionFilters - vfunc 0x00
		//
		// E1 spent four runs trying to build an entry the engine would accept on
		// its own, and the results only make sense one way:
		//
		//   E1   0 tabs, no conditions  -> discarded, no crash
		//   E1b  3 tabs, real ones      -> applied, but fire only
		//   E1c  3 tabs, null array     -> access violation walking the array
		//   E1d  0 tabs, no conditions  -> discarded again
		//
		// Zero condition tabs does not mean "always passes". It means no tab
		// matched, and the engine reads that as a failure. There was never a
		// field to get right: an entry carrying no conditions cannot pass this
		// check, so a synthetic one has to be answered for here. That is what
		// Perk Entry Point Extender does at this same vfunc, and reading its
		// source is what ended the guessing.
		//
		// The swap is global - every entry point perk entry in the load order
		// now routes its condition check through this thunk - so the body is a
		// pointer compare and a tail call, and anything that is not ours reaches
		// the engine's own implementation untouched.
		bool CheckConditionFiltersThunk(RE::BGSEntryPointPerkEntry* a_this, std::uint32_t a_numArgs, void* a_args)
		{
			const auto vtable   = a_this ? *reinterpret_cast<std::uintptr_t*>(a_this) : 0;
			const auto original = FindOriginalIn(g_perkEntryTypes, std::size(g_perkEntryTypes), vtable);
			if (!original) {
				return false;
			}

			if (a_this && a_this == g_syntheticEntry) {
				return true;
			}

			return reinterpret_cast<decltype(&CheckConditionFiltersThunk)>(original)(a_this, a_numArgs, a_args);
		}

		// E1 built this entry field by field and the engine discarded it: the
		// visitor took 5774 calls without complaint and no number moved. That
		// rules out "the hook never fires" and leaves two possibilities, which
		// no amount of re-reading the headers can separate -
		//
		//   (a) the entry is malformed in some field the headers do not spell
		//       out, so the engine rejects it;
		//   (b) contributing through the visitor does not feed the result at
		//       all, and the whole approach is wrong.
		//
		// E1b separates them by measurement instead of by argument. It CLONES a
		// real entry that the engine demonstrably honours - Augmented Flames,
		// measured moving Firebolt 25 -> 31 - and swaps only the functionData
		// pointer for our own. Every field we might have gotten wrong then comes
		// from something known to work.
		//
		//   clone shows 50 -> injection is sound, our construction was wrong,
		//                     and the field dump below says which field.
		//   clone shows 25 -> the visitor is not the seam that feeds the result,
		//                     and design D needs rethinking before any more of it
		//                     gets built.
		//
		// The clone shares the original's conditions array and perk pointer. That
		// is read-only for evaluation and this copy is never destructed, so
		// nothing owned by the game is freed.
		RE::BGSEntryPointPerkEntry* g_referenceEntry{ nullptr };

		void DumpEntry(const char* a_label, RE::BGSEntryPointPerkEntry* a_entry)
		{
			if (!a_entry) {
				spdlog::info("MagicScaling/E1b: {} - none.", a_label);
				return;
			}

			// conditions is logged by SIZE and not only by pointer. E1b dumped
			// the pointer alone, which is why the reference entry's numArgs=3
			// sitting next to exactly 3 condition tabs went unnoticed, and the
			// wrong field got the credit for that experiment working.
			spdlog::info("MagicScaling/E1: {} vtable=0x{:016X} rank={} priority={} entryPoint={} function={} "
						 "conditionTabs(numArgs)={} conditions.size()={} conditions=0x{:016X} "
						 "functionData=0x{:016X} perk=0x{:016X}",
				a_label,
				*reinterpret_cast<std::uintptr_t*>(a_entry),
				a_entry->header.rank,
				a_entry->header.priority,
				a_entry->entryData.entryPoint.underlying(),
				a_entry->entryData.function.underlying(),
				a_entry->entryData.numArgs,
				a_entry->conditions.size(),
				reinterpret_cast<std::uintptr_t>(a_entry->conditions.data()),
				reinterpret_cast<std::uintptr_t>(a_entry->functionData),
				reinterpret_cast<std::uintptr_t>(a_entry->perk));
		}

		// Augmented Flames rank 1 - the entry whose effect on the tooltip was
		// measured directly. Any perk with a kModSpellMagnitude/kOneValue entry
		// would do; this one is used because its behavior is already known.
		constexpr RE::FormID kReferencePerkID = 0x000581E7;

		RE::BGSEntryPointPerkEntry* FindReferenceEntry()
		{
			auto* handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return nullptr;
			}

			RE::BGSEntryPointPerkEntry* fallback = nullptr;

			for (auto* perk : handler->GetFormArray<RE::BGSPerk>()) {
				if (!perk) {
					continue;
				}

				for (auto* entry : perk->perkEntries) {
					if (!entry || entry->GetType() != RE::PERK_ENTRY_TYPE::kEntryPoint) {
						continue;
					}

					auto* epEntry = static_cast<RE::BGSEntryPointPerkEntry*>(entry);
					if (epEntry->entryData.entryPoint.get() != RE::BGSEntryPoint::ENTRY_POINT::kModSpellMagnitude) {
						continue;
					}

					auto* fd = epEntry->functionData;
					if (!fd || fd->GetType() != RE::BGSEntryPointFunctionData::FunctionType::kOneValue) {
						continue;
					}

					if (perk->GetFormID() == kReferencePerkID) {
						spdlog::info("MagicScaling/E1b: reference entry is '{}' (0x{:08X}), the measured one.",
							perk->GetName() ? perk->GetName() : "<unnamed>", perk->GetFormID());
						return epEntry;
					}

					if (!fallback) {
						fallback = epEntry;
						spdlog::info("MagicScaling/E1b: holding '{}' (0x{:08X}) as a fallback reference.",
							perk->GetName() ? perk->GetName() : "<unnamed>", perk->GetFormID());
					}
				}
			}

			return fallback;
		}

		bool BuildSyntheticEntry()
		{
			REL::Relocation<std::uintptr_t> entryVtbl{ RE::VTABLE_BGSEntryPointPerkEntry[0] };
			REL::Relocation<std::uintptr_t> dataVtbl{ RE::VTABLE_BGSEntryPointFunctionDataOneValue[0] };

			auto* data = reinterpret_cast<RE::BGSEntryPointFunctionDataOneValue*>(g_syntheticDataStorage);
			*reinterpret_cast<std::uintptr_t*>(data) = dataVtbl.address();
			data->data                               = kNeutralMultiplier;
			g_syntheticData                          = data;

			// E1d - one variable moved from E1, which is the only honest way left
			// to read the result. What each run actually carried:
			//
			//   E1    tabs=0  conditions=NULL  perk=NULL  -> discarded, no crash
			//   E1b   tabs=3  conditions=real  perk=real  -> worked, fire only
			//   E1c   tabs=3  conditions=NULL  perk=real  -> CRASH in condition eval
			//
			// E1 was internally consistent - zero tabs, zero conditions - so the
			// tab count was never what got it discarded, and the conclusion drawn
			// from E1b was wrong: that run changed three fields at once and the
			// credit went to the wrong one. The only difference left between E1
			// and a working entry is the null perk.
			//
			// So E1d is E1 with a borrowed perk and nothing else changed. It also
			// happens to be the configuration the real design needs, since an
			// entry with no conditions is one that applies to every spell rather
			// than to fire.
			std::memset(g_syntheticEntryStorage, 0, sizeof(g_syntheticEntryStorage));

			auto* entry      = reinterpret_cast<RE::BGSEntryPointPerkEntry*>(g_syntheticEntryStorage);
			g_referenceEntry = FindReferenceEntry();

			*reinterpret_cast<std::uintptr_t*>(entry) = entryVtbl.address();
			entry->header.rank                        = 0;
			entry->header.priority                    = 0;
			entry->entryData.entryPoint               = RE::BGSEntryPoint::ENTRY_POINT::kModSpellMagnitude;
			entry->entryData.function                 = RE::BGSEntryPointPerkEntry::EntryData::Function::kMultiplyValue;
			entry->entryData.numArgs                  = kNoConditionTabs;
			entry->functionData                       = data;
			entry->perk                               = g_referenceEntry ? g_referenceEntry->perk : nullptr;
			// conditions stays null from the memset, and the tab count above
			// agrees with it. Those two must move together - see the guard.

			g_syntheticEntry = entry;

			DumpEntry("REFERENCE (real, honoured by the engine)", g_referenceEntry);
			DumpEntry("INJECTED  (E1d: no conditions, borrowed perk)", g_syntheticEntry);

			// THE GUARD. E1c crashed the game because the tab count and the
			// conditions array disagreed, and nothing checked. This is cheap,
			// runs once, and refuses to hand the engine an entry that would send
			// it walking off the end of an array that is not there. Returning
			// false leaves the module exactly as it was without the hook.
			const auto tabs       = static_cast<std::size_t>(entry->entryData.numArgs);
			const auto conditions = entry->conditions.size();
			if (tabs != conditions) {
				spdlog::error("MagicScaling/E1d: REFUSING to install - the entry declares {} condition tab(s) but "
							  "carries {}. The engine iterates the declared count, so this is the exact shape that "
							  "caused an access violation in condition evaluation. Hook not installed.",
					tabs, conditions);
				g_syntheticEntry = nullptr;
				return false;
			}

			if (!entry->perk) {
				spdlog::warn("MagicScaling/E1d: no reference perk in the load order, so perk is null - that is the "
							 "field under test, and this run cannot answer the question it was built to answer.");
			}

			return true;
		}

		void ForEachPerkEntryThunk(RE::Actor* a_this, RE::BGSEntryPoint::ENTRY_POINT a_entryType, RE::PerkEntryVisitor& a_visitor)
		{
			const auto vtable   = a_this ? *reinterpret_cast<std::uintptr_t*>(a_this) : 0;
			const auto original = FindOriginalIn(g_actorTypes, std::size(g_actorTypes), vtable);
			if (!original) {
				return;
			}

			// Real perks first, ours after, so nothing the engine already had is
			// displaced by this.
			reinterpret_cast<decltype(&ForEachPerkEntryThunk)>(original)(a_this, a_entryType, a_visitor);

			try {
				if (a_entryType != RE::BGSEntryPoint::ENTRY_POINT::kModSpellMagnitude) {
					return;
				}

				if (!g_syntheticEntry) {
					return;
				}

				if (!a_this || !a_this->IsPlayerRef()) {
					return;
				}

				// Inert with no consumer, same contract as every other hook here.
				float mult = 0.0f, offset = 0.0f;
				if (!g_magnitude.Read(mult, offset) || !g_syntheticData) {
					return;
				}

				// STAGE E2 - THE QUESTION THIS STAGE EXISTS TO ANSWER.
				//
				// E1e proved the number in the spell menu can be written: a fixed
				// 2.0 moved Firebolt to 50, Healing to 20 and Lightning Bolt to
				// +80, across three schools. What it could NOT show is whether
				// that number FOLLOWS a multiplier that changes during play,
				// because the 2.0 never changed. The tooltip read the same at
				// attribute 0 and 85, which is exactly the symptom the whole
				// feature exists to fix.
				//
				// So the value stops being a constant here. It is read from the
				// registered channel on every query, immediately before the entry
				// is handed over, which is the same discipline Channel::Read
				// follows everywhere else in this module - no caching, the global
				// is the truth at the moment it is asked for.
				//
				// If the menu still shows the same number at two attribute values
				// after this, the description is computed once and cached, and
				// writing in the right place cannot help. That outcome ends this
				// approach rather than extending it.
				g_syntheticData->data = mult;

				// The return value is captured because it is the only thing the
				// engine tells us about the entry: kBreak here would mean the
				// visitor is done, kContinue that it took ours and wants more.
				// It is not proof either way, but it is free.
				const auto result = a_visitor.Visit(g_syntheticEntry);

				if (ShouldLogEvents()) {
					// The offset half of the channel contract has no equivalent
					// here: kMultiplyValue multiplies and nothing else. Consumers
					// measured so far drive magnitude with offset 0, so this is
					// logged rather than worked around - if one ever needs it, a
					// second entry carrying kAddValue is the answer, and that is
					// a design decision, not a silent approximation.
					if (offset != 0.0f) {
						spdlog::debug("MagicScaling/E2: channel offset {:.4f} is NOT applied at this seam - "
									  "kMultiplyValue only multiplies.",
							offset);
					}

					spdlog::debug("MagicScaling/E2: contributed kMultiplyValue {:.4f} from the live channel, "
								  "visitor returned {}.",
						mult,
						result == RE::PerkEntryVisitor::ReturnType::kContinue ? "kContinue" : "kBreak");
				}
			} catch (...) {
				// The original already ran, so the actor's real perks were
				// visited either way.
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

		// STAGE E1 - installed last, after the two channels that already work, so
		// a failure building the synthetic entry cannot stop them from going in.
		try {
			if (BuildSyntheticEntry()) {
				// Condition answer first: from the moment the actor hook is live
				// the engine can ask our entry whether its conditions pass, and
				// the answer has to already be in place.
				InstallVfunc(g_perkEntryTypes, std::size(g_perkEntryTypes), 0x00, reinterpret_cast<void*>(&CheckConditionFiltersThunk));
				InstallVfunc(g_actorTypes, std::size(g_actorTypes), 0x100, reinterpret_cast<void*>(&ForEachPerkEntryThunk));

				spdlog::info("MagicScaling: STAGE E2 installed - CheckConditionFilters @0x00 on the entry point perk "
							 "entry vtable, Actor::ForEachPerkEntry @0x100 on {} actor vtables. The contributed "
							 "kModSpellMagnitude entry now carries the REGISTERED CHANNEL's multiplier, read fresh on "
							 "every query. AdjustForPerks still scales as well, so the two compound for now - that is "
							 "expected at this stage and is what E3 removes.",
					std::size(g_actorTypes));
			}
		} catch (const std::exception& e) {
			spdlog::error("MagicScaling: stage E1 failed to install: {} - the spell menu seam is not hooked.", e.what());
		} catch (...) {
			spdlog::error("MagicScaling: stage E1 failed to install (unknown exception) - the spell menu seam is not hooked.");
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
