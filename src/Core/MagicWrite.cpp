// MagicWrite.cpp
// Lodestone - Shared SKSE framework
//
// Natives behind Lodestone.CopySpell, ClearSpellEffects, AddSpellEffect,
// SetSpellAutoCalculate, SetSpellCostOverride, CopyMagicEffect and
// CopyMagicEffectAppearance.
//
// WHAT THIS IS FOR. A consumer that needs many spell variants declares the
// SPEL and MGEF records EMPTY in its own plugin and fills them on every load
// from a recipe it keeps. The FormIDs exist from the moment the plugin loads,
// so a save resolves its references on its own, and nothing here allocates a
// form, a FormID or a cosave record. Content written here is session-only, the
// same doctrine as EffectDescription and BookFramework v1: nothing goes to the
// save, the next launch starts from the plugin, a save loaded later in the same
// session finds the last write, and the consumer writes again on every load.
//
// THREAD. Every native here stays on the default registration and writes
// directly. SpellRead.cpp records the measurement this rests on: a native on
// the default registration costs one frame per call, i.e. it waits for the
// game's main thread. That it then RUNS on the main thread, so a write cannot
// race the engine reading the same spell, is inferred from that cost, not read.
// EffectDescription queues its write through the task interface instead; that
// is not done here because the consumer reads back right after writing, and a
// queued write would return before landing.
//
// EFFECT ENTRIES ARE RECYCLED, NOT FREED. MagicItem::effects is a BSTArray of
// Effect POINTERS, and an active effect on an actor points at the Effect entry
// of the spell that cast it. A spell that is active on the player when a save
// loads is rewritten by the consumer right after the load, while that active
// effect still holds its pointer: freeing the old entry would leave it
// dangling. So an entry taken out of a spell (by a clear or by a rewrite) goes
// to a spare list kept per spell, and the next entry that spell needs is taken
// from that list before anything new is allocated. A consumer that rewrites a
// spell with the same recipe on every load gets the same entry objects back,
// in the same order, every time: the memory a spell holds is bounded by the
// largest number of effects it ever had, and does not grow per load.
// What IS freed on every rewrite: the condition list of each recycled entry,
// and the keyword array the form held before.
//
// NO ENTRY IS EVER SHARED WITH THE SOURCE. CopySpell builds the target's list
// from entries the target owns (recycled or new) and copies the source's
// values into them, conditions included as new nodes; a pointer that the
// target and the source both hold (left by a shallow copy made elsewhere) is
// dropped from the target without being reused or freed.
//
// hostileCount. MagicItem::IsHostile reads hostileCount > 0. CopySpell takes
// the source's count; AddSpellEffect adds one for an effect flagged hostile;
// ClearSpellEffects sets zero. That the engine counts exactly the hostile
// effects is INFERRED from IsHostile, not read.
//
// NOT WRITTEN. The spell's TESDescription (the engine loads it from the plugin
// file on demand); MagicItem::avEffectSetting and the preload fields outside
// CopySpell; EffectSetting's filter validation pointers and its two loaded
// counters, which are runtime bookkeeping and not content.
//
// Mechanism studied, not copied, from Dynamic Persistent Forms v1
// (github.com/Thiago099/DPF-Dynamic-Persistent-Forms, 3a6e8a8): new Effect
// plus push_back for an added entry, and the MGEF appearance field list.
//
// Phase L-F5

#include "MagicWrite.h"

#include "Config.h"

#include <deque>
#include <unordered_map>

namespace Lodestone::Core::MagicWrite
{
	namespace
	{
		bool g_log = false;  // Lodestone.ini LogMagicWrite = 1

		// Entries taken out of a spell, waiting to be reused by the same spell.
		// Main thread only (see THREAD above).
		std::unordered_map<RE::SpellItem*, std::deque<RE::Effect*>> g_spare;

		// --- conditions -----------------------------------------------------

		void FreeConditionChain(RE::TESConditionItem* a_head)
		{
			while (a_head) {
				RE::TESConditionItem* next = a_head->next;
				delete a_head;
				a_head = next;
			}
		}

		// Replaces a_dst with new nodes holding a_src's data. The new chain is
		// built before the old one is freed, and the old one is freed only when
		// it is not a_src's own chain.
		void CopyConditions(RE::TESCondition& a_dst, const RE::TESCondition& a_src)
		{
			RE::TESConditionItem*  head = nullptr;
			RE::TESConditionItem** tail = &head;
			for (const RE::TESConditionItem* item = a_src.head; item; item = item->next) {
				auto* copy = new RE::TESConditionItem();
				copy->data = item->data;
				copy->next = nullptr;
				*tail = copy;
				tail = &copy->next;
			}

			RE::TESConditionItem* old = a_dst.head;
			a_dst.head = head;
			if (old && old != a_src.head) {
				FreeConditionChain(old);
			}
		}

		void ClearConditions(RE::TESCondition& a_cond)
		{
			RE::TESConditionItem* old = a_cond.head;
			a_cond.head = nullptr;
			FreeConditionChain(old);
		}

		// --- keywords -------------------------------------------------------

		// Replaces the keyword array of a_dst with a copy of a_src's, on the
		// game heap (the allocator BGSKeywordForm itself uses), and frees the
		// old array.
		void CopyKeywords(RE::BGSKeywordForm& a_dst, const RE::BGSKeywordForm& a_src)
		{
			RE::BGSKeyword** old = a_dst.keywords;
			if (old == a_src.keywords) {
				return;
			}

			RE::BGSKeyword**    fresh = nullptr;
			const std::uint32_t count = a_src.keywords ? a_src.numKeywords : 0;
			if (count > 0) {
				fresh = RE::calloc<RE::BGSKeyword*>(count);
				for (std::uint32_t i = 0; i < count; ++i) {
					fresh[i] = a_src.keywords[i];
				}
			}

			a_dst.keywords = fresh;
			a_dst.numKeywords = fresh ? count : 0;
			if (old) {
				RE::free(old);
			}
		}

		// --- effect entries -------------------------------------------------

		// Moves every entry of a_spell to its spare list, freeing each entry's
		// conditions. Entries listed in a_keep (shared with a source) are dropped
		// from the array and left alone.
		void RetireEffects(RE::SpellItem* a_spell, const RE::BSTArray<RE::Effect*>* a_keep)
		{
			auto& spare = g_spare[a_spell];
			for (RE::Effect* effect : a_spell->effects) {
				if (!effect) {
					continue;
				}
				bool shared = false;
				if (a_keep) {
					for (const RE::Effect* other : *a_keep) {
						if (other == effect) {
							shared = true;
							break;
						}
					}
				}
				if (shared) {
					continue;
				}
				ClearConditions(effect->conditions);
				spare.push_back(effect);
			}
			a_spell->effects.clear();
		}

		// An entry a_spell owns: the oldest spare one, or a new one.
		RE::Effect* TakeEffect(RE::SpellItem* a_spell, bool& a_reused)
		{
			auto& spare = g_spare[a_spell];
			if (!spare.empty()) {
				RE::Effect* effect = spare.front();
				spare.pop_front();
				a_reused = true;
				return effect;
			}
			a_reused = false;
			return new RE::Effect();
		}

		void FillEffect(RE::Effect* a_effect, RE::EffectSetting* a_base, float a_magnitude, std::uint32_t a_area, std::uint32_t a_duration, float a_cost)
		{
			a_effect->effectItem.magnitude = a_magnitude;
			a_effect->effectItem.area = a_area;
			a_effect->effectItem.duration = a_duration;
			a_effect->baseEffect = a_base;
			a_effect->cost = a_cost;
		}

		bool IsSpell(const RE::SpellItem* a_spell)
		{
			return a_spell && a_spell->GetFormType() == RE::FormType::Spell;
		}

		bool IsMagicEffect(const RE::EffectSetting* a_effect)
		{
			return a_effect && a_effect->GetFormType() == RE::FormType::MagicEffect;
		}

		// --- natives: spells ------------------------------------------------

		// Lodestone.CopySpell(Spell akTarget, Spell akSource) -> Bool
		bool CopySpell(RE::StaticFunctionTag*, RE::SpellItem* a_target, RE::SpellItem* a_source)
		{
			try {
				if (!IsSpell(a_target) || !IsSpell(a_source)) {
					return false;
				}
				if (a_target == a_source) {
					return true;
				}

				a_target->data = a_source->data;
				a_target->equipSlot = a_source->equipSlot;
				a_target->menuDispObject = a_source->menuDispObject;
				a_target->fullName = a_source->fullName;
				CopyKeywords(*a_target, *a_source);

				RetireEffects(a_target, &a_source->effects);
				std::uint32_t reused = 0;
				std::uint32_t fresh = 0;
				for (const RE::Effect* src : a_source->effects) {
					if (!src) {
						continue;
					}
					bool        wasReused = false;
					RE::Effect* effect = TakeEffect(a_target, wasReused);
					FillEffect(effect, src->baseEffect, src->effectItem.magnitude, src->effectItem.area, src->effectItem.duration, src->cost);
					CopyConditions(effect->conditions, src->conditions);
					a_target->effects.push_back(effect);
					wasReused ? ++reused : ++fresh;
				}
				a_target->hostileCount = a_source->hostileCount;
				a_target->avEffectSetting = a_source->avEffectSetting;

				if (g_log) {
					spdlog::info("MagicWrite: CopySpell target {:08X} source {:08X} effects {} (reused {}, new {}).",
						a_target->GetFormID(), a_source->GetFormID(), a_target->effects.size(), reused, fresh);
				}
				return true;
			} catch (...) {
				return false;
			}
		}

		// Lodestone.ClearSpellEffects(Spell akSpell) -> Bool
		bool ClearSpellEffects(RE::StaticFunctionTag*, RE::SpellItem* a_spell)
		{
			try {
				if (!IsSpell(a_spell)) {
					return false;
				}
				const auto before = a_spell->effects.size();
				RetireEffects(a_spell, nullptr);
				a_spell->hostileCount = 0;

				if (g_log) {
					spdlog::info("MagicWrite: ClearSpellEffects target {:08X} effects {} -> 0 (spare {}).",
						a_spell->GetFormID(), before, g_spare[a_spell].size());
				}
				return true;
			} catch (...) {
				return false;
			}
		}

		// Lodestone.AddSpellEffect(Spell, MagicEffect, Float, Int, Int, Float) -> Bool
		bool AddSpellEffect(RE::StaticFunctionTag*, RE::SpellItem* a_spell, RE::EffectSetting* a_effect, float a_magnitude, std::int32_t a_area, std::int32_t a_duration, float a_cost)
		{
			try {
				if (!IsSpell(a_spell) || !IsMagicEffect(a_effect) || a_area < 0 || a_duration < 0) {
					return false;
				}

				bool        reused = false;
				RE::Effect* entry = TakeEffect(a_spell, reused);
				FillEffect(entry, a_effect, a_magnitude, static_cast<std::uint32_t>(a_area), static_cast<std::uint32_t>(a_duration), a_cost);
				ClearConditions(entry->conditions);
				a_spell->effects.push_back(entry);
				if (a_effect->IsHostile()) {
					++a_spell->hostileCount;
				}

				if (g_log) {
					spdlog::info("MagicWrite: AddSpellEffect target {:08X} effect {:08X} effects {} ({}).",
						a_spell->GetFormID(), a_effect->GetFormID(), a_spell->effects.size(), reused ? "reused" : "new");
				}
				return true;
			} catch (...) {
				return false;
			}
		}

		// Lodestone.SetSpellAutoCalculate(Spell, Bool) -> Bool
		bool SetSpellAutoCalculate(RE::StaticFunctionTag*, RE::SpellItem* a_spell, bool a_autoCalc)
		{
			try {
				if (!IsSpell(a_spell)) {
					return false;
				}
				if (a_autoCalc) {
					a_spell->data.flags.reset(RE::SpellItem::SpellFlag::kCostOverride);
				} else {
					a_spell->data.flags.set(RE::SpellItem::SpellFlag::kCostOverride);
				}
				if (g_log) {
					spdlog::info("MagicWrite: SetSpellAutoCalculate target {:08X} -> {}.", a_spell->GetFormID(), a_autoCalc ? "auto" : "fixed");
				}
				return true;
			} catch (...) {
				return false;
			}
		}

		// Lodestone.SetSpellCostOverride(Spell, Int) -> Bool
		bool SetSpellCostOverride(RE::StaticFunctionTag*, RE::SpellItem* a_spell, std::int32_t a_cost)
		{
			try {
				if (!IsSpell(a_spell) || a_cost < 0) {
					return false;
				}
				a_spell->data.costOverride = a_cost;
				a_spell->data.flags.set(RE::SpellItem::SpellFlag::kCostOverride);
				if (g_log) {
					spdlog::info("MagicWrite: SetSpellCostOverride target {:08X} -> {}.", a_spell->GetFormID(), a_cost);
				}
				return true;
			} catch (...) {
				return false;
			}
		}

		// --- natives: magic effects -----------------------------------------

		void CopyAppearanceFields(RE::EffectSetting* a_dst, const RE::EffectSetting* a_src)
		{
			auto&       d = a_dst->data;
			const auto& s = a_src->data;
			d.castingArt = s.castingArt;
			d.hitEffectArt = s.hitEffectArt;
			d.enchantEffectArt = s.enchantEffectArt;
			d.effectShader = s.effectShader;
			d.enchantShader = s.enchantShader;
			d.light = s.light;
			d.hitVisuals = s.hitVisuals;
			d.enchantVisuals = s.enchantVisuals;
			d.projectileBase = s.projectileBase;
			d.explosion = s.explosion;
			d.impactDataSet = s.impactDataSet;
			d.imageSpaceMod = s.imageSpaceMod;
			d.dualCastData = s.dualCastData;
			d.dualCastScale = s.dualCastScale;
			d.castingSoundLevel = s.castingSoundLevel;
			a_dst->effectSounds = a_src->effectSounds;
			a_dst->menuDispObject = a_src->menuDispObject;
		}

		// Lodestone.CopyMagicEffect(MagicEffect akTarget, MagicEffect akSource) -> Bool
		bool CopyMagicEffect(RE::StaticFunctionTag*, RE::EffectSetting* a_target, RE::EffectSetting* a_source)
		{
			try {
				if (!IsMagicEffect(a_target) || !IsMagicEffect(a_source)) {
					return false;
				}
				if (a_target == a_source) {
					return true;
				}

				a_target->data = a_source->data;
				a_target->counterEffects = a_source->counterEffects;
				a_target->effectSounds = a_source->effectSounds;
				a_target->magicItemDescription = a_source->magicItemDescription;
				a_target->fullName = a_source->fullName;
				a_target->menuDispObject = a_source->menuDispObject;
				CopyKeywords(*a_target, *a_source);
				CopyConditions(a_target->conditions, a_source->conditions);

				if (g_log) {
					spdlog::info("MagicWrite: CopyMagicEffect target {:08X} source {:08X} archetype {} keywords {} loaded {}/{}.",
						a_target->GetFormID(), a_source->GetFormID(), static_cast<std::int32_t>(a_target->data.archetype),
						a_target->numKeywords, a_target->effectLoadedCount, a_target->associatedItemLoadedCount);
				}
				return true;
			} catch (...) {
				return false;
			}
		}

		// Lodestone.CopyMagicEffectAppearance(MagicEffect akTarget, MagicEffect akSource) -> Bool
		bool CopyMagicEffectAppearance(RE::StaticFunctionTag*, RE::EffectSetting* a_target, RE::EffectSetting* a_source)
		{
			try {
				if (!IsMagicEffect(a_target) || !IsMagicEffect(a_source)) {
					return false;
				}
				if (a_target == a_source) {
					return true;
				}
				CopyAppearanceFields(a_target, a_source);
				if (g_log) {
					spdlog::info("MagicWrite: CopyMagicEffectAppearance target {:08X} source {:08X} sounds {} loaded {}/{}.",
						a_target->GetFormID(), a_source->GetFormID(), a_target->effectSounds.size(),
						a_target->effectLoadedCount, a_target->associatedItemLoadedCount);
				}
				return true;
			} catch (...) {
				return false;
			}
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("MagicWrite: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("CopySpell", "Lodestone", CopySpell);
		a_vm->RegisterFunction("ClearSpellEffects", "Lodestone", ClearSpellEffects);
		a_vm->RegisterFunction("AddSpellEffect", "Lodestone", AddSpellEffect);
		a_vm->RegisterFunction("SetSpellAutoCalculate", "Lodestone", SetSpellAutoCalculate);
		a_vm->RegisterFunction("SetSpellCostOverride", "Lodestone", SetSpellCostOverride);
		a_vm->RegisterFunction("CopyMagicEffect", "Lodestone", CopyMagicEffect);
		a_vm->RegisterFunction("CopyMagicEffectAppearance", "Lodestone", CopyMagicEffectAppearance);

		Config::ForEachPair([](std::string_view a_key, std::string_view a_value) {
			if (a_key == "logmagicwrite") {
				g_log = (Config::Trim(a_value) == "1");
			}
		});

		spdlog::info("MagicWrite: natives registered (CopySpell, ClearSpellEffects, AddSpellEffect, SetSpellAutoCalculate, "
					 "SetSpellCostOverride, CopyMagicEffect, CopyMagicEffectAppearance). Log {}.",
			g_log ? "ON" : "off");
		return true;
	}
}
