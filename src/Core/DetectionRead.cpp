// DetectionRead.cpp
// Lodestone - Shared SKSE framework
//
// The detection reading module. See DetectionRead.h for what this module is,
// and in particular for why it caches instead of calling the engine
// directly from the native.
//
// Phase L-B1 (detection reading)

#include "DetectionRead.h"

#include "Config.h"

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

		// -------------------------------------------------------------------
		// Side-aware reading (added 1.39.0). The Excluding pair above filters
		// by keyword and nothing else; this one also filters by which SIDE of
		// the target a candidate is on. See DetectionRead.h for the rules and
		// for why the pair is built to be read as one reading.
		// -------------------------------------------------------------------

		// aiFlags bits of the Filtered pair. Zero means "behave as the
		// Excluding pair" (apart from the pair-consistency guarantee).
		constexpr std::int32_t kFlagSkipAllies           = 1;  // teammates, and commanded actors that are not hostile
		constexpr std::int32_t kFlagKeywordSparesHostile = 2;  // the keyword excludes only candidates NOT hostile to the target
		constexpr std::int32_t kKnownFlags               = kFlagSkipAllies | kFlagKeywordSparesHostile;

		// Opt-in diagnostics, Lodestone.ini LogDetectionFilter = 1. Read once
		// from RegisterFuncs; never changes afterwards.
		bool g_logFilteredWalk = false;

		struct SideKey
		{
			std::uint32_t actorHandle    = 0;
			RE::FormID    excludeKeyword = 0;
			std::int32_t  flags          = 0;

			friend bool operator==(const SideKey&, const SideKey&) = default;
		};

		struct SideKeyHash
		{
			std::size_t operator()(const SideKey& a_key) const noexcept
			{
				std::size_t h = std::hash<std::uint32_t>{}(a_key.actorHandle);
				h ^= std::hash<RE::FormID>{}(a_key.excludeKeyword) + 0x9e3779b9U + (h << 6) + (h >> 2);
				h ^= std::hash<std::int32_t>{}(a_key.flags) + 0x9e3779b9U + (h << 6) + (h >> 2);
				return h;
			}
		};

		struct SideReading
		{
			std::int32_t  level          = kNoCandidateLevel;
			std::uint32_t detectingCount = 0;
		};

		// TWO SLOTS, NOT ONE. The refresh task never writes what a consumer
		// is reading: it lands in `next`, and a native call moves `next` into
		// `published` only at a pair boundary. That is what keeps the level and
		// the count a consumer reads back to back from two different readings
		// (the 1.15.1 note in Lodestone.psc). The boundary is not guessed from
		// time: it is "this call asks for a half of the pair that was already
		// read from `published`" - a consumer that has read the count and the
		// level has finished a pair, and its next call starts a new one,
		// whichever half it reads first. A consumer that only ever reads one
		// half starts a new pair on every call, so it still advances.
		struct SideEntry
		{
			RE::ActorHandle handle;
			SideReading     published;
			bool            warm = false;
			SideReading     next;
			bool            nextReady = false;
			bool            levelReadOnPublished = false;
			bool            countReadOnPublished = false;
			bool            pending              = false;
		};

		std::mutex                                                    g_sideLock;
		std::unordered_map<SideKey, SideEntry, SideKeyHash> g_sideCache;

		// Game thread, queued like RefreshOneFiltered. The walk drops the
		// same three candidates the Excluding walk drops (target, dead,
		// kDoNotShowOnStealthMeter) - copied rather than shared so the
		// published pair's walk stays byte-for-byte what it was - and then
		// applies the side rules, reading hostility NOW, at walk time.
		void RefreshOneSide(RE::ActorHandle a_handle, RE::BGSKeyword* a_excludeKeyword, SideKey a_key)
		{
			SideReading   reading;
			std::uint32_t walked         = 0;
			std::uint32_t skippedAlly    = 0;
			std::uint32_t skippedKeyword = 0;
			bool          ok             = false;

			const bool skipAllies   = (a_key.flags & kFlagSkipAllies) != 0;
			const bool sparesHostile = (a_key.flags & kFlagKeywordSparesHostile) != 0;

			auto target = a_handle.get();
			if (target) {
				try {
					// The target is on the player's side when it is the player
					// or one of the player's teammates; only then does "a
					// teammate of the player" mean "an ally of the target".
					const bool targetOnPlayerSide = target->IsPlayerRef() || target->IsPlayerTeammate();

					const auto visit = [&](RE::Actor* a_candidate) {
						if (!a_candidate || a_candidate == target.get()) {
							return;
						}
						if (a_candidate->IsDead()) {
							return;
						}
						if (a_candidate->NotShowOnStealthMeter()) {
							return;
						}
						++walked;

						// Hostility of the candidate to the target, asked at
						// most once per candidate and only when a rule needs it.
						// Two sources: the engine's own hostility query, and the
						// candidate already being in combat with the target (an
						// animal that is attacking counts as hostile whether or
						// not its faction relation says so).
						int hostile = -1;
						const auto isHostile = [&]() {
							if (hostile < 0) {
								const bool inCombatWithTarget =
									a_candidate->GetActorRuntimeData().currentCombatTarget.native_handle() == a_handle.native_handle();
								hostile = (inCombatWithTarget || a_candidate->IsHostileToActor(target.get())) ? 1 : 0;
							}
							return hostile == 1;
						};

						if (skipAllies) {
							if (targetOnPlayerSide && a_candidate->IsPlayerTeammate()) {
								++skippedAlly;
								return;
							}
							if ((a_candidate->IsCommandedActor() || a_candidate->IsSummoned()) && !isHostile()) {
								++skippedAlly;
								return;
							}
						}

						if (a_excludeKeyword && a_candidate->HasKeyword(a_excludeKeyword)) {
							if (!sparesHostile || !isHostile()) {
								++skippedKeyword;
								return;
							}
						}

						const auto candidateLevel = a_candidate->RequestDetectionLevel(
							target.get(), RE::DETECTION_PRIORITY::kNormal);
						if (candidateLevel > reading.level) {
							reading.level = candidateLevel;
						}
						if (candidateLevel >= 0) {
							++reading.detectingCount;
						}
					};

					RE::ProcessLists::GetSingleton()->ForEachHighActor(
						[&](RE::Actor* a_candidate) {
							visit(a_candidate);
							return RE::BSContainer::ForEachResult::kContinue;
						});
					ok = true;
				} catch (...) {
					spdlog::error("DetectionRead: side-aware aggregate threw for actor (0x{:08X}) - "
								  "cache left as it was.",
						target->GetFormID());
				}
			}

			if (ok && g_logFilteredWalk) {
				spdlog::info("DetectionRead: side walk target=0x{:08X} flags={} walked={} skippedAlly={} "
							 "skippedKeyword={} detecting={} level={}",
					target->GetFormID(), a_key.flags, walked, skippedAlly, skippedKeyword,
					reading.detectingCount, reading.level);
			}

			std::lock_guard lock(g_sideLock);
			auto            it = g_sideCache.find(a_key);
			if (it == g_sideCache.end()) {
				return;
			}

			it->second.pending = false;
			if (ok) {
				it->second.next      = reading;
				it->second.nextReady = true;
			}
		}

		// Shared by both Filtered natives. a_wantLevel says which half of the
		// pair this call is; see SideEntry for the boundary rule.
		bool ReadCacheSide(RE::Actor* a_actor, RE::BGSKeyword* a_excludeKeyword, std::int32_t a_flags,
			bool a_wantLevel, std::int32_t& a_outLevel, std::uint32_t& a_outCount)
		{
			if (!a_actor) {
				return false;
			}

			if (a_flags < 0 || (a_flags & ~kKnownFlags) != 0) {
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
			const SideKey key{ handle.native_handle(), keywordID, a_flags };

			bool warm      = false;
			bool needsTask = false;
			{
				std::lock_guard lock(g_sideLock);
				auto&           entry = g_sideCache[key];
				entry.handle          = handle;

				bool& thisHalfRead = a_wantLevel ? entry.levelReadOnPublished : entry.countReadOnPublished;
				if (entry.nextReady && (!entry.warm || thisHalfRead)) {
					entry.published            = entry.next;
					entry.warm                 = true;
					entry.nextReady            = false;
					entry.levelReadOnPublished = false;
					entry.countReadOnPublished = false;
				}
				// thisHalfRead is a reference into the entry, so it names the
				// right flag again after the reset above.
				thisHalfRead = true;

				if (entry.warm) {
					a_outLevel = entry.published.level;
					a_outCount = entry.published.detectingCount;
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
						task->AddTask([handle, a_excludeKeyword, key]() {
							RefreshOneSide(handle, a_excludeKeyword, key);
						});
					} catch (...) {
						spdlog::error("DetectionRead: AddTask threw queuing a side-aware refresh for actor "
									  "(0x{:08X}) - will retry on the next call.",
							a_actor->GetFormID());
						std::lock_guard lock(g_sideLock);
						auto            it = g_sideCache.find(key);
						if (it != g_sideCache.end()) {
							it->second.pending = false;
						}
					}
				} else {
					spdlog::error("DetectionRead: no task interface available - cannot queue a side-aware refresh.");
					std::lock_guard lock(g_sideLock);
					auto            it = g_sideCache.find(key);
					if (it != g_sideCache.end()) {
						it->second.pending = false;
					}
				}
			}

			return warm;
		}

		// Lodestone.GetHighestDetectionLevelFiltered(Actor, Keyword, Int) -> Int
		std::int32_t GetHighestDetectionLevelFiltered(RE::StaticFunctionTag*, RE::Actor* a_actor,
			RE::BGSKeyword* a_excludeType, std::int32_t a_flags)
		{
			std::int32_t  level = -1;
			std::uint32_t count = 0;
			if (!ReadCacheSide(a_actor, a_excludeType, a_flags, true, level, count)) {
				return -1;
			}
			return level;
		}

		// Lodestone.GetDetectionObserverCountFiltered(Actor, Keyword, Int) -> Int
		std::int32_t GetDetectionObserverCountFiltered(RE::StaticFunctionTag*, RE::Actor* a_actor,
			RE::BGSKeyword* a_excludeType, std::int32_t a_flags)
		{
			std::int32_t  level = 0;
			std::uint32_t count = 0;
			if (!ReadCacheSide(a_actor, a_excludeType, a_flags, false, level, count)) {
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
		a_vm->RegisterFunction("GetHighestDetectionLevelFiltered", "Lodestone", GetHighestDetectionLevelFiltered);
		a_vm->RegisterFunction("GetDetectionObserverCountFiltered", "Lodestone", GetDetectionObserverCountFiltered);

		// Absent file or key leaves the walk log off. Flat reader: the key is
		// named so it cannot collide, see Config.h.
		Config::ForEachPair([](std::string_view a_key, std::string_view a_value) {
			if (a_key == "logdetectionfilter") {
				g_logFilteredWalk = (Config::Trim(a_value) == "1");
			}
		});

		spdlog::info("DetectionRead: natives registered (GetHighestDetectionLevel, GetDetectionObserverCount, "
					 "GetHighestDetectionLevelExcluding, GetDetectionObserverCountExcluding, "
					 "GetHighestDetectionLevelFiltered, GetDetectionObserverCountFiltered). "
					 "No hook - a cache refreshed via AddTask on the game thread, cold until the first reading lands. "
					 "Side walk log {}.",
			g_logFilteredWalk ? "ON" : "off");
		return true;
	}
}
