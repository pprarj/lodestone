// ChordClaim.cpp
// Lodestone - Shared SKSE framework
//
// Implementation of the chord claim. See ChordClaim.h for what is taken, what is
// not, and why.
//
// Phase L-H1

#include "ChordClaim.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <vector>

namespace Lodestone::Core::ChordClaim
{
	namespace
	{
		// The modifier bits, the same family as the SoulLedger toml and the
		// Agility Matters AM_Hotkey: 1 Shift, 2 Ctrl, 4 Alt.
		constexpr std::int32_t kShift = 1;
		constexpr std::int32_t kCtrl  = 2;
		constexpr std::int32_t kAlt   = 4;

		// DirectInput scan codes, BOTH variants of each modifier.
		constexpr std::uint32_t kLShift = 42;
		constexpr std::uint32_t kRShift = 54;
		constexpr std::uint32_t kLCtrl  = 29;
		constexpr std::uint32_t kRCtrl  = 157;
		constexpr std::uint32_t kLAlt   = 56;
		constexpr std::uint32_t kRAlt   = 184;

		// Slots for the six modifier keys, in the order above.
		constexpr std::size_t kModifierKeys = 6;

		constexpr std::size_t kMaxClaims = 32;

		struct Entry
		{
			std::int32_t key;
			std::int32_t modifiers;
			std::string  event;
		};

		std::mutex          g_lock;
		std::vector<Entry> g_claims;

		// The keys whose DOWN was taken and whose release has not arrived yet,
		// each holding the event name its down went to (empty = not taken).
		// Indexed by scan code. Touched under g_lock.
		std::array<std::string, 256> g_active{};

		// Held state of the six modifier keys, from the events the sink sees.
		// Touched by the dispatching thread only.
		std::array<bool, kModifierKeys> g_modifierHeld{};

		// Whether the sink is registered. GAME THREAD (message handlers).
		bool g_installed = false;

		int ModifierSlot(std::uint32_t a_code)
		{
			switch (a_code) {
			case kLShift: return 0;
			case kRShift: return 1;
			case kLCtrl:  return 2;
			case kRCtrl:  return 3;
			case kLAlt:   return 4;
			case kRAlt:   return 5;
			default:      return -1;
			}
		}

		// EXACT match, the same rule as AM_Hotkey.ModifiersMatch: each of the
		// three modifiers is down if and only if its bit is in the mask.
		std::int32_t HeldMask()
		{
			std::int32_t mask = 0;
			if (g_modifierHeld[0] || g_modifierHeld[1]) {
				mask |= kShift;
			}
			if (g_modifierHeld[2] || g_modifierHeld[3]) {
				mask |= kCtrl;
			}
			if (g_modifierHeld[4] || g_modifierHeld[5]) {
				mask |= kAlt;
			}
			return mask;
		}

		// A menu that pauses the game, a modal menu, or anything taking text
		// (the console): the press belongs to whoever has the screen.
		bool InGameplay()
		{
			if (auto* ui = RE::UI::GetSingleton(); ui) {
				if (ui->GameIsPaused() || ui->IsModalMenuOpen()) {
					return false;
				}
			}
			if (const auto* controlMap = RE::ControlMap::GetSingleton(); controlMap) {
				if (controlMap->GetRuntimeData().textEntryCount > 0) {
					return false;
				}
			}
			return true;
		}

		// Sends the claimant its event on the game thread. Arguments by value:
		// the sink's stack is gone by the time the task runs.
		void Notify(std::string a_event, const char* a_what, std::uint32_t a_key)
		{
			try {
				auto* task = SKSE::GetTaskInterface();
				if (!task) {
					spdlog::error("ChordClaim: no SKSE task interface - '{}' not sent.", a_event);
					return;
				}
				task->AddTask([event = std::move(a_event), what = std::string(a_what), key = a_key]() {
					auto* source = SKSE::GetModCallbackEventSource();
					if (!source) {
						spdlog::error("ChordClaim: no mod callback event source - '{}' not sent.", event);
						return;
					}
					SKSE::ModCallbackEvent modEvent{
						RE::BSFixedString(event.c_str()),
						RE::BSFixedString(what.c_str()),
						static_cast<float>(key),
						nullptr
					};
					source->SendEvent(&modEvent);
				});
			} catch (...) {
				spdlog::error("ChordClaim: AddTask threw - '{}' not sent.", a_event);
			}
		}

		class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static InputSink* GetSingleton()
			{
				static InputSink singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
				RE::BSTEventSource<RE::InputEvent*>*) override
			{
				// Nothing here may throw into the input dispatch loop.
				try {
					if (!a_event || !*a_event) {
						return RE::BSEventNotifyControl::kContinue;
					}

					// The list head is written through when the first event is
					// the one taken. Every sink behind this one is handed the same
					// pointer, so they see the shortened list.
					auto* head = const_cast<RE::InputEvent**>(a_event);

					RE::InputEvent* prev = nullptr;
					for (auto* event = *head; event;) {
						auto* const next = event->next;

						if (Take(event)) {
							// ev->next is left as it was: if the engine walks
							// the chain it built, the walk is unchanged.
							if (prev) {
								prev->next = next;
							} else {
								*head = next;
							}
						} else {
							prev = event;
						}

						event = next;
					}
				} catch (...) {
					spdlog::error("ChordClaim: the input sink threw - that batch was left as it was.");
				}

				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			InputSink()                            = default;
			InputSink(const InputSink&)            = delete;
			InputSink& operator=(const InputSink&) = delete;

			// Decides one event. True means unlink it.
			static bool Take(RE::InputEvent* a_event)
			{
				if (a_event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton ||
					a_event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
					return false;
				}

				const auto* button  = static_cast<const RE::ButtonEvent*>(a_event);
				const auto  code    = button->GetIDCode();
				const bool  pressed = button->Value() > 0.0f;

				if (const int slot = ModifierSlot(code); slot >= 0) {
					g_modifierHeld[static_cast<std::size_t>(slot)] = pressed;
					return false;
				}

				if (code == 0 || code > 255) {
					return false;
				}

				std::string owner;
				const char* what = nullptr;
				{
					std::lock_guard lock(g_lock);

					if (!pressed) {
						// The release of a press whose down was taken.
						if (g_active[code].empty()) {
							return false;
						}
						owner = std::move(g_active[code]);
						g_active[code].clear();
						what = "up";
					} else if (button->HeldDuration() > 0.0f) {
						// Auto-repeat of a press whose down was taken.
						return !g_active[code].empty();
					} else {
						// A fresh down. Taken only if a claim matches the
						// modifiers held now, and only in gameplay.
						if (g_claims.empty()) {
							return false;
						}
						const auto mask = HeldMask();
						const auto it   = std::find_if(g_claims.begin(), g_claims.end(), [&](const Entry& c) {
							return static_cast<std::uint32_t>(c.key) == code && c.modifiers == mask;
						});
						if (it == g_claims.end() || !InGameplay()) {
							return false;
						}
						g_active[code] = it->event;
						owner          = it->event;
						what           = "down";
					}
				}

				spdlog::debug("ChordClaim: took {} of key {} for '{}'.", what, code, owner);
				if (!owner.empty()) {
					Notify(std::move(owner), what, code);
				}
				return true;
			}
		};

		bool IsModifierKey(std::int32_t a_key)
		{
			return ModifierSlot(static_cast<std::uint32_t>(a_key)) >= 0;
		}

		// --- Papyrus natives ---

		std::int32_t ClaimChord(RE::StaticFunctionTag*, std::int32_t a_key, std::int32_t a_modifiers,
			RE::BSFixedString a_event)
		{
			return Claim(a_key, a_modifiers, a_event.c_str() ? a_event.c_str() : "");
		}

		bool ReleaseChord(RE::StaticFunctionTag*, std::int32_t a_key, std::int32_t a_modifiers,
			RE::BSFixedString a_event)
		{
			return Release(a_key, a_modifiers, a_event.c_str() ? a_event.c_str() : "");
		}

		RE::BSFixedString GetChordOwner(RE::StaticFunctionTag*, std::int32_t a_key, std::int32_t a_modifiers)
		{
			return RE::BSFixedString(Owner(a_key, a_modifiers).c_str());
		}
	}

	std::int32_t Claim(std::int32_t a_key, std::int32_t a_modifiers, const std::string& a_event)
	{
		if (a_key < 1 || a_key > 255 || IsModifierKey(a_key)) {
			spdlog::warn("ChordClaim: refused key {} - not a claimable keyboard scan code.", a_key);
			return kBadKey;
		}
		if (a_modifiers <= 0 || (a_modifiers & ~(kShift | kCtrl | kAlt)) != 0) {
			spdlog::warn("ChordClaim: refused modifiers {} for key {} - a chord needs a mask in 1..7.",
				a_modifiers, a_key);
			return kBadModifiers;
		}
		if (a_event.empty()) {
			spdlog::warn("ChordClaim: refused key {} + {} - no event name.", a_key, a_modifiers);
			return kNoEvent;
		}

		std::lock_guard lock(g_lock);

		if (!g_installed) {
			spdlog::warn("ChordClaim: refused key {} + {} - the input sink is not installed.", a_key, a_modifiers);
			return kNotInstalled;
		}

		for (const auto& claim : g_claims) {
			if (claim.key == a_key && claim.modifiers == a_modifiers) {
				if (claim.event == a_event) {
					return kAlreadyYours;
				}
				spdlog::warn("ChordClaim: refused key {} + {} for '{}' - held by '{}'.",
					a_key, a_modifiers, a_event, claim.event);
				return kHeldByOther;
			}
		}

		if (g_claims.size() >= kMaxClaims) {
			spdlog::warn("ChordClaim: refused key {} + {} - {} claims already.", a_key, a_modifiers, kMaxClaims);
			return kTableFull;
		}

		g_claims.push_back({ a_key, a_modifiers, a_event });
		spdlog::info("ChordClaim: key {} + modifiers {} claimed for '{}'.", a_key, a_modifiers, a_event);
		return kClaimed;
	}

	bool Release(std::int32_t a_key, std::int32_t a_modifiers, const std::string& a_event)
	{
		std::lock_guard lock(g_lock);

		const auto it = std::find_if(g_claims.begin(), g_claims.end(), [&](const Entry& c) {
			return c.key == a_key && c.modifiers == a_modifiers && c.event == a_event;
		});
		if (it == g_claims.end()) {
			return false;
		}

		g_claims.erase(it);
		if (a_key >= 0 && a_key < 256 && g_active[static_cast<std::size_t>(a_key)] == a_event) {
			// A press already taken has its release let through from now on:
			// the claimant is gone, so nobody is owed the up.
			g_active[static_cast<std::size_t>(a_key)].clear();
		}
		spdlog::info("ChordClaim: key {} + modifiers {} released by '{}'.", a_key, a_modifiers, a_event);
		return true;
	}

	std::string Owner(std::int32_t a_key, std::int32_t a_modifiers)
	{
		std::lock_guard lock(g_lock);

		const auto it = std::find_if(g_claims.begin(), g_claims.end(), [&](const Entry& c) {
			return c.key == a_key && c.modifiers == a_modifiers;
		});
		return it == g_claims.end() ? std::string() : it->event;
	}

	void TryInstall()
	{
		{
			std::lock_guard lock(g_lock);
			if (g_installed) {
				return;
			}
		}

		auto* manager = RE::BSInputDeviceManager::GetSingleton();
		if (!manager) {
			spdlog::warn("ChordClaim: the input device manager does not exist yet - not installed at this seam.");
			return;
		}

		// PREPEND, so this sink runs before the one that feeds Papyrus. The
		// sink list is walked front to back and a prepend made outside a
		// dispatch goes to the front (BSTEvent.h, PrependEventSink).
		manager->PrependEventSink(InputSink::GetSingleton());

		std::lock_guard lock(g_lock);
		g_installed = true;
		spdlog::info("ChordClaim: input sink installed at the front of the list.");
	}

	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}

		if (a_msg->type == SKSE::MessagingInterface::kInputLoaded ||
			a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			TryInstall();
			return;
		}

		// A load or a new game ends every claim, before the scripts of the new
		// session run. The claimant claims again where it registers for keys.
		if (a_msg->type == SKSE::MessagingInterface::kPreLoadGame ||
			a_msg->type == SKSE::MessagingInterface::kNewGame) {
			std::lock_guard lock(g_lock);
			if (!g_claims.empty()) {
				spdlog::info("ChordClaim: {} claim(s) dropped at the load.", g_claims.size());
			}
			g_claims.clear();
			for (auto& name : g_active) {
				name.clear();
			}
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("ChordClaim: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("ClaimChord", "Lodestone", ClaimChord);
		a_vm->RegisterFunction("ReleaseChord", "Lodestone", ReleaseChord);
		a_vm->RegisterFunction("GetChordOwner", "Lodestone", GetChordOwner);

		spdlog::info("ChordClaim: natives registered (ClaimChord, ReleaseChord, GetChordOwner).");
		return true;
	}
}
