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
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("DetectionRead: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("GetHighestDetectionLevel", "Lodestone", GetHighestDetectionLevel);
		a_vm->RegisterFunction("GetDetectionObserverCount", "Lodestone", GetDetectionObserverCount);

		spdlog::info("DetectionRead: natives registered (GetHighestDetectionLevel, GetDetectionObserverCount). "
					 "No hook - a cache refreshed via AddTask on the game thread, cold until the first reading lands.");
		return true;
	}
}
