// MagelightUIBackend.cpp
// Lodestone - Shared SKSE framework
//
// The Magelight UI half of the WebUI bridge. Everything vendor-specific about
// Magelight lives here and nowhere else; see WebUIBackend.h for what the seam is
// and WebUIBridge.h for what the bridge is.
//
// THIS FILE IS THE ONLY ONE THAT INCLUDES MagelightUI_API.h. The header is MIT
// and vendored unmodified in extern/magelightui-api/ with its license and origin.
//
// NOTHING HERE KNOWS ABOUT VIEW IDS, LISTENER OWNERSHIP OR MOD EVENTS. Those are
// the coordinator's, keyed by the string the consumer chose. This file deals in
// ViewId handles and slot indices and nothing else.
//
// WHAT IS DIFFERENT ABOUT THIS BACKEND, in three lines:
//   - Magelight is addressed as a MOD. Lodestone registers itself once, under the
//     slug "Lodestone", and every view it makes belongs to that registration.
//   - Focus is Magelight's UI MODE: one view across every mod owns it, the host
//     raises its own engine menu for it, and while it is held NO game input sink
//     sees a key. The panic chord is therefore read from the keyboard state, not
//     from a sink - see the panic section.
//   - Events arrive on the game thread; JS listeners arrive on the RENDER thread,
//     inside the game's Present, and must never touch SKSE's task queue directly.

#include "WebUIBackend.h"
#include "WebUIPanicKeys.h"

#include "MagelightUI_API.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace Lodestone::Core
{
	namespace
	{
		namespace ML = MAGELIGHT_API;

		// The oldest Magelight this backend was written against: 1.5.0, the floor
		// the Lifecast project measured. Everything used here (UI-mode reclaim and
		// cancel from 1.1.0, escape capture from 0.28.0, PostGameTask from 0.30.1)
		// is older than that. The newest it was measured on is 1.6.0.
		constexpr std::uint32_t kMinHostVersion = ML::PackVersion(1, 5, 0);

		// The slug the host knows this plugin by. [A-Za-z0-9_.-], unique per process.
		constexpr const char* kModSlug = "Lodestone";

		// Written by Probe() and HandleSKSEMessage() on the main thread during load,
		// read from the Papyrus VM thread afterwards. Null means Magelight UI is not
		// installed (or too old), which is a supported state and not an error.
		const ML::MagelightApi4* g_api = nullptr;

		// Zero until RegisterMod succeeds. GAME THREAD (and load-time main thread).
		ML::ModId g_mod = 0;

		// --- The views this backend made ------------------------------------------
		//
		// NO LOCK. Everything that touches it runs on the main game thread: the view
		// operations arrive there from the coordinator, and the host delivers its
		// mod events there (CallbackThread::GameThread, the default). The panic
		// thread and the render-thread listener do not read it.
		struct ViewRecord
		{
			ML::ViewId handle = 0;

			// OBSERVED: set from the UIModeEntered / UIModeExited events, never from
			// what SetFocus asked for.
			bool focused = false;

			// RequestUIMode was accepted (Ok) and neither UIModeEntered nor
			// UIModeRefused has answered yet.
			bool pending = false;
		};

		std::vector<ViewRecord> g_views;

		ViewRecord* Find(ML::ViewId a_view)
		{
			for (auto& record : g_views) {
				if (record.handle == a_view) {
					return &record;
				}
			}
			return nullptr;
		}

		// --- Registration ---------------------------------------------------------

		void OnEvent(const ML::EventData* a_event, void*);
		void OnLog(int a_level, const char* a_message, void*);

		// Registers Lodestone with the host if it has not been yet. GAME THREAD or
		// the load-time main thread.
		//
		// AN EARLY "NOT READY" IS EXPECTED AND IS NOT AN ERROR. RegisterMod can answer
		// NotReady before the renderer exists; the caller says so with a_quiet and
		// the registration is tried again at kDataLoaded and at the first
		// CreateView.
		bool EnsureRegistered(bool a_quiet)
		{
			if (g_mod != 0) {
				return true;
			}
			if (!g_api) {
				return false;
			}

			ML::ModDesc desc{};
			desc.size            = sizeof(desc);
			desc.modId           = kModSlug;
			desc.displayName     = "Lodestone";
			desc.modVersion      = 0;  // for the host's logs only
			desc.minHostVersion  = kMinHostVersion;
			desc.callbackThread  = ML::CallbackThread::GameThread;
			desc.onEvent         = &OnEvent;
			desc.onLog           = &OnLog;
			desc.user            = nullptr;
			desc.sessionName     = nullptr;  // an isolated storage jar, named by the slug

			ML::ModId  mod    = 0;
			const auto result = g_api->RegisterMod(&desc, &mod);
			if (result == ML::Result::Ok && mod != 0) {
				g_mod = mod;
				spdlog::info("MagelightUIBackend: registered with Magelight UI {} as '{}'.",
					g_api->hostVersion ? g_api->hostVersion : "?", kModSlug);
				return true;
			}

			if (a_quiet && result == ML::Result::NotReady) {
				spdlog::info("MagelightUIBackend: Magelight UI is not ready to register a mod yet - "
							 "trying again later.");
			} else {
				spdlog::warn("MagelightUIBackend: RegisterMod answered {}.", static_cast<int>(result));
			}
			return false;
		}

		// --- The panic chord ------------------------------------------------------
		//
		// THE PLAYER'S WAY OUT, AND NO CONSUMER CAN TURN IT OFF.
		//
		// WHY NOT THE INPUT SINK THE PRISMA BACKEND USES. Under Magelight's UI mode
		// no game input sink sees a key; the host takes the keyboard for the page.
		// So the chord is read the one way that does not pass through the game's
		// input: the operating system's keyboard state, GetAsyncKeyState. The host
		// documents that call as exempt for the DLL that registered the mod while
		// that mod's own view holds UI mode, and Lodestone is that DLL.
		//
		// A POLL, NOT AN EVENT, and only while a view of this bridge holds UI mode:
		// the thread sleeps otherwise.
		//
		// IT ONLY RELEASES. IT NEVER TOGGLES, for the reason the Prisma sink gives.
		//
		// THE PRESS THAT GAVE THE FOCUS MUST NOT TAKE IT BACK. The key that brought
		// the panel up reaches the page about 6 ms after UIModeEntered, and a chord
		// that is still held when the mode is entered would read as complete at once.
		// So the chord is ARMED only after it has been seen NOT complete once since
		// the mode was entered; a press that began before the mode, and is still down,
		// never releases anything. A deliberate chord starts from "not complete" and
		// fires on the transition.

		constexpr auto kPanicPollInterval = std::chrono::milliseconds(15);

		// Virtual-key codes of the two halves of the chord, zero until installed.
		std::atomic<std::uint32_t> g_chordVkFirst{ 0 };
		std::atomic<std::uint32_t> g_chordVkSecond{ 0 };

		// Whether a view of this bridge holds UI mode right now. Written on the game
		// thread, read by the panic thread.
		std::atomic<bool> g_uiHeld{ false };

		// Bumped at every UIModeEntered, so the panic thread re-arms from scratch
		// for each entry, including a switch from one view to another.
		std::atomic<std::uint32_t> g_entryGeneration{ 0 };

		std::atomic<bool> g_panicStarted{ false };

		// DirectInput scan code (what WebUIPanicKeys carries) to a Win32 virtual key.
		// Codes from 0x80 up are the extended keys (0xE0 prefix).
		std::uint32_t VirtualKeyFromScan(std::uint32_t a_scan)
		{
			const UINT scan = a_scan >= 0x80u ? (0xE000u | (a_scan & 0x7Fu)) : a_scan;
			return static_cast<std::uint32_t>(::MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX));
		}

		bool KeyDown(std::uint32_t a_vk)
		{
			return a_vk != 0 && (::GetAsyncKeyState(static_cast<int>(a_vk)) & 0x8000) != 0;
		}

		// Whether the game window is the foreground window. The OS key state is
		// global; a chord typed in another application must not release the panel.
		bool GameIsForeground()
		{
			const HWND foreground = ::GetForegroundWindow();
			if (!foreground) {
				return false;
			}
			DWORD pid = 0;
			::GetWindowThreadProcessId(foreground, &pid);
			return pid == ::GetCurrentProcessId();
		}

		// Drops UI mode from this bridge. GAME THREAD ONLY - posted there by the
		// panic thread. Silent when nothing of this bridge holds it.
		void ReleaseByChord()
		{
			if (!g_api || g_mod == 0) {
				return;
			}

			bool holding = g_api->GetUIModeOwner() == g_mod;
			for (const auto& record : g_views) {
				holding = holding || record.focused;
			}
			if (!holding) {
				return;
			}

			g_api->ReleaseUIMode(g_mod);
			spdlog::info("MagelightUIBackend: panic chord pressed - released UI mode.");
		}

		void PanicLoop()
		{
			bool          armed      = false;
			std::uint32_t generation = 0;

			for (;;) {
				std::this_thread::sleep_for(kPanicPollInterval);

				try {
					if (!g_uiHeld.load(std::memory_order_acquire)) {
						armed = false;
						continue;
					}

					const auto entry = g_entryGeneration.load(std::memory_order_acquire);
					if (entry != generation) {
						generation = entry;
						armed      = false;
					}

					if (!GameIsForeground()) {
						continue;
					}

					const bool complete = KeyDown(g_chordVkFirst.load(std::memory_order_acquire)) &&
					                      KeyDown(g_chordVkSecond.load(std::memory_order_acquire));
					if (!complete) {
						armed = true;
						continue;
					}

					if (!armed) {
						continue;
					}
					armed = false;

					if (auto* task = SKSE::GetTaskInterface()) {
						task->AddTask([]() { ReleaseByChord(); });
					}
				} catch (...) {
					spdlog::error("MagelightUIBackend: the panic chord poll threw - that pass was skipped.");
				}
			}
		}

		// Reads the chord and starts the poll thread. Idempotent. Returns whether a
		// chord is installed: no chord, no focus.
		bool EnsurePanicChord()
		{
			if (g_panicStarted.load(std::memory_order_acquire)) {
				return true;
			}

			const auto          chord = WebUIPanicKeys::Get("MagelightUIBackend");
			const std::uint32_t vk1   = VirtualKeyFromScan(chord.first);
			const std::uint32_t vk2   = VirtualKeyFromScan(chord.second);
			if (vk1 == 0 || vk2 == 0) {
				spdlog::error("MagelightUIBackend: scan codes 0x{:02X} + 0x{:02X} do not map to virtual "
							  "keys - the panic chord is not installed, and focus will be refused.",
					chord.first, chord.second);
				return false;
			}

			g_chordVkFirst.store(vk1, std::memory_order_release);
			g_chordVkSecond.store(vk2, std::memory_order_release);

			bool expected = false;
			if (g_panicStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
				std::thread(&PanicLoop).detach();
				spdlog::info("MagelightUIBackend: panic chord installed on scan codes 0x{:02X} + 0x{:02X} "
							 "(virtual keys 0x{:02X} + 0x{:02X}), read from the keyboard state.",
					chord.first, chord.second, vk1, vk2);
			}
			return true;
		}

		// --- Events and log lines from the host -----------------------------------

		void SetFocusedView(ML::ViewId a_view)
		{
			for (auto& record : g_views) {
				const bool want = a_view != 0 && record.handle == a_view;
				if (want) {
					record.pending = false;
				}
				if (record.focused == want) {
					continue;
				}
				record.focused = want;

				try {
					WebUIBackendCallbacks::FocusChanged(WebUIBackends::MagelightUI(), record.handle, want);
				} catch (...) {
					spdlog::error("MagelightUIBackend: reporting a focus change threw - the bridge's view "
								  "of who holds focus is now stale.");
				}
			}
		}

		void OnEvent(const ML::EventData* a_event, void*)
		{
			if (!a_event) {
				return;
			}

			try {
				switch (a_event->type) {
				case ML::Event::ViewDomReady:
					if (Find(a_event->view)) {
						// Escape belongs to the page: without this, Escape leaves UI mode
						// and HIDES the view. The player's way out is the panic chord.
						if (g_api) {
							g_api->SetEscapeCapture(a_event->view, true);
						}
						WebUIBackendCallbacks::ViewReady(WebUIBackends::MagelightUI(), a_event->view);
					}
					break;

				case ML::Event::ViewReloaded:
					if (g_api && Find(a_event->view)) {
						g_api->SetEscapeCapture(a_event->view, true);
					}
					break;

				case ML::Event::ViewLoadFailed:
					spdlog::error("MagelightUIBackend: a view failed to load - {}",
						a_event->detail ? a_event->detail : "");
					break;

				case ML::Event::UIModeEntered:
					g_entryGeneration.fetch_add(1, std::memory_order_acq_rel);
					g_uiHeld.store(Find(a_event->view) != nullptr, std::memory_order_release);
					SetFocusedView(a_event->view);
					break;

				case ML::Event::UIModeExited:
					// Whoever left, nothing of this bridge holds UI mode afterwards:
					// there is one holder across every mod.
					g_uiHeld.store(false, std::memory_order_release);
					SetFocusedView(0);
					break;

				case ML::Event::UIModeRefused:
					for (auto& record : g_views) {
						record.pending = false;
					}
					spdlog::warn("MagelightUIBackend: the host refused UI mode - {}",
						a_event->detail ? a_event->detail : "");
					break;

				case ML::Event::FocusDenied:
					spdlog::info("MagelightUIBackend: another mod asked for UI mode while a view of this "
								 "bridge holds it - {}",
						a_event->detail ? a_event->detail : "");
					break;

				case ML::Event::RenderDead:
					spdlog::error("MagelightUIBackend: Magelight UI disabled its overlay for this session.");
					break;

				default:
					break;
				}
			} catch (...) {
				spdlog::error("MagelightUIBackend: an event handler threw.");
			}
		}

		void OnLog(int a_level, const char* a_message, void*)
		{
			try {
				const char* message = a_message ? a_message : "";
				if (a_level >= 2) {
					spdlog::error("MagelightUI: {}", message);
				} else if (a_level == 1) {
					spdlog::warn("MagelightUI: {}", message);
				} else {
					spdlog::info("MagelightUI: {}", message);
				}
			} catch (...) {
			}
		}

		// --- JS listeners ---------------------------------------------------------
		//
		// THE HOST CALLS THESE ON ITS RENDER THREAD, mid-frame, inside the game's
		// Present. The coordinator's mod-event path takes SKSE's task queue, which
		// the header forbids from there (on VR the post can wait forever on a job
		// that waits for the frame). So the argument is copied, boxed, and handed to
		// PostGameTask, which the host posts for us from a thread of its own.
		//
		// The slot index travels in the user pointer; the callback carries the view
		// and the argument, and the argument is raw (not wrapped).
		struct ListenerBox
		{
			std::size_t slot = 0;
			std::string argument;
		};

		void ListenerOnGameThread(void* a_box)
		{
			std::unique_ptr<ListenerBox> box(static_cast<ListenerBox*>(a_box));
			try {
				WebUIBackendCallbacks::ListenerFired(box->slot, box->argument.c_str());
			} catch (...) {
				spdlog::error("MagelightUIBackend: a JS listener threw on slot {} - the mod event was not "
							  "sent.",
					box->slot);
			}
		}

		void OnListener(ML::ViewId, const char* a_argument, void* a_user)
		{
			try {
				auto* box = new ListenerBox{ static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(a_user)),
					a_argument ? a_argument : "" };
				if (!g_api || g_api->PostGameTask(&ListenerOnGameThread, box) != ML::Result::Ok) {
					delete box;
				}
			} catch (...) {
			}
		}

		// --- Releasing and destroying ---------------------------------------------

		// Gives UI mode back if THIS view holds it or has asked for it. GAME THREAD.
		// Returns whether anything was asked of the host.
		bool ReleaseIfHolding(ViewRecord& a_record, const char* a_why)
		{
			if (!g_api || g_mod == 0) {
				return false;
			}

			if (a_record.pending) {
				// A request not yet run can be withdrawn, and the header wants that
				// BEFORE a release. Denied means the entry already ran and its
				// UIModeEntered is on the way: leave through the release below.
				if (g_api->CancelUIModeRequest(g_mod) == ML::Result::Ok) {
					a_record.pending = false;
					spdlog::info("MagelightUIBackend: UI mode request withdrawn - {}.", a_why);
					return true;
				}
			} else if (!a_record.focused) {
				return false;
			}

			g_api->ReleaseUIMode(g_mod);
			spdlog::info("MagelightUIBackend: UI mode released - {}.", a_why);
			return true;
		}

		// DestroyView is Busy for the view that holds UI mode, and the release that
		// precedes it is itself asynchronous, so the first attempt can still lose.
		// Retried from a thread of ours, 100 ms apart, ten times.
		void DestroyAttempt(ML::ViewId a_view, int a_left)
		{
			if (!g_api) {
				return;
			}

			const auto result = g_api->DestroyView(a_view);
			if (result == ML::Result::Busy && a_left > 0) {
				std::thread([a_view, a_left]() {
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					if (auto* task = SKSE::GetTaskInterface()) {
						task->AddTask([a_view, a_left]() { DestroyAttempt(a_view, a_left - 1); });
					}
				}).detach();
				return;
			}

			if (result != ML::Result::Ok && result != ML::Result::InvalidView) {
				spdlog::warn("MagelightUIBackend: DestroyView answered {}.", static_cast<int>(result));
			}
		}

		// Builds the absolute path of a view file: <game folder>\Data\Magelight\<path>.
		// Empty on a path that is not a plain relative path under that root.
		std::filesystem::path ResolveViewPath(const char* a_viewPath)
		{
			std::string relative = a_viewPath ? a_viewPath : "";
			for (auto& c : relative) {
				if (c == '/') {
					c = '\\';
				}
			}

			if (relative.empty() || relative.front() == '\\' || relative.find(':') != std::string::npos) {
				return {};
			}
			for (std::size_t pos = 0; pos <= relative.size();) {
				const auto next    = relative.find('\\', pos);
				const auto segment = relative.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
				if (segment == "..") {
					return {};
				}
				if (next == std::string::npos) {
					break;
				}
				pos = next + 1;
			}

			wchar_t   exe[MAX_PATH]{};
			const DWORD length = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
			if (length == 0 || length >= MAX_PATH) {
				return {};
			}

			// The consumer's string is UTF-8 on the wire; widen it before it becomes a path.
			const int wideLength = ::MultiByteToWideChar(CP_UTF8, 0, relative.c_str(), -1, nullptr, 0);
			if (wideLength <= 1) {
				return {};
			}
			std::wstring wide(static_cast<std::size_t>(wideLength - 1), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, relative.c_str(), -1, wide.data(), wideLength);

			return std::filesystem::path(exe).parent_path() / L"Data" / L"Magelight" / std::filesystem::path(wide);
		}

		// The path as UTF-8, which is what the host takes.
		std::string ToUtf8(const std::filesystem::path& a_path)
		{
			const std::wstring& wide = a_path.native();
			const int           size = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                nullptr, 0, nullptr, nullptr);
			if (size <= 0) {
				return {};
			}
			std::string utf8(static_cast<std::size_t>(size), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), size,
				nullptr, nullptr);
			return utf8;
		}

		// --- The backend ----------------------------------------------------------

		class MagelightUIBackend final : public IWebUIBackend
		{
		public:
			const char* Name() const override { return "MagelightUI"; }
			const char* DisplayName() const override { return "Magelight UI"; }

			// Phrased to be dropped straight into the coordinator's CreateView error.
			const char* ViewRootHint() const override
			{
				return "Data\\Magelight for Magelight UI (the view path is appended as given)";
			}

			// Magelight answers a direct request from an already-loaded
			// Magelight.dll, so presence is settled here. Registering as a mod is a
			// separate step that waits for the host (see EnsureRegistered).
			//
			// NO VIEW IS CREATED HERE, for the reason the Prisma backend gives.
			void Probe() override { Acquire(); }

			void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg) override
			{
				if (!a_msg) {
					return;
				}

				try {
					if (a_msg->type == SKSE::MessagingInterface::kInputLoaded) {
						Acquire();
						EnsureRegistered(true);
					} else if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
						EnsureRegistered(false);
					}
				} catch (...) {
					spdlog::error("MagelightUIBackend: handling an SKSE message threw.");
				}
			}

			bool IsAvailable() const override { return g_api != nullptr; }

			bool HasCapability(const char* a_capability) const override
			{
				if (!a_capability || !g_api) {
					return false;
				}

				const std::string_view capability(a_capability);

				// Two views holding focus independently: no. Magelight has one UI-mode
				// owner across every mod.
				if (capability == "focus-stack") {
					return false;
				}

				// One view can be given the mouse and keyboard: UI mode, with the
				// panic chord read from the keyboard state. See the panic section.
				if (capability == "view-focus") {
					return true;
				}

				// Both asked of the host, which answers for its own install:
				// SetViewOrder / RaiseView, and ShowInspector (needs the runtime's
				// inspector assets). The bridge itself exercises neither.
				if (capability == "view-order") {
					return g_api->QueryCapability("viewOrder") == 1;
				}
				if (capability == "inspector") {
					return g_api->QueryCapability("inspector") == 1;
				}

				return false;
			}

			// The view id is unused: Magelight names views per mod, and the consumer's
			// id is only the coordinator's key. The view is a fullscreen Panel (the
			// layer that may take UI mode - Hud and click-through views are refused
			// it), hidden until Show, transparent where the page draws nothing.
			ViewHandle CreateView(const char* a_viewId, const char* a_viewPath) override
			{
				if (!g_api || !EnsureRegistered(false)) {
					return 0;
				}

				const auto path = ResolveViewPath(a_viewPath);
				if (path.empty()) {
					spdlog::error("MagelightUIBackend: '{}' is not a plain relative path - a view path may "
								  "not be absolute, hold ':' or climb with '..'.",
						a_viewPath ? a_viewPath : "");
					return 0;
				}

				std::error_code ec;
				if (!std::filesystem::exists(path, ec)) {
					spdlog::error("MagelightUIBackend: no such view file: {}", path.string());
					return 0;
				}

				const std::string pathUtf8 = ToUtf8(path);
				if (pathUtf8.empty()) {
					spdlog::error("MagelightUIBackend: could not encode the view path.");
					return 0;
				}

				ML::ViewDesc desc{};
				desc.size         = sizeof(desc);
				desc.name         = a_viewId;
				desc.htmlPath     = pathUtf8.c_str();
				desc.anchor       = ML::Anchor::TopLeft;
				desc.fullscreen   = true;
				desc.clickThrough = false;
				desc.startVisible = false;
				desc.layer        = ML::Layer::Panel;
				desc.uiScale      = 0.0f;
				desc.onDomReady   = nullptr;  // the ViewDomReady event carries the same

				ML::ViewId view   = 0;
				const auto result = g_api->CreateViewEx(g_mod, &desc, &view);
				if (result != ML::Result::Ok || view == 0) {
					const char* why = g_api->GetLastErrorMessage(g_mod);
					spdlog::error("MagelightUIBackend: CreateViewEx answered {} - {}",
						static_cast<int>(result), why ? why : "");
					return 0;
				}

				if (!Find(view)) {
					g_views.push_back(ViewRecord{ view });
				}
				return view;
			}

			void DestroyView(ViewHandle a_view) override
			{
				if (!g_api) {
					return;
				}

				for (std::size_t i = 0; i < g_views.size(); ++i) {
					if (g_views[i].handle == a_view) {
						// Busy to destroy while it holds UI mode, and a capture
						// would also outlive the handle the chord can name.
						ReleaseIfHolding(g_views[i], "the view is being destroyed");
						g_views.erase(g_views.begin() + static_cast<std::ptrdiff_t>(i));
						break;
					}
				}

				DestroyAttempt(a_view, 10);
			}

			void Show(ViewHandle a_view) override
			{
				if (g_api) {
					g_api->ShowView(a_view, true);
				}
			}

			void Hide(ViewHandle a_view) override
			{
				if (!g_api) {
					return;
				}

				// A hidden view must not keep the keyboard.
				if (auto* record = Find(a_view)) {
					ReleaseIfHolding(*record, "the view is being hidden");
				}
				g_api->ShowView(a_view, false);
			}

			void Call(ViewHandle a_view, const char* a_function, const char* a_json) override
			{
				if (g_api) {
					g_api->InteropCall(a_view, a_function, a_json);
				}
			}

			void RegisterListener(ViewHandle a_view, const char* a_jsFunction, std::size_t a_slot) override
			{
				if (!g_api || a_slot >= kWebUIMaxListeners) {
					return;
				}

				const auto result = g_api->RegisterJSListenerEx(a_view, a_jsFunction, &OnListener,
					reinterpret_cast<void*>(static_cast<std::uintptr_t>(a_slot)));
				if (result != ML::Result::Ok) {
					const char* why = g_mod != 0 ? g_api->GetLastErrorMessage(g_mod) : "";
					spdlog::warn("MagelightUIBackend: RegisterJSListenerEx('{}') answered {} - {}",
						a_jsFunction ? a_jsFunction : "", static_cast<int>(result), why ? why : "");
				}
			}

			// --- Focus --------------------------------------------------------------
			//
			// Focus IS Magelight's UI mode. RequestUIMode only QUEUES the entry; the
			// truth comes back through UIModeEntered / UIModeRefused, which is what
			// reaches the coordinator through FocusChanged. Flags: Reclaim, so a
			// request right after our own release keeps the mode rather than losing it
			// to that exit. No Pause (a panel is an overlay) and text entry stays on
			// (a page may hold a text field).
			bool SetFocus(ViewHandle a_view) override
			{
				auto* record = Find(a_view);
				if (!g_api || g_mod == 0 || !record) {
					return false;
				}

				// No escape hatch, no focus - the condition the capability was
				// granted on.
				if (!EnsurePanicChord()) {
					spdlog::error("MagelightUIBackend: refusing focus - the panic chord is not installed, "
								  "and focus without a way out is what this backend may not give.");
					return false;
				}

				if (record->focused || record->pending) {
					return true;
				}

				const auto result = g_api->RequestUIMode(a_view, ML::kUIModeFlagReclaim);
				if (result == ML::Result::Busy) {
					spdlog::info("MagelightUIBackend: refusing focus - UI mode is held by another mod "
								 "(owner {}).",
						g_api->GetUIModeOwner());
					return false;
				}
				if (result != ML::Result::Ok) {
					const char* why = g_api->GetLastErrorMessage(g_mod);
					spdlog::warn("MagelightUIBackend: RequestUIMode answered {} - {}",
						static_cast<int>(result), why ? why : "");
					return false;
				}

				record->pending = true;
				return true;
			}

			// Safe on a view that does not hold focus, as the interface requires:
			// ReleaseIfHolding asks the host for nothing in that case.
			void ClearFocus(ViewHandle a_view) override
			{
				if (auto* record = Find(a_view)) {
					ReleaseIfHolding(*record, "focus cleared");
				}
			}

		private:
			// Resolves the API from an already-loaded Magelight.dll. Cheap, and
			// harmless to repeat.
			static void Acquire()
			{
				if (g_api) {
					return;
				}

				const auto* api = ML::RequestApi4();
				if (!api) {
					return;
				}

				if (api->hostVersionNumber < kMinHostVersion) {
					spdlog::info("MagelightUIBackend: Magelight UI {} is older than 1.5.0 - not used.",
						api->hostVersion ? api->hostVersion : "?");
					return;
				}

				g_api = api;
			}
		};

		MagelightUIBackend g_backend;
	}

	namespace WebUIBackends
	{
		IWebUIBackend* MagelightUI()
		{
			return &g_backend;
		}
	}
}
