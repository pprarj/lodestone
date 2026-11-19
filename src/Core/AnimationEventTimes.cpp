// AnimationEventTimes.cpp
// Lodestone - Shared SKSE framework
//
// Module: AnimationEventTimes (Core) - see AnimationEventTimes.h for why it
// exists.
//
// THE CLOCK. std::chrono::steady_clock, read by the hook at the same place and
// the same instant as the wall clock "t" that WebUIBridge sends to a page
// (WebUIBridge.cpp, OnPlayerAnimationEvent). Steady and not the wall clock
// because an age is a difference, and a wall clock can be set back or forward
// between the event and the question; steady_clock cannot. The two reads are
// one line apart, so the "t" in the per-event log line is the same moment.
//
// THE STATE. One record per tracked name (lower-cased, since the graph's
// spelling and the script's can differ in case): the last kHistory moments in a
// ring and a count since the name started being tracked. A name is tracked
// while at least one open request lists it; a name dropped by every request
// loses its record. Everything is guarded by g_lock, which the hook holds for a
// map lookup and two stores.
//
// THE INTERVAL KEEPS A SHORT HISTORY ON PURPOSE. A handler that arrives late
// can find that the first event has already happened again: the player drew
// the bow, released, and drew again before the release handler ran. "Last
// minus last" is then negative and meaningless. The interval is measured from
// the latest asFirst AT OR BEFORE the latest asSecond, which is the pair the
// late handler is about.

#include "AnimationEventTimes.h"

#include "WebUIBridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Lodestone::Core::AnimationEventTimes
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr std::size_t kMaxRequests        = 8;
		constexpr std::size_t kMaxNamesPerRequest = 32;
		constexpr std::size_t kHistory            = 8;

		// What the Float answers mean when there is no time to give.
		constexpr float kNever      = -1.0f;  // tracked, has not happened since
		constexpr float kNotTracked = -2.0f;  // no open request lists the name

		struct Request
		{
			std::string              owner;
			std::vector<std::string> keys;     // lower-cased
			std::string              spelled;  // the list as asked, for the log
		};

		struct Times
		{
			std::array<Clock::time_point, kHistory> at{};
			std::uint64_t                           count = 0;  // the newest is at[(count - 1) % kHistory]
		};

		std::mutex                             g_lock;
		std::vector<Request>                   g_requests;
		std::unordered_map<std::string, Times> g_times;
		std::atomic<bool>                      g_any{ false };

		std::string Lower(std::string_view a_text)
		{
			std::string out(a_text);
			for (auto& c : out) {
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return out;
		}

		std::string ToStd(const RE::BSFixedString& a_text)
		{
			const char* p = a_text.c_str();
			return p ? std::string(p) : std::string();
		}

		// Keeps exactly the names some open request lists. Names still listed
		// keep their history, so asking again with the same list loses nothing.
		// Called with g_lock held.
		void RebuildLocked()
		{
			for (const auto& request : g_requests) {
				for (const auto& key : request.keys) {
					g_times.try_emplace(key);
				}
			}
			for (auto it = g_times.begin(); it != g_times.end();) {
				const bool listed = std::any_of(g_requests.begin(), g_requests.end(), [&it](const Request& a_r) {
					return std::find(a_r.keys.begin(), a_r.keys.end(), it->first) != a_r.keys.end();
				});
				it = listed ? std::next(it) : g_times.erase(it);
			}
			g_any.store(!g_requests.empty(), std::memory_order_release);
		}

		// The end line: one per request that ends, with the count of each name.
		// Called with g_lock held, before the request leaves g_requests.
		void LogStopLocked(const Request& a_request, const char* a_reason)
		{
			std::string counts;
			for (const auto& key : a_request.keys) {
				const auto it = g_times.find(key);
				const auto n  = it == g_times.end() ? 0 : it->second.count;
				counts += std::format("{}{} {}", counts.empty() ? "" : ", ", key, n);
			}
			spdlog::info("AnimationEventTimes: '{}' stopped ({}); events seen: {}.", a_request.owner, a_reason,
				counts);
		}

		void DropAll(const char* a_reason)
		{
			std::scoped_lock lock(g_lock);
			if (g_requests.empty()) {
				return;
			}
			for (const auto& request : g_requests) {
				LogStopLocked(request, a_reason);
			}
			g_requests.clear();
			RebuildLocked();
		}

		Clock::time_point NewestLocked(const Times& a_times)
		{
			return a_times.at[(a_times.count - 1) % kHistory];
		}

		// Lodestone.RegisterPlayerAnimationEventTimes(String, String[]) -> Bool
		bool RegisterPlayerAnimationEventTimes(RE::StaticFunctionTag*, RE::BSFixedString a_owner,
			std::vector<RE::BSFixedString> a_events)
		{
			try {
				const std::string owner = ToStd(a_owner);

				if (!WebUIBridge::PlayerAnimationHookInstalled()) {
					spdlog::warn("AnimationEventTimes: RegisterPlayerAnimationEventTimes refused for '{}' - the "
								 "player's animation events cannot be heard in this session (see the WebUIBridge "
								 "hook line).",
						owner);
					return false;
				}
				if (owner.empty()) {
					spdlog::warn("AnimationEventTimes: RegisterPlayerAnimationEventTimes needs an owner name.");
					return false;
				}

				std::vector<std::string> keys;
				std::string              spelled;
				for (const auto& entry : a_events) {
					const std::string name = ToStd(entry);
					if (name.empty()) {
						continue;
					}
					std::string key = Lower(name);
					if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
						keys.push_back(std::move(key));
						spelled += spelled.empty() ? name : "," + name;
					}
				}
				if (keys.empty()) {
					spdlog::warn("AnimationEventTimes: RegisterPlayerAnimationEventTimes refused for '{}' - the event "
								 "list is empty.",
						owner);
					return false;
				}
				if (keys.size() > kMaxNamesPerRequest) {
					spdlog::warn("AnimationEventTimes: RegisterPlayerAnimationEventTimes refused for '{}' - {} event "
								 "names, the limit is {}.",
						owner, keys.size(), kMaxNamesPerRequest);
					return false;
				}

				std::scoped_lock lock(g_lock);

				for (auto& request : g_requests) {
					if (request.owner == owner) {
						request.keys    = std::move(keys);
						request.spelled = spelled;
						RebuildLocked();
						spdlog::info("AnimationEventTimes: '{}' replaced its list: {}.", owner, spelled);
						return true;
					}
				}
				if (g_requests.size() >= kMaxRequests) {
					spdlog::warn("AnimationEventTimes: RegisterPlayerAnimationEventTimes refused for '{}' - {} "
								 "requests are open, the limit.",
						owner, kMaxRequests);
					return false;
				}

				g_requests.push_back(Request{ owner, std::move(keys), spelled });
				RebuildLocked();
				spdlog::info("AnimationEventTimes: '{}' times the player's animation events: {}.", owner, spelled);
				return true;
			} catch (...) {
				spdlog::error("AnimationEventTimes: RegisterPlayerAnimationEventTimes threw.");
				return false;
			}
		}

		// Lodestone.UnregisterPlayerAnimationEventTimes(String) -> Bool
		bool UnregisterPlayerAnimationEventTimes(RE::StaticFunctionTag*, RE::BSFixedString a_owner)
		{
			try {
				const std::string owner = ToStd(a_owner);

				std::scoped_lock lock(g_lock);
				const auto       it = std::find_if(g_requests.begin(), g_requests.end(),
                    [&owner](const Request& a_r) { return a_r.owner == owner; });
				if (it == g_requests.end()) {
					spdlog::info("AnimationEventTimes: UnregisterPlayerAnimationEventTimes for '{}' - no such "
								 "request, nothing ended.",
						owner);
					return false;
				}
				LogStopLocked(*it, "unregistered");
				g_requests.erase(it);
				RebuildLocked();
				return true;
			} catch (...) {
				spdlog::error("AnimationEventTimes: UnregisterPlayerAnimationEventTimes threw.");
				return false;
			}
		}

		// Lodestone.GetPlayerAnimationEventAge(String) -> Float, seconds
		float GetPlayerAnimationEventAge(RE::StaticFunctionTag*, RE::BSFixedString a_event)
		{
			try {
				const auto        now = Clock::now();
				const std::string key = Lower(ToStd(a_event));

				std::scoped_lock lock(g_lock);
				const auto       it = g_times.find(key);
				if (it == g_times.end()) {
					return kNotTracked;
				}
				if (it->second.count == 0) {
					return kNever;
				}
				return static_cast<float>(std::chrono::duration<double>(now - NewestLocked(it->second)).count());
			} catch (...) {
				return kNotTracked;
			}
		}

		// Lodestone.GetPlayerAnimationEventInterval(String, String) -> Float,
		// seconds from the latest asFirst at or before the latest asSecond.
		float GetPlayerAnimationEventInterval(RE::StaticFunctionTag*, RE::BSFixedString a_first,
			RE::BSFixedString a_second)
		{
			try {
				const std::string firstKey  = Lower(ToStd(a_first));
				const std::string secondKey = Lower(ToStd(a_second));

				std::scoped_lock lock(g_lock);
				const auto       first  = g_times.find(firstKey);
				const auto       second = g_times.find(secondKey);
				if (first == g_times.end() || second == g_times.end()) {
					return kNotTracked;
				}
				if (first->second.count == 0 || second->second.count == 0) {
					return kNever;
				}

				const auto end  = NewestLocked(second->second);
				const auto kept = std::min<std::uint64_t>(first->second.count, kHistory);
				for (std::uint64_t back = 0; back < kept; ++back) {
					const auto at = first->second.at[(first->second.count - 1 - back) % kHistory];
					if (at <= end) {
						return static_cast<float>(std::chrono::duration<double>(end - at).count());
					}
				}
				return kNever;
			} catch (...) {
				return kNotTracked;
			}
		}

		// Lodestone.GetPlayerAnimationEventCount(String) -> Int
		std::int32_t GetPlayerAnimationEventCount(RE::StaticFunctionTag*, RE::BSFixedString a_event)
		{
			try {
				const std::string key = Lower(ToStd(a_event));

				std::scoped_lock lock(g_lock);
				const auto       it = g_times.find(key);
				if (it == g_times.end()) {
					return -1;
				}
				return static_cast<std::int32_t>(
					std::min<std::uint64_t>(it->second.count, (std::numeric_limits<std::int32_t>::max)()));
			} catch (...) {
				return -1;
			}
		}
	}

	bool Any()
	{
		return g_any.load(std::memory_order_acquire);
	}

	void Record(const char* a_tag, Clock::time_point a_at, double a_wallMs, bool a_logEach)
	{
		if (!a_tag || !*a_tag || !Any()) {
			return;
		}
		try {
			const std::string key = Lower(a_tag);
			std::uint64_t     count = 0;
			{
				std::scoped_lock lock(g_lock);
				const auto       it = g_times.find(key);
				if (it == g_times.end()) {
					return;
				}
				auto& times                    = it->second;
				times.at[times.count % kHistory] = a_at;
				count                          = ++times.count;
			}
			if (a_logEach) {
				spdlog::info("AnimationEventTimes: [time] '{}' t {:.3f} count {}.", a_tag, a_wallMs, count);
			}
		} catch (...) {
		}
	}

	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		// The script that asked runs again after a load or a new game and asks
		// again from OnPlayerLoadGame; a time from before the load would be a
		// time from another session. Every load ends the requests, the same
		// save included - the two cannot be told apart here.
		if (a_msg->type == SKSE::MessagingInterface::kPreLoadGame) {
			DropAll("a save is being loaded");
		} else if (a_msg->type == SKSE::MessagingInterface::kNewGame) {
			DropAll("a new game is starting");
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("AnimationEventTimes: null VM, cannot register natives.");
			return false;
		}

		// The fourth argument, callable from tasklets: run on the VM thread
		// without waiting for the next frame (see the header).
		a_vm->RegisterFunction("RegisterPlayerAnimationEventTimes", "Lodestone", RegisterPlayerAnimationEventTimes, true);
		a_vm->RegisterFunction("UnregisterPlayerAnimationEventTimes", "Lodestone", UnregisterPlayerAnimationEventTimes, true);
		a_vm->RegisterFunction("GetPlayerAnimationEventAge", "Lodestone", GetPlayerAnimationEventAge, true);
		a_vm->RegisterFunction("GetPlayerAnimationEventInterval", "Lodestone", GetPlayerAnimationEventInterval, true);
		a_vm->RegisterFunction("GetPlayerAnimationEventCount", "Lodestone", GetPlayerAnimationEventCount, true);

		spdlog::info("AnimationEventTimes: natives registered (5, no frame wait).");
		return true;
	}
}
