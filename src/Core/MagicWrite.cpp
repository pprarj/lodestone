// MagicWrite.cpp
// Lodestone - Shared SKSE framework
//
// Natives behind Lodestone.CopySpell, ClearSpellEffects, AddSpellEffect,
// SetSpellAutoCalculate, SetSpellCostOverride, CopyMagicEffect and
// CopyMagicEffectAppearance, and the per-save record that puts what they wrote
// back in place before the engine reads a save.
//
// WHAT THIS IS FOR. A consumer that needs many spell variants declares the
// SPEL and MGEF records EMPTY in its own plugin and fills them on every load
// from a recipe it keeps. The FormIDs exist from the moment the plugin loads,
// so a save resolves its references on its own, and nothing here allocates a
// form or a FormID. The consumer still writes again on every load.
//
// WHY THE CONTENT IS ALSO KEPT PER SAVE (L-F5 stage 2). An active effect that
// was running when the game saved is rebuilt by the engine while it reads the
// save, from whatever the spell and the magic effect hold AT THAT MOMENT. The
// consumer's own rewrite comes later, from OnPlayerLoadGame. In a fresh launch
// the forms hold the plugin's empty declaration then, so the effect comes back
// as something else (another archetype, another actor value) and its end never
// gives back what it changed. So:
//   - every save carries, in the 'LDST' cosave, one record with the final state
//     of every form these natives touched in this process;
//   - at kPreLoadGame, BEFORE SKSE hands the load to the engine, this module
//     reads that record straight from the save's .skse file and writes it back
//     into the forms; forms touched in this process that the record does not
//     list go back to the state they had before the first write;
//   - a save without the record (new game, save made before this) changes
//     nothing, as before.
// kPreLoadGame is used, and not the cosave load callback, because SKSE runs
// that callback from inside the engine's load (the Papyrus VM's load of its
// global data), and nothing read so far says it comes before the actors.
// The callback still sees the record: if the pre-load read failed, the record
// is applied there instead, and the log says it was late.
//
// CONDITIONS ARE KEPT BY ORIGIN, NOT BY VALUE. A condition's parameters are
// form pointers or plain numbers depending on the function, and nothing here
// knows which. Every condition list these natives write is a copy of another
// form's list (CopySpell, CopyMagicEffect) or empty (AddSpellEffect). So each
// written list remembers where it came from - a spell and an entry index, or a
// magic effect - as that form was BEFORE this process first wrote it, and the
// record stores that origin. Restoring rebuilds the list from the origin.
//
// THREAD. Every native here stays on the default registration and writes
// directly. SpellRead.cpp records the measurement this rests on: a native on
// the default registration costs one frame per call, i.e. it waits for the
// game's main thread. That it then RUNS on the main thread, so a write cannot
// race the engine reading the same spell, is inferred from that cost, not read.
// kPreLoadGame, kPostLoadGame and the cosave callbacks run inside the game's
// save and load, on the same thread. EffectDescription queues its write through
// the task interface instead; that is not done here because the consumer reads
// back right after writing, and a queued write would return before landing.
//
// EFFECT ENTRIES ARE RECYCLED, NOT FREED. MagicItem::effects is a BSTArray of
// Effect POINTERS, and an active effect on an actor points at the Effect entry
// of the spell that cast it. A spell that is active on the player when a save
// loads is rewritten right before and right after the load, while that active
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
// LOAD ORDER PROBE. With LogMagicWrite = 1 the module also swaps
// ActiveEffect::LoadGame and FinishLoadGame on the eight effect classes
// MagicScaling already knows, and logs, with the time since kPreLoadGame: the
// pre-load restore, the cosave load callback, the first effect loaded, every
// loaded effect whose spell these natives wrote, and kPostLoadGame. With the
// log off nothing is swapped.
//
// Mechanism studied, not copied, from Dynamic Persistent Forms v1
// (github.com/Thiago099/DPF-Dynamic-Persistent-Forms, 3a6e8a8): new Effect
// plus push_back for an added entry, and the MGEF appearance field list.
//
// Phase L-F5

#include "MagicWrite.h"

#include "Config.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace Lodestone::Core::MagicWrite
{
	namespace
	{
		bool g_log = false;  // Lodestone.ini LogMagicWrite = 1

		// The record inside the plugin's 'LDST' cosave.
		constexpr std::uint32_t kPluginID = 'LDST';  // Serialization.cpp kSerializationID
		constexpr std::uint32_t kRecordType = 'MWST';
		constexpr std::uint32_t kRecordVersion = 1;

		// Sanity caps for a record read back. Far above any real content; a
		// count above them means the bytes are not this record.
		constexpr std::uint32_t kMaxList = 1u << 16;
		constexpr std::uint32_t kMaxString = 1u << 20;

		// Entries taken out of a spell, waiting to be reused by the same spell.
		// Main thread only (see THREAD above).
		std::unordered_map<RE::SpellItem*, std::deque<RE::Effect*>> g_spare;

		using ConditionData = RE::CONDITION_ITEM_DATA;
		using ConditionList = std::vector<ConditionData>;

		// --- where a condition list came from ---------------------------------

		enum class OriginKind : std::uint8_t
		{
			kNone = 0,         // no conditions
			kSpellEntry = 1,   // entry `index` of spell `form`
			kMagicEffect = 2,  // the conditions of magic effect `form`
		};

		// Always read as the origin form was BEFORE this process first wrote
		// it: its pristine copy if it has one, its live state if it was never
		// written (then live is pristine).
		struct Origin
		{
			OriginKind   kind{ OriginKind::kNone };
			RE::TESForm* form{ nullptr };
			std::int32_t index{ -1 };
		};

		// Origins of the lists these natives wrote, by owner.
		std::unordered_map<RE::Effect*, Origin>        g_entryOrigin;
		std::unordered_map<RE::EffectSetting*, Origin> g_effectOrigin;

		// --- state of a form ------------------------------------------------

		struct EffectValues
		{
			RE::EffectSetting* base{ nullptr };
			float              magnitude{ 0.0f };
			std::uint32_t      area{ 0 };
			std::uint32_t      duration{ 0 };
			float              cost{ 0.0f };
			Origin             origin;
		};

		struct SpellState
		{
			RE::SpellItem::Data       data{};
			RE::BGSEquipSlot*         equipSlot{ nullptr };
			RE::TESBoundObject*       menuDispObject{ nullptr };
			std::string               fullName;
			std::vector<RE::BGSKeyword*> keywords;
			std::int32_t              hostileCount{ 0 };
			RE::EffectSetting*        avEffectSetting{ nullptr };
			std::vector<EffectValues> effects;  // one per array position
		};

		struct SoundValue
		{
			RE::MagicSystem::SoundID     id{};
			RE::BGSSoundDescriptorForm*  sound{ nullptr };
		};

		struct EffectState
		{
			RE::EffectSetting::EffectSettingData data{};
			std::vector<RE::EffectSetting*> counterEffects;
			std::vector<SoundValue>         sounds;
			std::string                     description;
			std::string                     fullName;
			RE::TESBoundObject*             menuDispObject{ nullptr };
			std::vector<RE::BGSKeyword*>    keywords;
			Origin                          origin;
		};

		// What a form held before this process first wrote it. Conditions are
		// kept by value here: in memory a condition is plain data whose form
		// parameters stay valid for the whole process.
		struct SpellPristine
		{
			SpellState                 state;
			std::vector<ConditionList> conditions;  // one per array position
		};

		struct EffectPristine
		{
			EffectState   state;
			ConditionList conditions;
		};

		std::unordered_map<RE::SpellItem*, SpellPristine>      g_spellPristine;
		std::unordered_map<RE::EffectSetting*, EffectPristine> g_effectPristine;

		// --- load order probe -------------------------------------------------

		using Clock = std::chrono::steady_clock;

		bool              g_loading = false;
		Clock::time_point g_loadStart{};
		std::uint32_t     g_loadCalls = 0;
		std::uint32_t     g_finishCalls = 0;
		std::uint32_t     g_slotEffects = 0;
		bool              g_appliedThisLoad = false;

		double SinceLoadStart()
		{
			return std::chrono::duration<double, std::milli>(Clock::now() - g_loadStart).count();
		}

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

		ConditionList ToList(const RE::TESCondition& a_cond)
		{
			ConditionList list;
			for (const RE::TESConditionItem* item = a_cond.head; item; item = item->next) {
				list.push_back(item->data);
			}
			return list;
		}

		// Replaces a_dst with new nodes holding a_list, then frees the old chain.
		void SetConditions(RE::TESCondition& a_dst, const ConditionList& a_list)
		{
			RE::TESConditionItem*  head = nullptr;
			RE::TESConditionItem** tail = &head;
			for (const ConditionData& data : a_list) {
				auto* copy = new RE::TESConditionItem();
				copy->data = data;
				copy->next = nullptr;
				*tail = copy;
				tail = &copy->next;
			}

			RE::TESConditionItem* old = a_dst.head;
			a_dst.head = head;
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

		std::vector<RE::BGSKeyword*> ToList(const RE::BGSKeywordForm& a_form)
		{
			std::vector<RE::BGSKeyword*> list;
			if (a_form.keywords) {
				for (std::uint32_t i = 0; i < a_form.numKeywords; ++i) {
					list.push_back(a_form.keywords[i]);
				}
			}
			return list;
		}

		// Same allocator rules as CopyKeywords.
		void SetKeywords(RE::BGSKeywordForm& a_dst, const std::vector<RE::BGSKeyword*>& a_list)
		{
			RE::BGSKeyword** old = a_dst.keywords;
			RE::BGSKeyword** fresh = nullptr;
			const auto       count = static_cast<std::uint32_t>(a_list.size());
			if (count > 0) {
				fresh = RE::calloc<RE::BGSKeyword*>(count);
				for (std::uint32_t i = 0; i < count; ++i) {
					fresh[i] = a_list[i];
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

		// --- origins --------------------------------------------------------

		Origin OriginOfEntry(RE::SpellItem* a_spell, std::size_t a_index, RE::Effect* a_entry)
		{
			if (a_entry) {
				if (const auto it = g_entryOrigin.find(a_entry); it != g_entryOrigin.end()) {
					return it->second;
				}
			}
			return Origin{ OriginKind::kSpellEntry, a_spell, static_cast<std::int32_t>(a_index) };
		}

		Origin OriginOfEffect(RE::EffectSetting* a_effect)
		{
			if (const auto it = g_effectOrigin.find(a_effect); it != g_effectOrigin.end()) {
				return it->second;
			}
			return Origin{ OriginKind::kMagicEffect, a_effect, -1 };
		}

		ConditionList ResolveConditions(const Origin& a_origin)
		{
			if (!a_origin.form) {
				return {};
			}
			if (a_origin.kind == OriginKind::kSpellEntry) {
				auto* spell = a_origin.form->As<RE::SpellItem>();
				if (!spell || a_origin.index < 0) {
					return {};
				}
				const auto index = static_cast<std::size_t>(a_origin.index);
				if (const auto it = g_spellPristine.find(spell); it != g_spellPristine.end()) {
					return index < it->second.conditions.size() ? it->second.conditions[index] : ConditionList{};
				}
				if (index < spell->effects.size() && spell->effects[static_cast<std::uint32_t>(index)]) {
					return ToList(spell->effects[static_cast<std::uint32_t>(index)]->conditions);
				}
				return {};
			}
			if (a_origin.kind == OriginKind::kMagicEffect) {
				auto* effect = a_origin.form->As<RE::EffectSetting>();
				if (!effect) {
					return {};
				}
				if (const auto it = g_effectPristine.find(effect); it != g_effectPristine.end()) {
					return it->second.conditions;
				}
				return ToList(effect->conditions);
			}
			return {};
		}

		// --- capture and apply ----------------------------------------------

		SpellState CaptureSpell(RE::SpellItem* a_spell)
		{
			SpellState s;
			s.data = a_spell->data;
			s.equipSlot = a_spell->equipSlot;
			s.menuDispObject = a_spell->menuDispObject;
			s.fullName = a_spell->fullName.c_str();
			s.keywords = ToList(static_cast<const RE::BGSKeywordForm&>(*a_spell));
			s.hostileCount = a_spell->hostileCount;
			s.avEffectSetting = a_spell->avEffectSetting;
			std::size_t index = 0;
			for (RE::Effect* entry : a_spell->effects) {
				EffectValues v;
				if (entry) {
					v.base = entry->baseEffect;
					v.magnitude = entry->effectItem.magnitude;
					v.area = entry->effectItem.area;
					v.duration = entry->effectItem.duration;
					v.cost = entry->cost;
				}
				v.origin = OriginOfEntry(a_spell, index, entry);
				s.effects.push_back(v);
				++index;
			}
			return s;
		}

		EffectState CaptureEffect(RE::EffectSetting* a_effect)
		{
			EffectState s;
			s.data = a_effect->data;
			for (RE::EffectSetting* counter : a_effect->counterEffects) {
				if (counter) {
					s.counterEffects.push_back(counter);
				}
			}
			for (const auto& pair : a_effect->effectSounds) {
				s.sounds.push_back(SoundValue{ pair.id, pair.sound });
			}
			s.description = a_effect->magicItemDescription.c_str();
			s.fullName = a_effect->fullName.c_str();
			s.menuDispObject = a_effect->menuDispObject;
			s.keywords = ToList(static_cast<const RE::BGSKeywordForm&>(*a_effect));
			s.origin = OriginOfEffect(a_effect);
			return s;
		}

		// Called before every write: the first one in this process keeps what
		// the form held, so a load can put it back.
		void EnsurePristine(RE::SpellItem* a_spell)
		{
			if (g_spellPristine.contains(a_spell)) {
				return;
			}
			SpellPristine p;
			p.state = CaptureSpell(a_spell);
			for (RE::Effect* entry : a_spell->effects) {
				p.conditions.push_back(entry ? ToList(entry->conditions) : ConditionList{});
			}
			g_spellPristine.emplace(a_spell, std::move(p));
		}

		void EnsurePristine(RE::EffectSetting* a_effect)
		{
			if (g_effectPristine.contains(a_effect)) {
				return;
			}
			EffectPristine p;
			p.state = CaptureEffect(a_effect);
			p.conditions = ToList(a_effect->conditions);
			g_effectPristine.emplace(a_effect, std::move(p));
		}

		RE::BSFixedString ToFixed(const std::string& a_text)
		{
			return a_text.empty() ? RE::BSFixedString() : RE::BSFixedString(a_text.c_str());
		}

		void ApplySpell(RE::SpellItem* a_spell, const SpellState& a_state)
		{
			EnsurePristine(a_spell);

			// Every origin is resolved before anything of this spell changes.
			std::vector<ConditionList> conditions;
			conditions.reserve(a_state.effects.size());
			for (const EffectValues& v : a_state.effects) {
				conditions.push_back(ResolveConditions(v.origin));
			}

			a_spell->data = a_state.data;
			a_spell->equipSlot = a_state.equipSlot;
			a_spell->menuDispObject = a_state.menuDispObject;
			a_spell->fullName = ToFixed(a_state.fullName);
			SetKeywords(*a_spell, a_state.keywords);

			RetireEffects(a_spell, nullptr);
			for (std::size_t i = 0; i < a_state.effects.size(); ++i) {
				const EffectValues& v = a_state.effects[i];
				if (!v.base) {
					continue;
				}
				bool        reused = false;
				RE::Effect* entry = TakeEffect(a_spell, reused);
				FillEffect(entry, v.base, v.magnitude, v.area, v.duration, v.cost);
				SetConditions(entry->conditions, conditions[i]);
				g_entryOrigin[entry] = v.origin;
				a_spell->effects.push_back(entry);
			}
			a_spell->hostileCount = a_state.hostileCount;
			a_spell->avEffectSetting = a_state.avEffectSetting;
		}

		void ApplyEffect(RE::EffectSetting* a_effect, const EffectState& a_state)
		{
			EnsurePristine(a_effect);
			const ConditionList conditions = ResolveConditions(a_state.origin);

			a_effect->data = a_state.data;

			RE::BSSimpleList<RE::EffectSetting*> counters;
			for (auto it = a_state.counterEffects.rbegin(); it != a_state.counterEffects.rend(); ++it) {
				counters.push_front(*it);
			}
			a_effect->counterEffects = counters;

			a_effect->effectSounds.clear();
			for (const SoundValue& v : a_state.sounds) {
				decltype(a_effect->effectSounds)::value_type pair{};
				pair.id = v.id;
				pair.sound = v.sound;
				a_effect->effectSounds.push_back(pair);
			}

			a_effect->magicItemDescription = ToFixed(a_state.description);
			a_effect->fullName = ToFixed(a_state.fullName);
			a_effect->menuDispObject = a_state.menuDispObject;
			SetKeywords(*a_effect, a_state.keywords);
			SetConditions(a_effect->conditions, conditions);
			g_effectOrigin[a_effect] = a_state.origin;
		}

		// --- the record: one field list for both directions -----------------
		//
		// Layout of record 'MWST' version 1, little endian:
		//   u32 fileCount, then fileCount strings      (plugin file names)
		//   u32 spellCount, then per spell:  form target, spell fields
		//   u32 effectCount, then per effect: form target, effect fields
		// string = u32 length + bytes. form = u8 kind:
		//   0 none
		//   1 plugin form: u16 file index + u32 local FormID
		//   2 created form: u32 FormID as saved (resolvable only by the cosave
		//     load callback, never at pre-load)
		// The field order is VisitSpell / VisitEffect below; the writer and the
		// reader both walk it, so the two cannot drift apart.

		template <class IO>
		void VisitOrigin(IO& a_io, Origin& a_origin)
		{
			a_io.en(a_origin.kind);
			a_io.form(a_origin.form);
			a_io.val(a_origin.index);
		}

		template <class IO>
		void VisitSpell(IO& a_io, SpellState& a_s)
		{
			auto& d = a_s.data;
			a_io.val(d.costOverride);
			a_io.flags(d.flags);
			a_io.en(d.spellType);
			a_io.val(d.chargeTime);
			a_io.en(d.castingType);
			a_io.en(d.delivery);
			a_io.val(d.castDuration);
			a_io.val(d.range);
			a_io.form(d.castingPerk);

			a_io.form(a_s.equipSlot);
			a_io.form(a_s.menuDispObject);
			a_io.str(a_s.fullName);
			a_io.list(a_s.keywords, [&](RE::BGSKeyword*& a_k) { a_io.form(a_k); });
			a_io.val(a_s.hostileCount);
			a_io.form(a_s.avEffectSetting);
			a_io.list(a_s.effects, [&](EffectValues& a_v) {
				a_io.form(a_v.base);
				a_io.val(a_v.magnitude);
				a_io.val(a_v.area);
				a_io.val(a_v.duration);
				a_io.val(a_v.cost);
				VisitOrigin(a_io, a_v.origin);
			});
		}

		template <class IO>
		void VisitEffect(IO& a_io, EffectState& a_s)
		{
			auto& d = a_s.data;
			a_io.flags(d.flags);
			a_io.val(d.baseCost);
			a_io.form(d.associatedForm);
			a_io.en(d.associatedSkill);
			a_io.en(d.resistVariable);
			a_io.val(d.numCounterEffects);
			a_io.form(d.light);
			a_io.val(d.taperWeight);
			a_io.form(d.effectShader);
			a_io.form(d.enchantShader);
			a_io.val(d.minimumSkill);
			a_io.val(d.spellmakingArea);
			a_io.val(d.spellmakingChargeTime);
			a_io.val(d.taperCurve);
			a_io.val(d.taperDuration);
			a_io.val(d.secondAVWeight);
			a_io.en(d.archetype);
			a_io.en(d.primaryAV);
			a_io.form(d.projectileBase);
			a_io.form(d.explosion);
			a_io.en(d.castingType);
			a_io.en(d.delivery);
			a_io.en(d.secondaryAV);
			a_io.form(d.castingArt);
			a_io.form(d.hitEffectArt);
			a_io.form(d.impactDataSet);
			a_io.val(d.skillUsageMult);
			a_io.form(d.dualCastData);
			a_io.val(d.dualCastScale);
			a_io.form(d.enchantEffectArt);
			a_io.form(d.hitVisuals);
			a_io.form(d.enchantVisuals);
			a_io.form(d.equipAbility);
			a_io.form(d.imageSpaceMod);
			a_io.form(d.perk);
			a_io.en(d.castingSoundLevel);
			a_io.val(d.aiScore);
			a_io.val(d.aiDelayTimer);

			a_io.list(a_s.counterEffects, [&](RE::EffectSetting*& a_c) { a_io.form(a_c); });
			a_io.list(a_s.sounds, [&](SoundValue& a_v) {
				a_io.en(a_v.id);
				a_io.form(a_v.sound);
			});
			a_io.str(a_s.description);
			a_io.str(a_s.fullName);
			a_io.form(a_s.menuDispObject);
			a_io.list(a_s.keywords, [&](RE::BGSKeyword*& a_k) { a_io.form(a_k); });
			VisitOrigin(a_io, a_s.origin);
		}

		class Writer
		{
		public:
			template <class T>
			void val(T& a_value)
			{
				static_assert(std::is_trivially_copyable_v<T>);
				const auto* p = reinterpret_cast<const std::uint8_t*>(&a_value);
				_body.insert(_body.end(), p, p + sizeof(T));
			}

			template <class E>
			void en(E& a_value)
			{
				auto raw = static_cast<std::int32_t>(a_value);
				val(raw);
			}

			template <class E, class U>
			void flags(REX::EnumSet<E, U>& a_value)
			{
				U raw = a_value.underlying();
				val(raw);
			}

			void str(std::string& a_text)
			{
				auto length = static_cast<std::uint32_t>(a_text.size());
				val(length);
				_body.insert(_body.end(), a_text.begin(), a_text.end());
			}

			template <class T>
			void form(T*& a_form)
			{
				const RE::TESForm* form = a_form;
				std::uint8_t       kind = 0;
				if (!form) {
					val(kind);
					return;
				}
				const RE::TESFile* file = form->GetFile(0);
				if (file && (form->GetFormID() >> 24) != 0xFF) {
					kind = 1;
					val(kind);
					auto index = FileIndex(file->GetFilename());
					val(index);
					auto local = form->GetLocalFormID();
					val(local);
					return;
				}
				kind = 2;
				val(kind);
				auto id = form->GetFormID();
				val(id);
			}

			template <class T, class F>
			void list(std::vector<T>& a_list, F&& a_visit)
			{
				auto count = static_cast<std::uint32_t>(a_list.size());
				val(count);
				for (T& item : a_list) {
					a_visit(item);
				}
			}

			std::vector<std::uint8_t> Finish() const
			{
				std::vector<std::uint8_t> out;
				auto put = [&](const void* a_p, std::size_t a_n) {
					const auto* p = static_cast<const std::uint8_t*>(a_p);
					out.insert(out.end(), p, p + a_n);
				};
				const auto fileCount = static_cast<std::uint32_t>(_files.size());
				put(&fileCount, sizeof(fileCount));
				for (const std::string& name : _files) {
					const auto length = static_cast<std::uint32_t>(name.size());
					put(&length, sizeof(length));
					put(name.data(), name.size());
				}
				out.insert(out.end(), _body.begin(), _body.end());
				return out;
			}

		private:
			std::uint16_t FileIndex(std::string_view a_name)
			{
				std::string name{ a_name };
				if (const auto it = _fileIndex.find(name); it != _fileIndex.end()) {
					return it->second;
				}
				const auto index = static_cast<std::uint16_t>(_files.size());
				_files.push_back(name);
				_fileIndex.emplace(std::move(name), index);
				return index;
			}

			std::vector<std::uint8_t>                      _body;
			std::vector<std::string>                       _files;
			std::unordered_map<std::string, std::uint16_t> _fileIndex;
		};

		class Reader
		{
		public:
			Reader(const std::uint8_t* a_data, std::size_t a_size, const SKSE::SerializationInterface* a_intfc) :
				_data(a_data), _size(a_size), _intfc(a_intfc)
			{}

			bool ok{ true };        // false: the bytes are not a valid record
			bool missing{ false };  // a form this item names does not resolve

			bool ReadFileTable()
			{
				std::uint32_t count = 0;
				val(count);
				if (!ok || count > kMaxList) {
					ok = false;
					return false;
				}
				_files.resize(count);
				for (std::string& name : _files) {
					str(name);
				}
				return ok;
			}

			bool AtEnd() const { return _pos == _size; }

			template <class T>
			void val(T& a_value)
			{
				static_assert(std::is_trivially_copyable_v<T>);
				if (!ok || _size - _pos < sizeof(T)) {
					ok = false;
					return;
				}
				std::memcpy(&a_value, _data + _pos, sizeof(T));
				_pos += sizeof(T);
			}

			template <class E>
			void en(E& a_value)
			{
				std::int32_t raw = 0;
				val(raw);
				a_value = static_cast<E>(raw);
			}

			template <class E, class U>
			void flags(REX::EnumSet<E, U>& a_value)
			{
				U raw = 0;
				val(raw);
				a_value = static_cast<E>(raw);
			}

			void str(std::string& a_text)
			{
				std::uint32_t length = 0;
				val(length);
				if (!ok || length > kMaxString || _size - _pos < length) {
					ok = false;
					return;
				}
				a_text.assign(reinterpret_cast<const char*>(_data + _pos), length);
				_pos += length;
			}

			template <class T>
			void form(T*& a_form)
			{
				a_form = nullptr;
				std::uint8_t kind = 0;
				val(kind);
				if (!ok || kind == 0) {
					return;
				}

				RE::TESForm* form = nullptr;
				if (kind == 1) {
					std::uint16_t index = 0;
					RE::FormID    local = 0;
					val(index);
					val(local);
					if (!ok || index >= _files.size()) {
						ok = false;
						return;
					}
					if (auto* handler = RE::TESDataHandler::GetSingleton()) {
						form = handler->LookupForm(local, _files[index]);
					}
				} else if (kind == 2) {
					RE::FormID saved = 0;
					val(saved);
					if (!ok) {
						return;
					}
					RE::FormID current = 0;
					if (_intfc && _intfc->ResolveFormID(saved, current)) {
						form = RE::TESForm::LookupByID(current);
					}
				} else {
					ok = false;
					return;
				}

				if (!form) {
					missing = true;
					return;
				}
				if constexpr (std::is_same_v<T, RE::TESForm>) {
					a_form = form;
				} else {
					a_form = form->As<T>();
					if (!a_form) {
						missing = true;
					}
				}
			}

			template <class T, class F>
			void list(std::vector<T>& a_list, F&& a_visit)
			{
				std::uint32_t count = 0;
				val(count);
				if (!ok || count > kMaxList) {
					ok = false;
					return;
				}
				a_list.resize(count);
				for (T& item : a_list) {
					a_visit(item);
					if (!ok) {
						return;
					}
				}
			}

		private:
			const std::uint8_t*                 _data;
			std::size_t                         _size;
			std::size_t                         _pos{ 0 };
			const SKSE::SerializationInterface* _intfc;
			std::vector<std::string>            _files;
		};

		std::vector<std::uint8_t> BuildRecord(std::uint32_t& a_spells, std::uint32_t& a_effects)
		{
			Writer w;

			std::vector<RE::SpellItem*> spells;
			for (const auto& [spell, pristine] : g_spellPristine) {
				spells.push_back(spell);
			}
			auto spellCount = static_cast<std::uint32_t>(spells.size());
			w.val(spellCount);
			for (RE::SpellItem* spell : spells) {
				w.form(spell);
				SpellState state = CaptureSpell(spell);
				VisitSpell(w, state);
			}

			std::vector<RE::EffectSetting*> effects;
			for (const auto& [effect, pristine] : g_effectPristine) {
				effects.push_back(effect);
			}
			auto effectCount = static_cast<std::uint32_t>(effects.size());
			w.val(effectCount);
			for (RE::EffectSetting* effect : effects) {
				w.form(effect);
				EffectState state = CaptureEffect(effect);
				VisitEffect(w, state);
			}

			a_spells = spellCount;
			a_effects = effectCount;
			return w.Finish();
		}

		struct ApplyResult
		{
			bool          parsed{ false };
			std::uint32_t spells{ 0 };
			std::uint32_t effects{ 0 };
			std::uint32_t skipped{ 0 };   // named in the record, did not resolve
			std::uint32_t reverted{ 0 };  // touched here, not in the record
		};

		// All or nothing on a parse error: nothing is written unless the whole
		// record reads. A single item whose forms do not resolve (plugin gone,
		// created form at pre-load) is skipped and counted.
		ApplyResult ApplyRecord(const std::vector<std::uint8_t>& a_bytes, const SKSE::SerializationInterface* a_intfc)
		{
			ApplyResult result;
			Reader      r(a_bytes.data(), a_bytes.size(), a_intfc);
			if (!r.ReadFileTable()) {
				return result;
			}

			std::vector<std::pair<RE::SpellItem*, SpellState>>      spells;
			std::vector<std::pair<RE::EffectSetting*, EffectState>> effects;

			std::uint32_t spellCount = 0;
			r.val(spellCount);
			if (!r.ok || spellCount > kMaxList) {
				return result;
			}
			for (std::uint32_t i = 0; i < spellCount && r.ok; ++i) {
				r.missing = false;
				RE::SpellItem* target = nullptr;
				SpellState     state;
				r.form(target);
				VisitSpell(r, state);
				if (!r.ok) {
					break;
				}
				if (r.missing || !target) {
					++result.skipped;
					continue;
				}
				spells.emplace_back(target, std::move(state));
			}

			std::uint32_t effectCount = 0;
			r.val(effectCount);
			if (!r.ok || effectCount > kMaxList) {
				return result;
			}
			for (std::uint32_t i = 0; i < effectCount && r.ok; ++i) {
				r.missing = false;
				RE::EffectSetting* target = nullptr;
				EffectState        state;
				r.form(target);
				VisitEffect(r, state);
				if (!r.ok) {
					break;
				}
				if (r.missing || !target) {
					++result.skipped;
					continue;
				}
				effects.emplace_back(target, std::move(state));
			}
			if (!r.ok || !r.AtEnd()) {
				return result;
			}
			result.parsed = true;

			// Forms written in this process that this save does not list go
			// back to what they held before the first write. Keys first: the
			// apply below may add to the maps.
			std::unordered_set<RE::SpellItem*>     listedSpells;
			std::unordered_set<RE::EffectSetting*> listedEffects;
			for (const auto& item : spells) {
				listedSpells.insert(item.first);
			}
			for (const auto& item : effects) {
				listedEffects.insert(item.first);
			}

			std::vector<RE::SpellItem*> revertSpells;
			for (const auto& [spell, pristine] : g_spellPristine) {
				if (!listedSpells.contains(spell)) {
					revertSpells.push_back(spell);
				}
			}
			std::vector<RE::EffectSetting*> revertEffects;
			for (const auto& [effect, pristine] : g_effectPristine) {
				if (!listedEffects.contains(effect)) {
					revertEffects.push_back(effect);
				}
			}
			for (RE::SpellItem* spell : revertSpells) {
				const SpellState pristine = g_spellPristine.at(spell).state;
				ApplySpell(spell, pristine);
				++result.reverted;
			}
			for (RE::EffectSetting* effect : revertEffects) {
				const EffectState pristine = g_effectPristine.at(effect).state;
				ApplyEffect(effect, pristine);
				++result.reverted;
			}

			for (const auto& [effect, state] : effects) {
				ApplyEffect(effect, state);
				++result.effects;
			}
			for (const auto& [spell, state] : spells) {
				ApplySpell(spell, state);
				++result.spells;
			}
			return result;
		}

		// --- reading the record before the engine loads -----------------------

		// The .skse file SKSE writes next to the save: the same path SKSE's
		// Serialization builds (My Games folder, then sLocalSavePath or Saves,
		// then the save name without .ess). The My Games folder is taken from
		// the SKSE log folder, which CommonLib resolves per runtime.
		std::optional<std::filesystem::path> CosavePath(std::string a_saveName)
		{
			if (a_saveName.size() >= 4 && _stricmp(a_saveName.c_str() + a_saveName.size() - 4, ".ess") == 0) {
				a_saveName.resize(a_saveName.size() - 4);
			}
			if (a_saveName.empty()) {
				return std::nullopt;
			}

			auto logs = SKSE::log::log_directory();
			if (!logs) {
				return std::nullopt;
			}
			std::filesystem::path root = *logs;
			if (!root.has_filename()) {
				root = root.parent_path();
			}
			root = root.parent_path();

			std::string savesFolder = "Saves\\";
			if (auto* ini = RE::INISettingCollection::GetSingleton()) {
				if (auto* setting = ini->GetSetting("sLocalSavePath:General"); setting && setting->GetType() == RE::Setting::Type::kString && setting->data.s) {
					savesFolder = setting->data.s;
				}
			}
			return root / savesFolder / (a_saveName + ".skse");
		}

		enum class PreReadStatus
		{
			kFound,
			kNoPath,
			kNoFile,
			kBadFile,
			kNoPlugin,
			kNoRecord,
			kNewerRecord,
		};

		const char* ToString(PreReadStatus a_status)
		{
			switch (a_status) {
			case PreReadStatus::kFound:
				return "record found";
			case PreReadStatus::kNoPath:
				return "no cosave path";
			case PreReadStatus::kNoFile:
				return "no cosave file";
			case PreReadStatus::kBadFile:
				return "cosave unreadable";
			case PreReadStatus::kNoPlugin:
				return "cosave has no Lodestone data";
			case PreReadStatus::kNoRecord:
				return "cosave has no magic write record (save made before 1.41.1)";
			case PreReadStatus::kNewerRecord:
				return "magic write record of a newer version";
			}
			return "?";
		}

		// SKSE cosave layout (format version 1): header {u32 signature 'SKSE',
		// u32 formatVersion, u32 skseVersion, u32 runtimeVersion, u32
		// numPlugins}, then per plugin {u32 uid, u32 numChunks, u32 length},
		// then per chunk {u32 type, u32 version, u32 length} and its data.
		PreReadStatus ReadRecordFromCosave(const std::filesystem::path& a_path, std::vector<std::uint8_t>& a_out)
		{
			std::error_code ec;
			if (!std::filesystem::exists(a_path, ec)) {
				return PreReadStatus::kNoFile;
			}
			std::ifstream file(a_path, std::ios::binary);
			if (!file) {
				return PreReadStatus::kBadFile;
			}

			auto read32 = [&](std::uint32_t& a_value) {
				return static_cast<bool>(file.read(reinterpret_cast<char*>(&a_value), sizeof(a_value)));
			};

			char          signature[4]{};
			std::uint32_t formatVersion = 0, skseVersion = 0, runtimeVersion = 0, numPlugins = 0;
			if (!file.read(signature, 4) || std::memcmp(signature, "SKSE", 4) != 0 ||
				!read32(formatVersion) || formatVersion != 1 ||
				!read32(skseVersion) || !read32(runtimeVersion) || !read32(numPlugins)) {
				return PreReadStatus::kBadFile;
			}

			for (std::uint32_t p = 0; p < numPlugins; ++p) {
				std::uint32_t uid = 0, numChunks = 0, length = 0;
				if (!read32(uid) || !read32(numChunks) || !read32(length)) {
					return PreReadStatus::kBadFile;
				}
				if (uid != kPluginID) {
					file.seekg(length, std::ios::cur);
					if (!file) {
						return PreReadStatus::kBadFile;
					}
					continue;
				}
				for (std::uint32_t c = 0; c < numChunks; ++c) {
					std::uint32_t type = 0, version = 0, size = 0;
					if (!read32(type) || !read32(version) || !read32(size)) {
						return PreReadStatus::kBadFile;
					}
					if (type != kRecordType) {
						file.seekg(size, std::ios::cur);
						if (!file) {
							return PreReadStatus::kBadFile;
						}
						continue;
					}
					if (version > kRecordVersion) {
						return PreReadStatus::kNewerRecord;
					}
					a_out.resize(size);
					if (size > 0 && !file.read(reinterpret_cast<char*>(a_out.data()), size)) {
						return PreReadStatus::kBadFile;
					}
					return PreReadStatus::kFound;
				}
				return PreReadStatus::kNoRecord;
			}
			return PreReadStatus::kNoPlugin;
		}

		void LogApply(const char* a_when, const ApplyResult& a_result)
		{
			if (!a_result.parsed) {
				spdlog::error("MagicWrite: {}: the magic write record did not read; nothing was restored.", a_when);
				return;
			}
			if (a_result.spells + a_result.effects + a_result.skipped + a_result.reverted == 0 && !g_log) {
				return;
			}
			spdlog::info("MagicWrite: {}: restored {} spell(s) and {} magic effect(s) from the save, {} back to the "
						 "plugin state, {} skipped (form not found).",
				a_when, a_result.spells, a_result.effects, a_result.reverted, a_result.skipped);
		}

		void OnPreLoadGame(const SKSE::MessagingInterface::Message* a_msg)
		{
			g_loadStart = Clock::now();
			g_loading = true;
			g_loadCalls = 0;
			g_finishCalls = 0;
			g_slotEffects = 0;
			g_appliedThisLoad = false;

			std::string name;
			if (a_msg->data && a_msg->dataLen > 0) {
				name.assign(static_cast<const char*>(a_msg->data), a_msg->dataLen);
			}
			if (g_log) {
				spdlog::info("MagicWrite: load order - kPreLoadGame '{}' at +0.0 ms.", name);
			}

			const auto path = CosavePath(name);
			std::vector<std::uint8_t> bytes;
			const PreReadStatus status = path ? ReadRecordFromCosave(*path, bytes) : PreReadStatus::kNoPath;

			if (status == PreReadStatus::kFound) {
				const ApplyResult result = ApplyRecord(bytes, nullptr);
				g_appliedThisLoad = result.parsed;
				LogApply("pre-load", result);
			} else if (status == PreReadStatus::kBadFile || status == PreReadStatus::kNewerRecord) {
				spdlog::warn("MagicWrite: pre-load: {} ({}); the cosave load callback will try again.",
					ToString(status), path ? path->string() : std::string{});
			} else if (g_log) {
				spdlog::info("MagicWrite: pre-load: {} ({}).", ToString(status), path ? path->string() : std::string{});
			}

			if (g_log) {
				spdlog::info("MagicWrite: load order - pre-load restore done at +{:.1f} ms ({} byte record, {} spell(s) "
							 "and {} magic effect(s) touched this process).",
					SinceLoadStart(), bytes.size(), g_spellPristine.size(), g_effectPristine.size());
			}
		}

		// --- probe hooks: ActiveEffect::LoadGame (0x09), FinishLoadGame (0x0A)

		struct EffectType
		{
			REL::VariantID vtable;
			const char*    name;
			std::uintptr_t address{ 0 };
			std::uintptr_t original{ 0 };
		};

		// The same eight classes MagicScaling swaps at 0x00. Slot indices from
		// ActiveEffect.h:72-74. ScriptEffect overrides LoadGame; the others
		// inherit both, and each table resolves its own original.
		EffectType g_loadTypes[] = {
			{ RE::VTABLE_ActiveEffect[0], "ActiveEffect" },
			{ RE::VTABLE_ValueModifierEffect[0], "ValueModifierEffect" },
			{ RE::VTABLE_DualValueModifierEffect[0], "DualValueModifierEffect" },
			{ RE::VTABLE_PeakValueModifierEffect[0], "PeakValueModifierEffect" },
			{ RE::VTABLE_AccumulatingValueModifierEffect[0], "AccumulatingValueModifierEffect" },
			{ RE::VTABLE_TargetValueModifierEffect[0], "TargetValueModifierEffect" },
			{ RE::VTABLE_ValueAndConditionsEffect[0], "ValueAndConditionsEffect" },
			{ RE::VTABLE_ScriptEffect[0], "ScriptEffect" },
		};
		EffectType g_finishTypes[] = {
			{ RE::VTABLE_ActiveEffect[0], "ActiveEffect" },
			{ RE::VTABLE_ValueModifierEffect[0], "ValueModifierEffect" },
			{ RE::VTABLE_DualValueModifierEffect[0], "DualValueModifierEffect" },
			{ RE::VTABLE_PeakValueModifierEffect[0], "PeakValueModifierEffect" },
			{ RE::VTABLE_AccumulatingValueModifierEffect[0], "AccumulatingValueModifierEffect" },
			{ RE::VTABLE_TargetValueModifierEffect[0], "TargetValueModifierEffect" },
			{ RE::VTABLE_ValueAndConditionsEffect[0], "ValueAndConditionsEffect" },
			{ RE::VTABLE_ScriptEffect[0], "ScriptEffect" },
		};
		constexpr std::size_t kLoadGameIndex = 0x09;
		constexpr std::size_t kFinishLoadGameIndex = 0x0A;

		template <std::size_t N>
		const EffectType* FindType(const EffectType (&a_table)[N], const void* a_object)
		{
			const auto vtable = *reinterpret_cast<const std::uintptr_t*>(a_object);
			for (const EffectType& type : a_table) {
				if (type.address == vtable) {
					return &type;
				}
			}
			return nullptr;
		}

		// Name of the class of an effect, from any of the tables (they hold the
		// same addresses once installed).
		const char* ClassName(const RE::ActiveEffect* a_effect)
		{
			const EffectType* type = FindType(g_finishTypes, a_effect);
			return type ? type->name : "other";
		}

		std::string DescribeEffect(RE::ActiveEffect* a_effect)
		{
			auto* spell = a_effect->spell ? a_effect->spell->As<RE::SpellItem>() : nullptr;
			std::int32_t index = -1;
			if (spell) {
				for (std::uint32_t i = 0; i < spell->effects.size(); ++i) {
					if (spell->effects[i] == a_effect->effect) {
						index = static_cast<std::int32_t>(i);
						break;
					}
				}
			}
			RE::EffectSetting* base = a_effect->effect ? a_effect->effect->baseEffect : nullptr;
			auto*              player = RE::PlayerCharacter::GetSingleton();
			const bool         onPlayer = player && a_effect->target == player->AsMagicTarget();
			return std::format("class {} spell {:08X} entry {} base {:08X} archetype {} av {} magnitude {:.2f} "
							   "elapsed {:.1f}/{:.1f} {}",
				ClassName(a_effect), spell ? spell->GetFormID() : 0, index, base ? base->GetFormID() : 0,
				base ? static_cast<std::int32_t>(base->data.archetype) : -2,
				base ? static_cast<std::int32_t>(base->data.primaryAV) : -2,
				a_effect->magnitude, a_effect->elapsedSeconds, a_effect->duration, onPlayer ? "player" : "other actor");
		}

		bool IsWrittenSpell(const RE::MagicItem* a_item)
		{
			auto* spell = a_item ? const_cast<RE::MagicItem*>(a_item)->As<RE::SpellItem>() : nullptr;
			return spell && g_spellPristine.contains(spell);
		}

		void LoadGameThunk(RE::ActiveEffect* a_this, RE::BGSLoadFormBuffer* a_buf)
		{
			const EffectType* type = FindType(g_loadTypes, a_this);
			if (g_loading && g_loadCalls++ == 0) {
				spdlog::info("MagicWrite: load order - first ActiveEffect::LoadGame ({}) at +{:.1f} ms.",
					type ? type->name : "?", SinceLoadStart());
			}
			if (type && type->original) {
				reinterpret_cast<void (*)(RE::ActiveEffect*, RE::BGSLoadFormBuffer*)>(type->original)(a_this, a_buf);
			}
		}

		void FinishLoadGameThunk(RE::ActiveEffect* a_this, RE::BGSLoadFormBuffer* a_buf)
		{
			const EffectType* type = FindType(g_finishTypes, a_this);
			if (type && type->original) {
				reinterpret_cast<void (*)(RE::ActiveEffect*, RE::BGSLoadFormBuffer*)>(type->original)(a_this, a_buf);
			}
			if (!g_loading) {
				return;
			}
			if (g_finishCalls++ == 0) {
				spdlog::info("MagicWrite: load order - first ActiveEffect::FinishLoadGame ({}) at +{:.1f} ms.",
					type ? type->name : "?", SinceLoadStart());
			}
			try {
				if (IsWrittenSpell(a_this->spell)) {
					++g_slotEffects;
					spdlog::info("MagicWrite: load order - loaded effect of a written spell at +{:.1f} ms: {}.",
						SinceLoadStart(), DescribeEffect(a_this));
				}
			} catch (...) {
			}
		}

		template <std::size_t N>
		void InstallVfunc(EffectType (&a_table)[N], std::size_t a_index, void* a_thunk)
		{
			for (EffectType& type : a_table) {
				REL::Relocation<std::uintptr_t> vtbl{ type.vtable };
				type.address = vtbl.address();
				type.original = *reinterpret_cast<std::uintptr_t*>(type.address + (a_index * sizeof(std::uintptr_t)));
			}
			for (EffectType& type : a_table) {
				REL::Relocation<std::uintptr_t> vtbl{ type.vtable };
				vtbl.write_vfunc(a_index, reinterpret_cast<std::uintptr_t>(a_thunk));
			}
		}

		void InstallProbe()
		{
			InstallVfunc(g_loadTypes, kLoadGameIndex, reinterpret_cast<void*>(&LoadGameThunk));
			InstallVfunc(g_finishTypes, kFinishLoadGameIndex, reinterpret_cast<void*>(&FinishLoadGameThunk));
			spdlog::info("MagicWrite: load order probe installed (ActiveEffect::LoadGame 0x09 and FinishLoadGame 0x0A "
						 "on {} effect classes).",
				std::size(g_loadTypes));
		}

		void OnPostLoadGame()
		{
			if (g_log) {
				spdlog::info("MagicWrite: load order - kPostLoadGame at +{:.1f} ms (LoadGame {}, FinishLoadGame {}, "
							 "written-spell effects {}; record applied {}).",
					SinceLoadStart(), g_loadCalls, g_finishCalls, g_slotEffects,
					g_appliedThisLoad ? "yes" : "no");

				// What the player carries now, for the spells written here.
				if (auto* player = RE::PlayerCharacter::GetSingleton()) {
					if (auto* target = player->AsMagicTarget()) {
						std::uint32_t listed = 0;
						target->VisitActiveEffects([&](RE::ActiveEffect* a_effect) {
							if (a_effect && IsWrittenSpell(a_effect->spell)) {
								++listed;
								spdlog::info("MagicWrite: load order - post-load, on the player: {}.", DescribeEffect(a_effect));
							}
							return RE::BSContainer::ForEachResult::kContinue;
						});
						spdlog::info("MagicWrite: load order - post-load, {} active effect(s) of written spells on the player.", listed);
					}
				}
			}
			g_loading = false;
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
				EnsurePristine(a_target);

				// Origins first: the source's own entries may be looked up by
				// pointer, and nothing of the source changes below.
				std::vector<Origin> origins;
				{
					std::size_t index = 0;
					for (RE::Effect* src : a_source->effects) {
						origins.push_back(OriginOfEntry(a_source, index, src));
						++index;
					}
				}

				a_target->data = a_source->data;
				a_target->equipSlot = a_source->equipSlot;
				a_target->menuDispObject = a_source->menuDispObject;
				a_target->fullName = a_source->fullName;
				CopyKeywords(*a_target, *a_source);

				RetireEffects(a_target, &a_source->effects);
				std::uint32_t reused = 0;
				std::uint32_t fresh = 0;
				std::size_t   index = 0;
				for (const RE::Effect* src : a_source->effects) {
					const std::size_t position = index++;
					if (!src) {
						continue;
					}
					bool        wasReused = false;
					RE::Effect* effect = TakeEffect(a_target, wasReused);
					FillEffect(effect, src->baseEffect, src->effectItem.magnitude, src->effectItem.area, src->effectItem.duration, src->cost);
					CopyConditions(effect->conditions, src->conditions);
					g_entryOrigin[effect] = origins[position];
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
				EnsurePristine(a_spell);
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
				EnsurePristine(a_spell);

				bool        reused = false;
				RE::Effect* entry = TakeEffect(a_spell, reused);
				FillEffect(entry, a_effect, a_magnitude, static_cast<std::uint32_t>(a_area), static_cast<std::uint32_t>(a_duration), a_cost);
				ClearConditions(entry->conditions);
				g_entryOrigin[entry] = Origin{};
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
				EnsurePristine(a_spell);
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
				EnsurePristine(a_spell);
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
				EnsurePristine(a_target);
				const Origin origin = OriginOfEffect(a_source);

				a_target->data = a_source->data;
				a_target->counterEffects = a_source->counterEffects;
				a_target->effectSounds = a_source->effectSounds;
				a_target->magicItemDescription = a_source->magicItemDescription;
				a_target->fullName = a_source->fullName;
				a_target->menuDispObject = a_source->menuDispObject;
				CopyKeywords(*a_target, *a_source);
				CopyConditions(a_target->conditions, a_source->conditions);
				g_effectOrigin[a_target] = origin;

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
				EnsurePristine(a_target);
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

		void ReadConfig()
		{
			Config::ForEachPair([](std::string_view a_key, std::string_view a_value) {
				if (a_key == "logmagicwrite") {
					g_log = (Config::Trim(a_value) == "1");
				}
			});
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

		ReadConfig();

		spdlog::info("MagicWrite: natives registered (CopySpell, ClearSpellEffects, AddSpellEffect, SetSpellAutoCalculate, "
					 "SetSpellCostOverride, CopyMagicEffect, CopyMagicEffectAppearance). Log {}.",
			g_log ? "ON" : "off");
		return true;
	}

	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		try {
			switch (a_msg->type) {
			case SKSE::MessagingInterface::kDataLoaded:
				ReadConfig();
				if (g_log) {
					InstallProbe();
				}
				break;
			case SKSE::MessagingInterface::kPreLoadGame:
				OnPreLoadGame(a_msg);
				break;
			case SKSE::MessagingInterface::kPostLoadGame:
				OnPostLoadGame();
				break;
			default:
				break;
			}
		} catch (const std::exception& e) {
			spdlog::error("MagicWrite: SKSE message {} failed: {}.", a_msg->type, e.what());
		} catch (...) {
			spdlog::error("MagicWrite: SKSE message {} failed.", a_msg->type);
		}
	}

	void CosaveSave(SKSE::SerializationInterface* a_intfc)
	{
		try {
			std::uint32_t spells = 0;
			std::uint32_t effects = 0;
			const auto    bytes = BuildRecord(spells, effects);
			if (!a_intfc->WriteRecord(kRecordType, kRecordVersion, bytes.data(), static_cast<std::uint32_t>(bytes.size()))) {
				spdlog::error("MagicWrite: could not write the magic write record ({} bytes) to the cosave.", bytes.size());
				return;
			}
			if (g_log) {
				spdlog::info("MagicWrite: saved the state of {} spell(s) and {} magic effect(s) ({} bytes).", spells, effects, bytes.size());
			}
		} catch (...) {
			spdlog::error("MagicWrite: saving the magic write record failed.");
		}
	}

	void CosaveLoadBegin()
	{
		// Nothing to reset: the forms are restored (or left alone) at
		// kPreLoadGame, before this callback, and resetting them here would
		// undo that. This line is the cosave half of the load order probe.
		if (g_log) {
			spdlog::info("MagicWrite: load order - cosave load callback at +{:.1f} ms (LoadGame {}, FinishLoadGame {} so far).",
				SinceLoadStart(), g_loadCalls, g_finishCalls);
		}
	}

	bool CosaveLoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, std::uint32_t a_version, std::uint32_t a_length)
	{
		if (a_type != kRecordType) {
			return false;
		}
		try {
			std::vector<std::uint8_t> bytes(a_length);
			if (a_length > 0 && a_intfc->ReadRecordData(bytes.data(), a_length) != a_length) {
				spdlog::error("MagicWrite: the magic write record in the cosave is short; skipped.");
				return true;
			}
			if (a_version > kRecordVersion) {
				spdlog::warn("MagicWrite: magic write record version {} is newer than this build reads; skipped.", a_version);
				return true;
			}
			if (g_appliedThisLoad) {
				if (g_log) {
					spdlog::info("MagicWrite: cosave load callback: record already applied at pre-load ({} bytes).", a_length);
				}
				return true;
			}
			// The pre-load read failed. Late is better than not at all, and
			// the log says it was late.
			const ApplyResult result = ApplyRecord(bytes, a_intfc);
			g_appliedThisLoad = result.parsed;
			LogApply("LATE, in the cosave load callback", result);
		} catch (...) {
			spdlog::error("MagicWrite: reading the magic write record failed.");
		}
		return true;
	}
}
