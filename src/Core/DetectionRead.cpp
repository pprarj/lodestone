// DetectionRead.cpp
// Lodestone - Shared SKSE framework
//
// The detection reading module. See DetectionRead.h for what this module is,
// and in particular for why it caches instead of calling the engine
// directly from the native.
//
// Phase L-B1 (detection reading)

#include "DetectionRead.h"

#include <mutex>
#include <unordered_map>

namespace Lodestone::Core::DetectionRead
{
	namespace
	{
		// One reading, keyed by the actor's handle rather than its FormID.
		// A FormID survives the actor dying or unloading and can be reused
		// by an unrelated object later; the handle carries a generation and
		// simply fails to resolve once it no longer matches, which is what
		// lets a stale entry be told apart from a live one without any
		// explicit invalidation on save load - see the header note and
		// Core/Incapacitation's FallenState, which keys the same way for the
		// same reason.
		struct CacheEntry
		{
			RE::ActorHandle handle;
			std::int32_t    level         = 0;
			std::uint32_t   observerCount = 0;

			// True once a reading has actually landed. Distinct from the
			// entry merely existing - ReadCache creates an entry the first
			// time an actor is asked about, before any reading has run.
			bool warm = false;

			// True while a refresh for this actor is already queued. Without
			// this, a consumer calling both natives back to back - the
			// documented way to read the pair - would queue two AddTask
			// refreshes for one logical reading, paying for the engine call
			// twice. See the research doc, section 2.2: "guarde o par
			// junto."
			bool pending = false;
		};

		std::mutex                                    g_lock;
		std::unordered_map<std::uint32_t, CacheEntry> g_cache;

		// Runs on the game thread, queued via SKSE::GetTaskInterface().
		// Resolves the handle itself rather than trusting that the actor
		// the native saw is still around - it may have died or unloaded in
		// the time between the native call and this task running.
		void RefreshOne(RE::ActorHandle a_handle)
		{
			std::int32_t  level    = 0;
			std::uint32_t losCount = 0;
			bool          ok       = false;

			auto actor = a_handle.get();
			if (actor) {
				try {
					level = RE::ProcessLists::GetSingleton()->RequestHighestDetectionLevelAgainstActor(actor.get(), losCount);
					ok    = true;
				} catch (...) {
					spdlog::error("DetectionRead: RequestHighestDetectionLevelAgainstActor threw for actor (0x{:08X}) - "
								  "cache left as it was.",
						actor->GetFormID());
				}
			}
			// actor gone: nothing to read. Fall through and clear pending -
			// the entry stays cold (or keeps its last reading) until the
			// actor is asked about again with a live handle.

			std::lock_guard lock(g_lock);
			auto            it = g_cache.find(a_handle.native_handle());
			if (it == g_cache.end()) {
				// ReadCache always creates the entry before queuing this
				// task, so this would mean the entry was erased from under
				// it - never happens today, since nothing erases entries.
				return;
			}

			it->second.pending = false;
			if (ok) {
				it->second.level         = level;
				it->second.observerCount = losCount;
				it->second.warm          = true;
			}
		}

		// Shared by both natives. Returns false when there is nothing to
		// report (null actor, no active AI process, dead, or the cache has
		// not warmed up yet) - the caller turns that into the documented -1
		// sentinel. On a warm cache it fills both outputs from the SAME
		// entry, so the two natives can never disagree about whether the
		// reading is ready.
		//
		// Cheap guards run synchronously, matching how every other native in
		// this plugin already touches Actor* directly (IsDead,
		// GetActorRuntimeData()). Only the engine's own detection query is
		// deferred to the game thread - see the header note on why.
		bool ReadCache(RE::Actor* a_actor, std::int32_t& a_outLevel, std::uint32_t& a_outCount)
		{
			if (!a_actor) {
				return false;
			}

			if (a_actor->IsDead()) {
				return false;
			}

			if (!a_actor->GetActorRuntimeData().currentProcess) {
				return false;
			}

			const auto handle = a_actor->GetHandle();
			const auto native = handle.native_handle();

			bool warm       = false;
			bool needsTask  = false;
			{
				std::lock_guard lock(g_lock);
				auto&           entry = g_cache[native];
				entry.handle          = handle;

				if (entry.warm) {
					a_outLevel = entry.level;
					a_outCount = entry.observerCount;
					warm       = true;
				}

				if (!entry.pending) {
					entry.pending = true;
					needsTask     = true;
				}
			}

			if (needsTask) {
				auto* task = SKSE::GetTaskInterface();
				if (task) {
					try {
						task->AddTask([handle]() { RefreshOne(handle); });
					} catch (...) {
						spdlog::error("DetectionRead: AddTask threw queuing a refresh for actor (0x{:08X}) - "
									  "will retry on the next call.",
							a_actor->GetFormID());
						std::lock_guard lock(g_lock);
						auto            it = g_cache.find(native);
						if (it != g_cache.end()) {
							it->second.pending = false;
						}
					}
				} else {
					spdlog::error("DetectionRead: no task interface available - cannot queue a refresh.");
					std::lock_guard lock(g_lock);
					auto            it = g_cache.find(native);
					if (it != g_cache.end()) {
						it->second.pending = false;
					}
				}
			}

			return warm;
		}

		// Lodestone.GetHighestDetectionLevel(Actor) -> Int
		//
		// See Lodestone.psc for the full contract (scale, sentinel,
		// freshness). None actor, no active AI process, dead actor, or cold
		// cache -> -1.
		std::int32_t GetHighestDetectionLevel(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			std::int32_t  level = -1;
			std::uint32_t count = 0;
			if (!ReadCache(a_actor, level, count)) {
				return -1;
			}
			return level;
		}

		// Lodestone.GetDetectionObserverCount(Actor) -> Int
		//
		// Same reading as GetHighestDetectionLevel, same error cases -> -1.
		// Zero is a real answer (nobody has line of sight).
		std::int32_t GetDetectionObserverCount(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			std::int32_t  level = 0;
			std::uint32_t count = 0;
			if (!ReadCache(a_actor, level, count)) {
				return -1;
			}
			return static_cast<std::int32_t>(count);
		}

		// -------------------------------------------------------------------
		// Filtered reading (added 1.15.0). A second, DLL-built aggregate -
		// see DetectionRead.h for why it exists and why it is not promised
		// to agree with the engine's own aggregate above, even unfiltered.
		// -------------------------------------------------------------------

		// Mirrors the engine's own observed floor for "nobody is watching" -
		// see RefreshOneFiltered for what is and is not known about -1000.
		// Chosen so a filtered reading with no surviving candidate looks
		// like the same "nobody is watching" a consumer already recognizes
		// from the unfiltered pair, instead of a third value to learn.
		constexpr std::int32_t kNoCandidateLevel = -1000;

		// A filtered reading is keyed by the pair (actor, excluded keyword) -
		// two different exclusions on the same actor are both live readings
		// at once, and neither may overwrite the other. excludeKeyword is a
		// FormID, not a pointer: identity derived from the form, same as
		// MultiChannel derives a contributor's identity from its plugin
		// rather than holding a reference to it. 0 (no real form has it)
		// marks "no exclusion", for a Keyword None argument.
		struct FilteredKey
		{
			std::uint32_t actorHandle    = 0;
			RE::FormID    excludeKeyword = 0;

			friend bool operator==(const FilteredKey&, const FilteredKey&) = default;
		};

		struct FilteredKeyHash
		{
			std::size_t operator()(const FilteredKey& a_key) const noexcept
			{
				// Plain combine, not cryptographic - a collision costs a
				// rehash bucket, nothing more, since the map still compares
				// full keys on lookup.
				std::size_t h = std::hash<std::uint32_t>{}(a_key.actorHandle);
				h ^= std::hash<RE::FormID>{}(a_key.excludeKeyword) + 0x9e3779b9U + (h << 6) + (h >> 2);
				return h;
			}
		};

		struct FilteredCacheEntry
		{
			RE::ActorHandle handle;
			std::int32_t    level          = 0;
			std::uint32_t   detectingCount = 0;
			bool            warm           = false;
			bool            pending        = false;
		};

		std::mutex                                                           g_filteredLock;
		std::unordered_map<FilteredKey, FilteredCacheEntry, FilteredKeyHash> g_filteredCache;

		// Runs on the game thread, queued via SKSE::GetTaskInterface(), same
		// as RefreshOne. a_excludeKeyword is the live pointer (keyword forms
		// are static game data, safe to hold across the queue - unlike an
		// Actor, nothing unloads it mid-session); a_excludeKeywordID is its
		// FormID, already derived, used only to find this reading's cache
		// entry again.
		//
		// -1000 AS "NOBODY IS WATCHING": observed, not proven. It is what
		// the unfiltered engine aggregate reports when no actor is paying
		// attention (see Pesquisas/ and the L-B1b consumer report), and nor
		// is it a value found in any vendored header - the aggregate that
		// produces it is compiled logic, not something Address Library
		// exposes a name for. This module reuses the same number as the
		// "nobody survived the filter" answer on purpose, so a consumer
		// never has to learn a third convention.
		void RefreshOneFiltered(RE::ActorHandle a_handle, RE::BGSKeyword* a_excludeKeyword, RE::FormID a_excludeKeywordID)
		{
			std::int32_t  level          = kNoCandidateLevel;
			std::uint32_t detectingCount = 0;
			bool          ok             = false;

			auto target = a_handle.get();
			if (target) {
				try {
					RE::ProcessLists::GetSingleton()->ForEachHighActor(
						[&](RE::Actor* a_candidate) {
							if (!a_candidate || a_candidate == target.get()) {
								return RE::BSContainer::ForEachResult::kContinue;
							}

							// A corpse stays in high process for a while after
							// it dies, and asking a dead actor for a detection
							// level answers with whatever it last had rather
							// than with nothing. The engine's own aggregate
							// does not report dead watchers; this one walks the
							// actor list itself, so it has to drop them by
							// hand. Without this the reading keeps a killed
							// bandit's last level alive - reported from play as
							// the consumer's stage moving on an empty room
							// minutes after the fight was over.
							if (a_candidate->IsDead()) {
								return RE::BSContainer::ForEachResult::kContinue;
							}

							// The engine flags actors that must not move the
							// stealth meter (kDoNotShowOnStealthMeter). Counting
							// one makes this aggregate disagree with the vanilla
							// eye the player is actually looking at, which is
							// not a disagreement anybody can act on - reported
							// from play as the stage reaching DETECTED while the
							// eye never closed.
							if (a_candidate->NotShowOnStealthMeter()) {
								return RE::BSContainer::ForEachResult::kContinue;
							}

							if (a_excludeKeyword && a_candidate->HasKeyword(a_excludeKeyword)) {
								return RE::BSContainer::ForEachResult::kContinue;
							}

							const auto candidateLevel = a_candidate->RequestDetectionLevel(
								target.get(), RE::DETECTION_PRIORITY::kNormal);
							if (candidateLevel > level) {
								level = candidateLevel;
							}
							if (candidateLevel >= 0) {
								++detectingCount;
							}
							return RE::BSContainer::ForEachResult::kContinue;
						});
					ok = true;
				} catch (...) {
					spdlog::error("DetectionRead: filtered aggregate threw for actor (0x{:08X}) - "
								  "cache left as it was.",
						target->GetFormID());
				}
			}
			// target gone: nothing to read, same fallthrough as RefreshOne.

			std::lock_guard lock(g_filteredLock);
			auto            it = g_filteredCache.find(FilteredKey{ a_handle.native_handle(), a_excludeKeywordID });
			if (it == g_filteredCache.end()) {
				// ReadCacheFiltered always creates the entry before queuing
				// this task - never happens today, nothing erases entries.
				return;
			}

			it->second.pending = false;
			if (ok) {
				it->second.level          = level;
				it->second.detectingCount = detectingCount;
				it->second.warm           = true;
			}
		}

		// Shared by both Excluding natives, same shape as ReadCache above:
		// guards run synchronously, the engine-touching work is deferred to
		// the game thread, and both outputs come from the SAME cache entry
		// so the two natives can never disagree about whether the reading
		// is ready.
		bool ReadCacheFiltered(RE::Actor* a_actor, RE::BGSKeyword* a_excludeKeyword,
			std::int32_t& a_outLevel, std::uint32_t& a_outCount)
		{
			if (!a_actor) {
				return false;
			}

			if (a_actor->IsDead()) {
				return false;
			}

			if (!a_actor->GetActorRuntimeData().currentProcess) {
				return false;
			}

			const auto handle    = a_actor->GetHandle();
			const auto keywordID = a_excludeKeyword ? a_excludeKeyword->GetFormID() : 0;
			const FilteredKey key{ handle.native_handle(), keywordID };

			bool warm      = false;
			bool needsTask = false;
			{
				std::lock_guard lock(g_filteredLock);
				auto&           entry = g_filteredCache[key];
				entry.handle          = handle;

				if (entry.warm) {
					a_outLevel = entry.level;
					a_outCount = entry.detectingCount;
					warm       = true;
				}

				if (!entry.pending) {
					entry.pending = true;
					needsTask     = true;
				}
			}

			if (needsTask) {
				auto* task = SKSE::GetTaskInterface();
				if (task) {
					try {
						task->AddTask([handle, a_excludeKeyword, keywordID]() {
							RefreshOneFiltered(handle, a_excludeKeyword, keywordID);
						});
					} catch (...) {
						spdlog::error("DetectionRead: AddTask threw queuing a filtered refresh for actor "
									  "(0x{:08X}) - will retry on the next call.",
							a_actor->GetFormID());
						std::lock_guard lock(g_filteredLock);
						auto            it = g_filteredCache.find(key);
						if (it != g_filteredCache.end()) {
							it->second.pending = false;
						}
					}
				} else {
					spdlog::error("DetectionRead: no task interface available - cannot queue a filtered refresh.");
					std::lock_guard lock(g_filteredLock);
					auto            it = g_filteredCache.find(key);
					if (it != g_filteredCache.end()) {
						it->second.pending = false;
					}
				}
			}

			return warm;
		}

		// Lodestone.GetHighestDetectionLevelExcluding(Actor, Keyword) -> Int
		//
		// Same sentinel and freshness discipline as GetHighestDetectionLevel.
		// A None keyword reads as "no exclusion" - see DetectionRead.h for
		// why that is still not promised to match the unfiltered native.
		std::int32_t GetHighestDetectionLevelExcluding(RE::StaticFunctionTag*, RE::Actor* a_actor, RE::BGSKeyword* a_excludeType)
		{
			std::int32_t  level = -1;
			std::uint32_t count = 0;
			if (!ReadCacheFiltered(a_actor, a_excludeType, level, count)) {
				return -1;
			}
			return level;
		}

		// Lodestone.GetDetectionObserverCountExcluding(Actor, Keyword) -> Int
		//
		// NOT a line-of-sight count - see DetectionRead.h for why. Counts how
		// many keyword-filtered candidates currently detect the target
		// (level >= 0).
		std::int32_t GetDetectionObserverCountExcluding(RE::StaticFunctionTag*, RE::Actor* a_actor, RE::BGSKeyword* a_excludeType)
		{
			std::int32_t  level = 0;
			std::uint32_t count = 0;
			if (!ReadCacheFiltered(a_actor, a_excludeType, level, count)) {
				return -1;
			}
			return static_cast<std::int32_t>(count);
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("DetectionRead: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("GetHighestDetectionLevel", "Lodestone", GetHighestDetectionLevel);
		a_vm->RegisterFunction("GetDetectionObserverCount", "Lodestone", GetDetectionObserverCount);
		a_vm->RegisterFunction("GetHighestDetectionLevelExcluding", "Lodestone", GetHighestDetectionLevelExcluding);
		a_vm->RegisterFunction("GetDetectionObserverCountExcluding", "Lodestone", GetDetectionObserverCountExcluding);

		spdlog::info("DetectionRead: natives registered (GetHighestDetectionLevel, GetDetectionObserverCount, "
					 "GetHighestDetectionLevelExcluding, GetDetectionObserverCountExcluding). "
					 "No hook - a cache refreshed via AddTask on the game thread, cold until the first reading lands.");
		return true;
	}
}
