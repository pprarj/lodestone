// EquipVeto.cpp
// Lodestone - Shared SKSE framework
//
// Implementation of the equip veto. Read EquipVeto.h first - it carries what
// was measured and why the shape is what it is.
//
// Phase: equip veto, 1.16.0.

#include "EquipVeto.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>
#include <safetyhook.hpp>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Lodestone::Core::EquipVeto
{
	namespace
	{
		constexpr auto kConfigPath = "Data/SKSE/Plugins/Lodestone.ini"sv;

		// How often the running totals get a line. Short enough to show a rate,
		// long enough not to become the load it is measuring.
		constexpr std::int64_t kSummaryIntervalMs{ 5000 };

		// This module's cosave record tag. The plugin's cosave id ('LDST')
		// belongs to Core/Serialization, which owns the callbacks.
		constexpr std::uint32_t kRecordType = 'EVQ1';
		constexpr std::uint32_t kRecordVersion = 1;

		// -------------------------------------------------------------------
		// THE REGISTRY - what a consumer drives.
		//
		// Key is the pair (actor, item) packed into 64 bits; the value is what
		// to equip instead, plus who asked.
		//
		// SYNCHRONISATION IS NOT OPTIONAL, and that is a measurement rather
		// than caution: this module's own log lines carry several different
		// thread ids. The hook reads under a shared lock; the natives write
		// under an exclusive one.
		//
		// THE ATOMIC IN FRONT OF THE LOCK is the pattern Core/SpellTomes
		// established - with nobody registered the hot path is one relaxed
		// load and nothing else.
		// -------------------------------------------------------------------
		struct BlockEntry
		{
			RE::TESBoundObject* substitute;  // null = plain refusal
			RE::FormID          owner;
		};

		std::shared_mutex                             g_registryLock;
		std::unordered_map<std::uint64_t, BlockEntry> g_registry;
		std::atomic<bool>                             g_registryHasEntries{ false };

		constexpr std::uint64_t MakeKey(RE::FormID a_actor, RE::FormID a_item)
		{
			return (static_cast<std::uint64_t>(a_actor) << 32) | static_cast<std::uint64_t>(a_item);
		}

		// Carries the decision from the top of the thunk to the refusal path a
		// few lines below, in the same call. THREAD LOCAL because the thunk
		// runs on several threads at once - a shared one would let one thread's
		// substitute be equipped on another thread's actor.
		thread_local RE::TESBoundObject* g_pendingSubstitute{ nullptr };

		// --- diagnostics, none of which changes what happens in game --------
		bool                       g_logEquipObject{ true };
		std::unordered_set<RE::FormID> g_watchActors;
		std::vector<RE::TESFaction*>   g_watchFactions;

		SafetyHookInline g_hook{};

		std::atomic<std::uint64_t> g_calls{ 0 };
		std::atomic<std::uint64_t> g_watched{ 0 };
		std::atomic<std::uint64_t> g_refusals{ 0 };
		std::atomic<std::uint64_t> g_substitutions{ 0 };
		std::atomic<std::uint64_t> g_redundantSwapsAvoided{ 0 };
		std::atomic<std::int64_t>  g_lastSummaryMs{ 0 };

		// Only touched by whichever thread wins the summary exchange.
		std::uint64_t g_callsAtLastSummary{ 0 };
		std::uint64_t g_watchedAtLastSummary{ 0 };

		std::mutex                                       g_statsLock;
		std::unordered_map<std::uint64_t, std::uint64_t> g_refusalsByPair;

		// -------------------------------------------------------------------
		// Small helpers
		// -------------------------------------------------------------------
		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
				.count();
		}

		std::string Trim(std::string_view a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_text.find_last_not_of(" \t\r\n");
			return std::string{ a_text.substr(first, last - first + 1) };
		}

		std::string ToLower(std::string a_text)
		{
			for (auto& c : a_text) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return a_text;
		}

		bool EqualsNoCase(std::string_view a_lhs, std::string_view a_rhs)
		{
			if (a_lhs.size() != a_rhs.size()) {
				return false;
			}

			for (std::size_t i = 0; i < a_lhs.size(); ++i) {
				if (std::tolower(static_cast<unsigned char>(a_lhs[i])) !=
					std::tolower(static_cast<unsigned char>(a_rhs[i]))) {
					return false;
				}
			}

			return true;
		}

		// The plugin that DEFINED a form. GetFile(0) is the first file in the
		// source chain, so it is the plugin that CREATED the record rather than
		// whichever one edited it last - the same identity Core/MultiChannel
		// derives for its contributors.
		std::string_view DefiningFile(RE::TESForm* a_form)
		{
			if (a_form) {
				if (const auto* file = a_form->GetFile(0)) {
					return file->GetFilename();
				}
			}
			return {};
		}

		// THE ONE WRONG OWNER THIS MODULE CAN RECOGNISE ON ITS OWN.
		//
		// akOwner exists so a block can expire: on load, a form whose plugin
		// left the load order does not resolve. A form from a base-game master
		// can NEVER stop resolving, so a block owned by one can never expire -
		// the exact failure the owner was introduced to prevent, wearing a
		// valid-looking argument.
		//
		// The general case is unreachable from here: this module has no idea
		// which plugin is "the consumer's". These five ship with the game and
		// are never absent, so they are checkable.
		//
		// WARNED, NOT REFUSED. Refusing on a heuristic would lock out an
		// arrangement nobody has thought of yet. A loud line turns a silent
		// mistake into a visible one, which is the whole point.
		bool IsBaseGameMaster(std::string_view a_file)
		{
			constexpr std::string_view kMasters[]{
				"Skyrim.esm"sv, "Update.esm"sv, "Dawnguard.esm"sv, "HearthFires.esm"sv,
				"Dragonborn.esm"sv
			};

			for (const auto master : kMasters) {
				if (EqualsNoCase(a_file, master)) {
					return true;
				}
			}

			return false;
		}

		// -------------------------------------------------------------------
		// Configuration - Data\SKSE\Plugins\Lodestone.ini, section [EquipVeto].
		//
		// DIAGNOSTICS ONLY. Nothing in this file changes what is refused; the
		// blocks come from the consumer, through the natives. The parser is
		// flat and ignores section headers - see the note in the ini itself.
		// -------------------------------------------------------------------

		// "0x00012E4D | Skyrim.esm" -> the form. Returns null and logs on
		// anything it cannot parse or find - a typo in configuration must not
		// look like the feature failing.
		RE::TESForm* ResolveFormLine(std::string_view a_key, std::string_view a_value)
		{
			const auto bar = a_value.find('|');
			if (bar == std::string_view::npos) {
				spdlog::error("EquipVeto: config line \"{} = {}\" has no '|' separator - expected "
							  "\"0x00012E4D | Skyrim.esm\". Line ignored.",
					a_key, a_value);
				return nullptr;
			}

			const auto idText = Trim(a_value.substr(0, bar));
			const auto modName = Trim(a_value.substr(bar + 1));

			if (idText.empty() || modName.empty()) {
				spdlog::error("EquipVeto: config line \"{} = {}\" is missing the form id or the plugin "
							  "name. Line ignored.",
					a_key, a_value);
				return nullptr;
			}

			RE::FormID rawID{ 0 };
			try {
				rawID = static_cast<RE::FormID>(std::stoul(idText, nullptr, 16));
			} catch (...) {
				spdlog::error("EquipVeto: \"{}\" is not a hexadecimal form id. Line ignored.", idText);
				return nullptr;
			}

			auto* handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				spdlog::error("EquipVeto: no TESDataHandler - cannot resolve \"{}\".", a_value);
				return nullptr;
			}

			// LookupForm takes the LOCAL id, so the load index in the written
			// value is ignored on purpose: 0x00012E4D and 0x12E4D name the same
			// record, and a value copied out of xEdit carries whatever index
			// that session happened to have.
			auto* form = handler->LookupForm(rawID & 0x00FFFFFF, modName);
			if (!form) {
				spdlog::error("EquipVeto: form 0x{:06X} not found in \"{}\" - is that plugin loaded? Line "
							  "ignored.",
					rawID & 0x00FFFFFF, modName);
				return nullptr;
			}

			return form;
		}

		void LoadConfig()
		{
			std::ifstream file{ std::string{ kConfigPath } };
			if (!file) {
				spdlog::info("EquipVeto: no config at \"{}\" - diagnostics keep their defaults. This does "
							 "not affect what is refused.",
					kConfigPath);
				return;
			}

			std::string line;
			while (std::getline(file, line)) {
				const auto trimmed = Trim(line);
				if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#' ||
					trimmed.front() == '[') {
					continue;
				}

				const auto eq = trimmed.find('=');
				if (eq == std::string::npos) {
					continue;
				}

				const auto key = ToLower(Trim(std::string_view{ trimmed }.substr(0, eq)));
				const auto value = Trim(std::string_view{ trimmed }.substr(eq + 1));

				if (key == "logequipobject") {
					g_logEquipObject = (value != "0");
				} else if (key == "watchactor") {
					if (auto* form = ResolveFormLine("WatchActor", value)) {
						g_watchActors.insert(form->GetFormID());
						spdlog::info("EquipVeto: watching actor [0x{:08X}].", form->GetFormID());
					}
				} else if (key == "watchfaction") {
					if (auto* form = ResolveFormLine("WatchFaction", value)) {
						if (form->GetFormType() != RE::FormType::Faction) {
							spdlog::error("EquipVeto: [0x{:08X}] is not a faction - WatchFaction line "
										  "ignored.",
								form->GetFormID());
						} else {
							g_watchFactions.push_back(static_cast<RE::TESFaction*>(form));
							spdlog::info("EquipVeto: watching faction [0x{:08X}] - every actor in it.",
								form->GetFormID());
						}
					}
				}
			}
		}

		// -------------------------------------------------------------------
		// Diagnostics
		// -------------------------------------------------------------------
		bool HasWatchList()
		{
			return !g_watchActors.empty() || !g_watchFactions.empty();
		}

		bool IsWatched(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return false;
			}

			if (g_watchActors.contains(a_actor->GetFormID())) {
				return true;
			}

			for (auto* faction : g_watchFactions) {
				if (faction && a_actor->IsInFaction(faction)) {
					return true;
				}
			}

			return false;
		}

		// With a watch list, a watched actor is the measurement and every one of
		// its calls is written - thinning it would recreate the hole this
		// exists to close. Without one, thinning by count is all that keeps a
		// cell load from drowning the file.
		//
		// THINNING BY COUNT ALREADY LOST A MEASUREMENT ONCE: a cell load spent
		// the whole budget dressing every NPC in the area (58 calls in 7 ms)
		// before the actor being tested did anything. That is why the watch
		// list exists at all.
		bool ShouldLog(RE::Actor* a_actor, std::uint64_t a_seq)
		{
			if (HasWatchList()) {
				return IsWatched(a_actor);
			}

			return a_seq <= 100 || a_seq % 25 == 0;
		}

		// The crowd is silenced in the log and kept in the arithmetic. Without
		// this, filtering by actor would answer "how often does this one retry"
		// and lose "what does this cost everybody else".
		void MaybeLogSummary()
		{
			const auto now = NowMs();
			auto       last = g_lastSummaryMs.load(std::memory_order_relaxed);

			if (now - last < kSummaryIntervalMs) {
				return;
			}

			if (!g_lastSummaryMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
				return;
			}

			const auto calls = g_calls.load(std::memory_order_relaxed);
			const auto watched = g_watched.load(std::memory_order_relaxed);

			const auto callsDelta = calls - g_callsAtLastSummary;
			const auto watchedDelta = watched - g_watchedAtLastSummary;

			g_callsAtLastSummary = calls;
			g_watchedAtLastSummary = watched;

			if (callsDelta == 0) {
				return;
			}

			const auto elapsedMs = now - last;

			spdlog::info("EquipVeto: [rate] {} calls ({} watched) in {} ms ({:.1f} calls/s). Totals: {} "
						 "calls, {} watched.",
				callsDelta, watchedDelta, elapsedMs,
				elapsedMs > 0 ? (static_cast<double>(callsDelta) * 1000.0 / static_cast<double>(elapsedMs))
							  : 0.0,
				calls, watched);
		}

		// Answers, from the engine's own inventory, whether the item just
		// refused is nevertheless being reported as worn. Not free, and it runs
		// inside the hook, so it fires only on the first refusal of a pair and
		// then every twenty-fifth.
		void LogWornState(RE::Actor* a_actor, RE::TESBoundObject* a_item, std::uint64_t a_count)
		{
			try {
				auto inventory = a_actor->GetInventory([&](RE::TESBoundObject& a_object) {
					return std::addressof(a_object) == a_item;
				});

				const auto it = inventory.find(a_item);
				if (it == inventory.end()) {
					spdlog::info("EquipVeto:   [worn check #{}] the item is NOT in the actor's inventory "
								 "at all.",
						a_count);
					return;
				}

				const auto& entry = it->second;
				const bool  worn = entry.second ? entry.second->IsWorn() : false;

				spdlog::info("EquipVeto:   [worn check #{}] count={} IsWorn={}", a_count, entry.first,
					worn ? "TRUE - INCONSISTENT" : "false");
			} catch (...) {
				spdlog::error("EquipVeto:   [worn check #{}] threw - no answer.", a_count);
			}
		}

		// Answers whether an item is ALREADY on the actor's body. A substitution
		// aimed at something already worn is a redundant equip, and redundant
		// equips are what turned this module into a visible defect.
		//
		// WHAT GOES WRONG WITHOUT IT: the refusal is mute - the AI cannot see a
		// veto, so it re-picks the forbidden item and asks again, and again. Every
		// ask used to put the substitute on again. On a weapon that restarts the
		// draw animation, so the follower sheathes and draws and never attacks -
		// measured at 3.1 calls/s across 75 seconds, 16 of them one single pair.
		// On armor the same loop runs unseen, because armor has no animation to
		// restart: a consumer log from the day before shows one helmet substituted
		// seven times inside 350 ms and nobody noticed.
		//
		// THIS DOES NOT STOP THE LOOP, and nothing on this path can. The AI keeps
		// asking at the same rate; what stops is putting the item on each time.
		// Convergence has to come from what the AI can CHOOSE - the consumer
		// keeping the forbidden item out of reach - not from the equip path.
		//
		// Two primitives, because the engine keeps the two kinds of worn item in
		// different places:
		//   - armor -> GetWornArmor(formID), which walks the inventory
		//   - anything else -> the two hands, which is a pointer read
		//
		// Either hand counts. The substitution passes slot=nullptr, so it never
		// targets a hand in the first place - if the item is in one of them,
		// equipping it again buys nothing.
		//
		// FAILING OPEN IS THE SAFE SIDE. Any doubt returns false, which equips the
		// substitute exactly as it did before this guard existed.
		bool IsAlreadyOnTheBody(RE::Actor* a_actor, RE::TESBoundObject* a_item)
		{
			if (!a_actor || !a_item) {
				return false;
			}

			try {
				if (a_item->IsArmor()) {
					// noInit: this runs inside the hook, and forcing an
					// uninitialised inventory to load here is not this module's
					// call to make.
					return a_actor->GetWornArmor(a_item->GetFormID(), true) != nullptr;
				}

				return a_actor->GetEquippedObject(false) == a_item ||
				       a_actor->GetEquippedObject(true) == a_item;
			} catch (...) {
				return false;
			}
		}

		// RESETS THE DIAGNOSTIC COUNTERS, and this exists because their survival
		// across a load hid the one thing a production-style run was there to
		// show. Carried across a load, a pair already at 50 refusals and a
		// substitution counter already at 50 mean the first refusal and the
		// first swap AFTER the load fall in the silent range - and absence of
		// the line reads exactly like absence of the swap.
		//
		// A load is the boundary that matters here: it is where the blocks come
		// back from the cosave and act before the consumer is awake.
		//
		// The call totals behind [rate] are deliberately NOT reset: they measure
		// traffic over a session, and that question does not restart at a load.
		void ResetDiagnosticCounters(const char* a_why)
		{
			{
				const std::lock_guard lock{ g_statsLock };
				g_refusalsByPair.clear();
			}

			g_substitutions.store(0, std::memory_order_relaxed);
			g_redundantSwapsAvoided.store(0, std::memory_order_relaxed);

			spdlog::info("EquipVeto: refusal and substitution counters reset ({}). The numbering below "
						 "restarts at #1 - it is not a new session.",
				a_why);
		}

		// -------------------------------------------------------------------
		// Cosave, driven by Core/Serialization
		// -------------------------------------------------------------------
		void CosaveSaveImpl(SKSE::SerializationInterface* a_intfc)
		{
			const std::shared_lock lock{ g_registryLock };

			if (!a_intfc->OpenRecord(kRecordType, kRecordVersion)) {
				spdlog::error("EquipVeto: failed to open the save record - {} block(s) not saved, and the "
							  "consumer will have to re-register all of them after the load.",
					g_registry.size());
				return;
			}

			const auto count = static_cast<std::uint32_t>(g_registry.size());
			if (!a_intfc->WriteRecordData(count)) {
				spdlog::error("EquipVeto: failed to write the block count - save record incomplete.");
				return;
			}

			for (const auto& [key, entry] : g_registry) {
				const auto actorID = static_cast<RE::FormID>(key >> 32);
				const auto itemID = static_cast<RE::FormID>(key & 0xFFFFFFFF);
				const auto substituteID = entry.substitute ? entry.substitute->GetFormID() : 0;

				if (!a_intfc->WriteRecordData(actorID) || !a_intfc->WriteRecordData(itemID) ||
					!a_intfc->WriteRecordData(substituteID) || !a_intfc->WriteRecordData(entry.owner)) {
					spdlog::error("EquipVeto: failed to write a block (actor 0x{:08X}, item 0x{:08X}) - "
								  "save record incomplete.",
						actorID, itemID);
					return;
				}
			}

			spdlog::info("EquipVeto: saved {} block(s).", count);
		}

		void CosaveLoadBeginImpl()
		{
			{
				const std::unique_lock lock{ g_registryLock };
				g_registry.clear();
				g_registryHasEntries.store(false, std::memory_order_release);
			}

			// Outside the registry lock on purpose. The thunk never holds both
			// mutexes at once, and keeping it that way means no ordering rule
			// has to be remembered later.
			ResetDiagnosticCounters("load");
		}

		bool CosaveLoadRecordImpl(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type)
		{
			if (a_type != kRecordType) {
				return false;
			}

			const std::unique_lock lock{ g_registryLock };

			std::uint32_t count = 0;
			if (a_intfc->ReadRecordData(count) != sizeof(count)) {
				spdlog::error("EquipVeto: failed to read the block count - load abandoned, every block is "
							  "gone and the consumer must re-register.");
				return true;
			}

			std::uint32_t loaded = 0;
			std::uint32_t droppedOwner = 0;
			std::uint32_t droppedOther = 0;

			for (std::uint32_t i = 0; i < count; ++i) {
				RE::FormID actorID = 0;
				RE::FormID itemID = 0;
				RE::FormID substituteID = 0;
				RE::FormID ownerID = 0;

				if (a_intfc->ReadRecordData(actorID) != sizeof(actorID) ||
					a_intfc->ReadRecordData(itemID) != sizeof(itemID) ||
					a_intfc->ReadRecordData(substituteID) != sizeof(substituteID) ||
					a_intfc->ReadRecordData(ownerID) != sizeof(ownerID)) {
					spdlog::error("EquipVeto: failed to read block {} of {} - load abandoned.", i, count);
					return true;
				}

				// THE OWNER IS CHECKED FIRST, and it is the whole reason this
				// record can exist. A form whose plugin left the load order
				// does not resolve, so an uninstalled mod's blocks die here -
				// during the load, before anything can be equipped.
				RE::FormID newOwner = 0;
				if (!a_intfc->ResolveFormID(ownerID, newOwner)) {
					++droppedOwner;
					continue;
				}

				RE::FormID newActor = 0;
				RE::FormID newItem = 0;
				if (!a_intfc->ResolveFormID(actorID, newActor) ||
					!a_intfc->ResolveFormID(itemID, newItem)) {
					++droppedOther;
					continue;
				}

				RE::TESBoundObject* substitute = nullptr;
				if (substituteID != 0) {
					RE::FormID newSubstitute = 0;
					if (!a_intfc->ResolveFormID(substituteID, newSubstitute)) {
						// The block had a substitute and it is gone. Keeping the
						// block would turn it into a bare refusal, and a bare
						// refusal was MEASURED leaving an actor stuck in its
						// base outfit. Dropping the block gives back vanilla
						// behavior, which is the honest worse-of-two.
						++droppedOther;
						continue;
					}

					auto* form = RE::TESForm::LookupByID(newSubstitute);
					substitute = form ? form->As<RE::TESBoundObject>() : nullptr;
					if (!substitute) {
						++droppedOther;
						continue;
					}
				}

				g_registry[MakeKey(newActor, newItem)] = BlockEntry{ substitute, newOwner };
				++loaded;
			}

			g_registryHasEntries.store(!g_registry.empty(), std::memory_order_release);

			spdlog::info("EquipVeto: loaded {} of {} block(s). Dropped {} whose owner is no longer in the "
						 "load order, {} whose actor, item or substitute did not resolve.",
				loaded, count, droppedOwner, droppedOther);

			return true;
		}

		void CosaveRevertImpl()
		{
			{
				const std::unique_lock lock{ g_registryLock };
				g_registry.clear();
				g_registryHasEntries.store(false, std::memory_order_release);
			}

			spdlog::info("EquipVeto: registry cleared (new game or return to main menu).");
			ResetDiagnosticCounters("new game or main menu");
		}

		// -------------------------------------------------------------------
		// THE HOOK
		//
		// It SUPPRESSES the original on the blocked path, which the conventions
		// allow under three conditions:
		//
		//   1. Only the blocked path suppresses. Every other call reaches the
		//      original untouched, so with nothing registered this is exactly
		//      equivalent to not hooking.
		//
		//   2. The effect cannot be undone afterwards, and that is the reason
		//      this module exists. EquipObject PUTS THE ITEM ON - letting it
		//      run and unequipping after is the reactive loop being replaced,
		//      window and all. The engine does not hand back what the worn
		//      state was beforehand, so an undo cannot tell "this call put it
		//      on" from "it was already on".
		//
		//   3. Every skipped side effect accounted for. Skipping the call skips
		//      the sound, the slot bookkeeping, and whatever the AI reads back.
		//      What the measurement says: the engine equips the whole loadout
		//      in one pass, permitted piece first, so a refusal leaves the
		//      previous piece on rather than leaving the slot bare - and nine
		//      consumer-driven test rounds produced no inconsistent worn state.
		//
		// MEASURE WITH OTHER PLUGINS OFF. Several published SKSE plugins hook
		// this same function - powerof3's Item Equip Restrictor among them. If
		// one is loaded, the "original" here may be its thunk, and hook
		// chaining leaves no trace in a log.
		// -------------------------------------------------------------------
		struct EquipObjectHook
		{
			static void thunk(RE::ActorEquipManager* a_this, RE::Actor* a_actor,
				RE::TESBoundObject* a_object, RE::ExtraDataList* a_extraData, std::uint32_t a_count,
				const RE::BGSEquipSlot* a_slot, bool a_queueEquip, bool a_forceEquip, bool a_playSounds,
				bool a_applyNow)
			{
				const auto seq = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;

				bool refuse = false;

				try {
					if (IsWatched(a_actor)) {
						g_watched.fetch_add(1, std::memory_order_relaxed);
					}

					MaybeLogSummary();

					RE::TESBoundObject* substitute{ nullptr };
					bool                blocked{ false };

					if (a_actor && a_object && g_registryHasEntries.load(std::memory_order_acquire)) {
						const auto key = MakeKey(a_actor->GetFormID(), a_object->GetFormID());

						const std::shared_lock lock{ g_registryLock };
						if (const auto it = g_registry.find(key); it != g_registry.end()) {
							blocked = true;
							substitute = it->second.substitute;
						}
					}

					if (blocked) {
						refuse = true;
						g_pendingSubstitute = substitute;

						const auto pairKey = MakeKey(a_actor->GetFormID(), a_object->GetFormID());

						std::uint64_t count{ 0 };
						{
							const std::lock_guard lock{ g_statsLock };
							count = ++g_refusalsByPair[pairKey];
						}

						g_refusals.fetch_add(1, std::memory_order_relaxed);

						if (count == 1 || count % 25 == 0) {
							spdlog::info("EquipVeto: [REFUSED #{}] call #{} - actor [0x{:08X}] \"{}\" item "
										 "[0x{:08X}] \"{}\" count={} queueEquip={} forceEquip={} "
										 "applyNow={}",
								count, seq, a_actor->GetFormID(),
								a_actor->GetDisplayFullName() ? a_actor->GetDisplayFullName() : "",
								a_object->GetFormID(), a_object->GetName() ? a_object->GetName() : "",
								a_count, a_queueEquip, a_forceEquip, a_applyNow);

							LogWornState(a_actor, a_object, count);
						}
					} else if (g_logEquipObject && ShouldLog(a_actor, seq)) {
						spdlog::info("EquipVeto: [#{}] actor [0x{:08X}] \"{}\" item [0x{:08X}] \"{}\" "
									 "count={} queueEquip={} forceEquip={} playSounds={} applyNow={} "
									 "extraData={}",
							seq, a_actor ? a_actor->GetFormID() : 0,
							(a_actor && a_actor->GetDisplayFullName()) ? a_actor->GetDisplayFullName() : "",
							a_object ? a_object->GetFormID() : 0,
							(a_object && a_object->GetName()) ? a_object->GetName() : "", a_count,
							a_queueEquip, a_forceEquip, a_playSounds, a_applyNow,
							a_extraData ? "yes" : "no");
					}
				} catch (...) {
					// A C++ exception crossing back into the engine is undefined
					// behavior. Swallow, and let the call through - vanilla is
					// the safe side of this branch.
					refuse = false;
				}

				if (refuse) {
					// THE REFUSAL. The original never runs with the blocked
					// item, so it is never put on.
					//
					// NO RECURSION. The replacement goes through the SAVED
					// ORIGINAL, not through EquipObject again, so it cannot
					// re-enter this thunk.
					try {
						auto* replacement = g_pendingSubstitute;
						g_pendingSubstitute = nullptr;

						// THE GUARD. Putting on what is already on restarts the
						// animation and buys nothing - see IsAlreadyOnTheBody. The
						// refusal above still stands either way: the blocked item
						// does not go on.
						if (replacement && IsAlreadyOnTheBody(a_actor, replacement)) {
							const auto avoided =
								g_redundantSwapsAvoided.fetch_add(1, std::memory_order_relaxed) + 1;

							if (avoided <= 25 || avoided % 25 == 0) {
								spdlog::info("EquipVeto: [ALREADY WORN #{}] actor [0x{:08X}] \"{}\" - "
											 "[0x{:08X}] \"{}\" refused, and [0x{:08X}] \"{}\" is "
											 "already on. Nothing equipped.",
									avoided, a_actor ? a_actor->GetFormID() : 0,
									(a_actor && a_actor->GetDisplayFullName())
										? a_actor->GetDisplayFullName()
										: "",
									a_object->GetFormID(),
									a_object->GetName() ? a_object->GetName() : "",
									replacement->GetFormID(),
									replacement->GetName() ? replacement->GetName() : "");
							}
						} else if (replacement) {
							const auto swaps = g_substitutions.fetch_add(1, std::memory_order_relaxed) + 1;

							if (swaps <= 25 || swaps % 25 == 0) {
								spdlog::info("EquipVeto: [SUBSTITUTED #{}] actor [0x{:08X}] \"{}\" - "
											 "[0x{:08X}] \"{}\" refused, equipping [0x{:08X}] \"{}\" "
											 "instead.",
									swaps, a_actor ? a_actor->GetFormID() : 0,
									(a_actor && a_actor->GetDisplayFullName())
										? a_actor->GetDisplayFullName()
										: "",
									a_object->GetFormID(),
									a_object->GetName() ? a_object->GetName() : "",
									replacement->GetFormID(),
									replacement->GetName() ? replacement->GetName() : "");
							}

							// THE THREE ARGUMENTS THAT ARE A JUDGEMENT CALL:
							//
							//   extraData -> nullptr. The caller's ExtraDataList
							//     belongs to the REFUSED item's inventory entry -
							//     enchantment charge, health, a custom name.
							//     Handing it to a different object would attach
							//     one item's data to another.
							//   slot -> nullptr, so the replacement lands in its
							//     own default slot rather than one computed for
							//     the refused item.
							//   count -> 1. A worn piece is one piece.
							//
							// Everything else passes through untouched.
							g_hook.call<void, RE::ActorEquipManager*, RE::Actor*, RE::TESBoundObject*,
								RE::ExtraDataList*, std::uint32_t, const RE::BGSEquipSlot*, bool, bool,
								bool, bool>(a_this, a_actor, replacement, nullptr, 1, nullptr,
								a_queueEquip, a_forceEquip, a_playSounds, a_applyNow);
						}
					} catch (...) {
						spdlog::error("EquipVeto: the substitution threw - the blocked item is still "
									  "refused, and nothing was equipped in its place.");
					}

					return;
				}

				g_hook.call<void, RE::ActorEquipManager*, RE::Actor*, RE::TESBoundObject*,
					RE::ExtraDataList*, std::uint32_t, const RE::BGSEquipSlot*, bool, bool, bool, bool>(
					a_this, a_actor, a_object, a_extraData, a_count, a_slot, a_queueEquip, a_forceEquip,
					a_playSounds, a_applyNow);
			}
		};

		// -------------------------------------------------------------------
		// Natives
		//
		// No exception crosses into the VM, failure is a sentinel return
		// (Bool -> false, Int -> -1), and each one states what it returns and
		// when it fails.
		// -------------------------------------------------------------------
		namespace Natives
		{
			// Blocks a_item on a_actor, naming what to equip instead and who is
			// asking. a_substitute may be None, which is a plain refusal - see
			// EquipVeto.h for why that is rarely what a consumer wants.
			//
			// Returns false if the actor, the item or the owner is None, if the
			// item is not something that can be worn, or if a substitute was
			// given that is not either. Re-blocking the same pair overwrites
			// the substitute and the owner, which is how a consumer keeps them
			// current without unblocking first.
			bool BlockEquip(RE::StaticFunctionTag*, RE::Actor* a_actor, RE::TESForm* a_item,
				RE::TESForm* a_substitute, RE::TESForm* a_owner)
			{
				try {
					if (!a_actor || !a_item) {
						spdlog::warn("EquipVeto: BlockEquip called with a None actor or item.");
						return false;
					}

					if (!a_item->As<RE::TESBoundObject>()) {
						spdlog::warn("EquipVeto: BlockEquip - [0x{:08X}] is not a bound object and cannot "
									 "be equipped, so blocking it means nothing.",
							a_item->GetFormID());
						return false;
					}

					// The owner is REQUIRED, and refusing without it is the
					// point. It is what lets a block survive a load and still
					// die when the mod that made it is gone.
					if (!a_owner) {
						spdlog::warn("EquipVeto: BlockEquip called with a None owner - refused. Pass any "
									 "Form your own plugin defines; it is the identity that lets this "
									 "block be dropped when your mod is uninstalled.");
						return false;
					}

					RE::TESBoundObject* substitute{ nullptr };
					if (a_substitute) {
						substitute = a_substitute->As<RE::TESBoundObject>();
						if (!substitute) {
							spdlog::warn("EquipVeto: BlockEquip - the substitute [0x{:08X}] is not a bound "
										 "object.",
								a_substitute->GetFormID());
							return false;
						}
					}

					const auto ownerFile = DefiningFile(a_owner);

					if (ownerFile.empty()) {
						spdlog::warn("EquipVeto: BlockEquip - the owner [0x{:08X}] has no source plugin. "
									 "The block is registered, but nothing can tell later whether the mod "
									 "that made it is still installed.",
							a_owner->GetFormID());
					} else if (IsBaseGameMaster(ownerFile)) {
						spdlog::warn("EquipVeto: BlockEquip - the owner [0x{:08X}] is defined by \"{}\", a "
									 "base-game master. THAT FILE NEVER LEAVES THE LOAD ORDER, so this "
									 "block will NEVER expire and will outlive the mod that made it - "
									 "which is exactly what the owner exists to prevent. Pass a Form YOUR "
									 "OWN plugin defines. Registering it anyway.",
							a_owner->GetFormID(), ownerFile);
					}

					{
						const std::unique_lock lock{ g_registryLock };
						g_registry[MakeKey(a_actor->GetFormID(), a_item->GetFormID())] =
							BlockEntry{ substitute, a_owner->GetFormID() };
						g_registryHasEntries.store(true, std::memory_order_release);
					}

					spdlog::info("EquipVeto: BlockEquip - actor [0x{:08X}] item [0x{:08X}] \"{}\", "
								 "substitute [0x{:08X}], owner [0x{:08X}] from \"{}\".",
						a_actor->GetFormID(), a_item->GetFormID(),
						a_item->GetName() ? a_item->GetName() : "",
						substitute ? substitute->GetFormID() : 0, a_owner->GetFormID(),
						ownerFile.empty() ? "(no source plugin)"sv : ownerFile);

					return true;
				} catch (...) {
					spdlog::error("EquipVeto: BlockEquip threw - nothing was registered.");
					return false;
				}
			}

			// Removes one block. Returns false if there was nothing to remove,
			// or if either argument is None.
			bool UnblockEquip(RE::StaticFunctionTag*, RE::Actor* a_actor, RE::TESForm* a_item)
			{
				try {
					if (!a_actor || !a_item) {
						return false;
					}

					bool removed{ false };
					{
						const std::unique_lock lock{ g_registryLock };
						removed = g_registry.erase(MakeKey(a_actor->GetFormID(), a_item->GetFormID())) > 0;
						g_registryHasEntries.store(!g_registry.empty(), std::memory_order_release);
					}

					if (removed) {
						spdlog::info("EquipVeto: UnblockEquip - actor [0x{:08X}] item [0x{:08X}].",
							a_actor->GetFormID(), a_item->GetFormID());
					}

					return removed;
				} catch (...) {
					spdlog::error("EquipVeto: UnblockEquip threw.");
					return false;
				}
			}

			// Drops every block held for one actor. Returns how many were
			// removed, or -1 if the actor is None or the call failed.
			std::int32_t ClearEquipBlocks(RE::StaticFunctionTag*, RE::Actor* a_actor)
			{
				try {
					if (!a_actor) {
						return -1;
					}

					const auto   actorID = a_actor->GetFormID();
					std::int32_t removed{ 0 };

					{
						const std::unique_lock lock{ g_registryLock };
						for (auto it = g_registry.begin(); it != g_registry.end();) {
							if (static_cast<RE::FormID>(it->first >> 32) == actorID) {
								it = g_registry.erase(it);
								++removed;
							} else {
								++it;
							}
						}
						g_registryHasEntries.store(!g_registry.empty(), std::memory_order_release);
					}

					spdlog::info("EquipVeto: ClearEquipBlocks - actor [0x{:08X}], {} removed.", actorID,
						removed);

					return removed;
				} catch (...) {
					spdlog::error("EquipVeto: ClearEquipBlocks threw.");
					return -1;
				}
			}

			// Whether a block is held for the pair. Returns false for None
			// arguments and on failure.
			bool IsEquipBlocked(RE::StaticFunctionTag*, RE::Actor* a_actor, RE::TESForm* a_item)
			{
				try {
					if (!a_actor || !a_item) {
						return false;
					}

					const std::shared_lock lock{ g_registryLock };
					return g_registry.contains(MakeKey(a_actor->GetFormID(), a_item->GetFormID()));
				} catch (...) {
					return false;
				}
			}

			// How many blocks are held, across every actor. Diagnostic: a
			// consumer that expects a bounded number can watch it and notice a
			// registration loop. Returns -1 on failure.
			std::int32_t GetEquipBlockCount(RE::StaticFunctionTag*)
			{
				try {
					const std::shared_lock lock{ g_registryLock };
					return static_cast<std::int32_t>(g_registry.size());
				} catch (...) {
					return -1;
				}
			}
		}
	}

	void CosaveSave(SKSE::SerializationInterface* a_intfc)
	{
		CosaveSaveImpl(a_intfc);
	}

	void CosaveLoadBegin()
	{
		CosaveLoadBeginImpl();
	}

	bool CosaveLoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type)
	{
		return CosaveLoadRecordImpl(a_intfc, a_type);
	}

	void CosaveRevert()
	{
		CosaveRevertImpl();
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("EquipVeto: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("BlockEquip", "Lodestone", Natives::BlockEquip);
		a_vm->RegisterFunction("UnblockEquip", "Lodestone", Natives::UnblockEquip);
		a_vm->RegisterFunction("ClearEquipBlocks", "Lodestone", Natives::ClearEquipBlocks);
		a_vm->RegisterFunction("IsEquipBlocked", "Lodestone", Natives::IsEquipBlocked);
		a_vm->RegisterFunction("GetEquipBlockCount", "Lodestone", Natives::GetEquipBlockCount);

		spdlog::info("EquipVeto: natives registered (BlockEquip, UnblockEquip, ClearEquipBlocks, "
					 "IsEquipBlocked, GetEquipBlockCount).");
		return true;
	}

	void Install()
	{
		if (REL::Module::IsVR()) {
			spdlog::info("EquipVeto: NOT installed - this is a VR runtime. The address is only known for "
						 "SE and AE, and a two-argument RELOCATION_ID assigns the SE id to the VR slot "
						 "rather than refusing, which installs quietly on the wrong function. The natives "
						 "still exist and the registry still accepts blocks; nothing is refused.");
			return;
		}

		try {
			LoadConfig();

			g_lastSummaryMs.store(NowMs(), std::memory_order_relaxed);

			// ActorEquipManager::EquipObject. Non-virtual, so an inline hook -
			// the target decides the mechanism. The id is read from the pinned
			// fork's own implementation in
			// extern\commonlibsse-ng\src\RE\A\ActorEquipManager.cpp.
			REL::Relocation<std::uintptr_t> target{ REL::RelocationID(37938, 38894) };

			g_hook = safetyhook::create_inline(reinterpret_cast<void*>(target.address()),
				reinterpret_cast<void*>(&EquipObjectHook::thunk));

			if (!g_hook) {
				spdlog::error("EquipVeto: SafetyHook refused to hook ActorEquipManager::EquipObject at "
							  "0x{:X} - NOTHING WILL BE REFUSED this session, and a consumer registering "
							  "blocks will see no effect.",
					target.address());
				return;
			}

			spdlog::info("EquipVeto: installed on ActorEquipManager::EquipObject at 0x{:X} ({} runtime). "
						 "Idle until a consumer registers a block. Watch list: {} actor(s), {} "
						 "faction(s).",
				target.address(), REL::Module::IsAE() ? "AE" : "SE", g_watchActors.size(),
				g_watchFactions.size());
		} catch (const std::exception& e) {
			spdlog::error("EquipVeto: NOT installed - {}. Equipping is untouched and nothing will be "
						  "refused.",
				e.what());
		} catch (...) {
			spdlog::error("EquipVeto: NOT installed (unknown exception). Equipping is untouched and "
						  "nothing will be refused.");
		}
	}
}
