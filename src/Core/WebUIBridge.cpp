// WebUIBridge.cpp
// Lodestone - Shared SKSE framework
//
// The backend-neutral half of the WebUI bridge: the view table, the listener
// slot pool, the mod events, the main-thread dispatch, the single-holder focus
// policy, and all 27 natives. Nothing here names a vendor or includes a
// vendor's header.
//
// See WebUIBridge.h for why this module exists and why it is Core. See
// WebUIBackend.h for the seam, and PrismaUIBackend.cpp and MeridianUIBackend.cpp
// for the two sides of it.

#include "WebUIBridge.h"

#include "Config.h"
#include "WebUIBackend.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Lodestone::Core::WebUIBridge
{
	namespace
	{
		// --- State -------------------------------------------------------------

		// The backends this build has, in the order the choice walks them.
		//
		// MERIDIAN FIRST. The reasoning is not that it is better: it is under
		// active maintenance, it needs no Media Keys Fix, and its focus is
		// arbitrated rather than absent. Prisma has the installed base. The
		// order is an opinion and the author may invert it; what it may not be
		// is undefined, because then the answer would depend on load order.
		constexpr std::array<IWebUIBackend* (*)(), 2> kBackendOrder = {
			&WebUIBackends::MeridianUI,
			&WebUIBackends::PrismaUI,
		};

		// The chosen backend, or null. Resolved once - see Resolve().
		IWebUIBackend* g_active = nullptr;

		// Whether Resolve() has run. Distinct from g_active being null, which is
		// also the answer when it ran and found nothing.
		bool g_resolved = false;

		// The active backend, or null when none is usable.
		//
		// ONE BACKEND PER SESSION, AND THE CHOICE NEVER CHANGES AFTER IT IS
		// MADE. Two live backends would be the sum of two focus models, which is
		// the one thing this design refuses. Re-asking the order on every call
		// would do exactly that in slow motion: Prisma answers at kPostLoad and
		// Meridian only at kInputLoaded, so "first available" would hand out
		// Prisma early and Meridian later, in the same session, for the same
		// consumer.
		//
		// Availability is still re-read, because a backend can go away - a
		// shutdown callback drops Meridian's pointer - and a native must answer
		// its sentinel then rather than call into a dead browser.
		IWebUIBackend* Active()
		{
			return (g_active && g_active->IsAvailable()) ? g_active : nullptr;
		}

		// What Lodestone.ini's WebUIBackend key asked for. Empty means auto,
		// which is also what an absent file and an absent key mean.
		//
		// Read once, from Resolve(), because the choice is made once.
		std::string ReadRequestedBackend()
		{
			std::string requested;

			Config::ForEachPair([&requested](std::string_view a_key, std::string_view a_value) {
				if (a_key == "webuibackend") {
					requested = a_value;
				}
			});

			// Deliberately NOT scoped to a [WebUI] section: the reader is flat,
			// and the key is named so it cannot collide. See Config.h.
			return requested;
		}

		// Picks the backend for this session, once, and says so in one line.
		//
		// The line is the whole diagnostic surface of the choice: from outside
		// the process, "no panel" looks the same whether nothing is installed,
		// both are installed and one lost, the user asked for one that is not
		// there, or the bridge failed. It names the winner and, when there was
		// a contest, the loser.
		void Resolve()
		{
			if (g_resolved) {
				return;
			}
			g_resolved = true;

			const std::string requested = ReadRequestedBackend();

			// An explicit name is honoured or nothing is. Falling through to
			// the other backend would make "force this one" mean "prefer this
			// one", which is a different setting and not the one that was
			// written down.
			if (!requested.empty() && !Config::EqualsNoCase(requested, "auto")) {
				for (const auto& accessor : kBackendOrder) {
					auto* backend = accessor();
					if (Config::EqualsNoCase(requested, backend->Name())) {
						if (backend->IsAvailable()) {
							g_active = backend;
							spdlog::info("WebUIBridge: {} found - bridge active. Chosen by "
										 "WebUIBackend in Lodestone.ini.",
								backend->DisplayName());
						} else {
							spdlog::warn("WebUIBridge: Lodestone.ini asks for '{}', which is not "
										 "installed - bridge inactive. Set WebUIBackend to auto, or "
										 "remove the line, to use whichever backend is present.",
								requested);
						}
						return;
					}
				}

				// A typo must not silently turn the panel off for someone, so
				// this falls through to auto rather than refusing.
				spdlog::warn("WebUIBridge: Lodestone.ini asks for backend '{}', which this version "
							 "does not know - choosing automatically instead.",
					requested);
			}

			std::string alsoPresent;

			for (const auto& accessor : kBackendOrder) {
				auto* backend = accessor();
				if (!backend->IsAvailable()) {
					continue;
				}

				if (!g_active) {
					g_active = backend;
					continue;
				}

				if (!alsoPresent.empty()) {
					alsoPresent += ", ";
				}
				alsoPresent += backend->DisplayName();
			}

			if (!g_active) {
				// Not an error. See the header: this is the common case, and a
				// consumer asking WebUIAvailable() is expecting it.
				spdlog::info("WebUIBridge: no web UI backend present - bridge inactive, "
							 "natives return their sentinels.");
				return;
			}

			if (alsoPresent.empty()) {
				spdlog::info("WebUIBridge: {} found - bridge active.", g_active->DisplayName());
			} else {
				spdlog::info("WebUIBridge: {} found - bridge active. Also installed: {} - not used, "
							 "because one backend per session is deliberate. Set WebUIBackend in "
							 "Lodestone.ini to choose.",
					g_active->DisplayName(), alsoPresent);
			}
		}

		// Guards g_views and g_listeners. Both are reached from at least three
		// threads: the Papyrus VM (natives), the backend's own callback thread
		// (JS listeners and page-ready) and the main game thread (the task
		// queue).
		std::mutex g_mutex;

		// What Lodestone knows about one view.
		//
		// THE HANDLE IS NOT THE KEY, AND THIS IS THE WHOLE REASON FOR THE MAP.
		// A backend handle is 64 bits wide; the Papyrus Int is 32. Handing the
		// handle back to a consumer truncates it and corrupts it. The consumer
		// names its view with a string it chose, which also removes any question
		// of what to do with a handle across a save: there is nothing to keep.
		struct ViewRecord
		{
			// Which backend built this view, and therefore the one every later
			// operation on it has to go to.
			//
			// THIS FIELD IS THE INDIRECTION THE SPLIT WAS FOR. Reading the
			// active backend at operation time instead would be wrong the moment
			// a view outlives a change of backend, and it is what keeps the
			// natives below from having to know how many backends exist.
			//
			// Never null in a record that reached the map: it is filled at
			// reservation time, before the handle exists.
			IWebUIBackend* backend = nullptr;

			// Zero until the create task has run on the main thread. A record
			// with a zero handle is reserved, not usable.
			IWebUIBackend::ViewHandle handle = 0;

			// Set by the backend, through WebUIBackendCallbacks::ViewReady. A
			// Call before this point is dropped by the framework, so this is
			// what gates WebUICall.
			bool domReady = false;

			// Mirror of the last Show/Hide this module applied.
			//
			// WHY MIRRORED RATHER THAN ASKED. Asking the backend would mean a
			// call into it, and every call into a backend from this module goes
			// through the main-thread task queue - see DispatchToGame. A native
			// cannot wait for a queued answer without blocking the VM thread on
			// the game thread, which is a deadlock waiting for a bad day. The
			// mirror is exact for every transition this module made, and this
			// module is the only thing that can move a view it created.
			//
			// IT STARTS HIDDEN, AND IT DEFAULTED TO VISIBLE UNTIL 1.23.1. That
			// default was a straight lie for the window between CreateView and
			// the consumer's first Show: the backend hides a view at creation -
			// MeridianUIBackend calls SetBrowserVisible(false) and says so in the
			// log - while this field claimed it was on screen. So
			// WebUIIsViewVisible answered True and WebUIGetViewState answered 2,
			// "ready and visible", for a panel nobody had shown yet.
			//
			// The symptom a consumer saw was the other end of it: state 1,
			// "ready, hidden", was unreachable. A script waiting for 1 before
			// calling Show waited forever, because the view went from 0 straight
			// to 2.
			//
			// Nobody wrote a wrong transition; the initial value was wrong and
			// every transition after it was right, which is why Show and Hide
			// always behaved and only the first window lied.
			//
			// MEASURED ON ONE BACKEND, CONTRACTUAL ON THE OTHER, and the
			// difference is worth knowing before someone "corrects" this back.
			// Meridian is measured: it hides at creation and logs that it did.
			// PrismaUIBackend::CreateView hides nothing, and whether Prisma
			// shows a view the moment it exists was never measured here. True is
			// what Lodestone.psc promises for both - "a view becomes visible when
			// WebUIShow is called, not when it is built" - so if Prisma turns out
			// to create a visible view, the defect is that divergence, not this
			// field, and it gets fixed in the backend that has it.
			bool hidden = true;

			// Whether this view holds the game's mouse and keyboard.
			//
			// NOT A MIRROR OF INTENT, AND THAT IS THE DIFFERENCE FROM `hidden`
			// ABOVE. Visibility only ever changes because this module changed
			// it, so remembering what it asked for is exact. Focus does not
			// behave that way: a backend that arbitrates can hand it to somebody
			// else, the player can take it back with the backend's own panic
			// chord, and a view can be destroyed while holding it. None of those
			// pass through here.
			//
			// So this is written from WebUIBackendCallbacks::FocusChanged, which
			// a backend sends when it OBSERVES a change rather than when one is
			// requested. The cost is that it lags reality by one poll interval;
			// the alternative was a field that goes on claiming a consumer holds
			// focus that the player escaped ten minutes ago, which would refuse
			// every later request forever.
			bool focused = false;
		};

		std::unordered_map<std::string, ViewRecord> g_views;

		// --- JS listener slots --------------------------------------------------
		//
		// The pool is the coordinator's: a slot is a (view, mod event) pair, and
		// neither of those means anything to a backend. What the backend does
		// with the index it is handed - a per-slot function, a context pointer -
		// is its own business. See kWebUIMaxListeners in WebUIBackend.h for why
		// the count is a compile-time constant.
		struct ListenerSlot
		{
			bool        used = false;
			std::string viewId;
			std::string modEvent;
		};

		std::array<ListenerSlot, kWebUIMaxListeners> g_listeners{};

		// --- Helpers ------------------------------------------------------------

		// BSFixedString::c_str() returns null for a default-constructed string,
		// which is what a Papyrus None string arrives as.
		std::string ToStd(const RE::BSFixedString& a_str)
		{
			const char* raw = a_str.c_str();
			return raw ? std::string(raw) : std::string();
		}

		// Runs a_work on the main game thread.
		//
		// EVERY CALL INTO A BACKEND GOES THROUGH HERE, and the reason is not
		// symmetry. Natives run on the Papyrus VM thread and JS callbacks arrive
		// on the framework's own thread; neither is the thread the renderer
		// belongs to. The sibling project that established the CreateView timing
		// trap drives Prisma from an input sink, which is already the main
		// thread, so it never had to answer this question and its precedent does
		// not cover us.
		//
		// The consequence is deliberate and is stated in Lodestone.psc: a native
		// that mutates a view reports that the request was accepted, not that the
		// framework has finished acting on it.
		//
		// NOTHING ESCAPES THIS FUNCTION, and that is not defensive habit. Part of
		// its callers are backend callbacks, running on a vendor's thread - an
		// exception leaving here would unwind back into that vendor, which is the
		// same undefined behavior as letting one cross into the Papyrus VM.
		// AddTask is treated as able to throw because DetectionRead already
		// treats it that way in this plugin.
		void DispatchToGame(std::function<void()> a_work)
		{
			try {
				auto* task = SKSE::GetTaskInterface();
				if (!task) {
					spdlog::error("WebUIBridge: no SKSE task interface - request dropped.");
					return;
				}
				task->AddTask(std::move(a_work));
			} catch (...) {
				spdlog::error("WebUIBridge: AddTask threw - request dropped.");
			}
		}

		// Sends a mod event on the main game thread.
		//
		// Arguments are taken by value because the caller is usually a backend
		// callback thread, holding a const char* that does not outlive the call.
		void SendModEvent(std::string a_event, std::string a_arg)
		{
			if (a_event.empty()) {
				return;
			}

			DispatchToGame([event = std::move(a_event), arg = std::move(a_arg)]() {
				auto* source = SKSE::GetModCallbackEventSource();
				if (!source) {
					spdlog::error("WebUIBridge: no mod callback event source - '{}' not sent.", event);
					return;
				}

				SKSE::ModCallbackEvent modEvent{
					RE::BSFixedString(event.c_str()),
					RE::BSFixedString(arg.c_str()),
					0.0f,
					nullptr
				};
				source->SendEvent(&modEvent);
			});
		}

		// The mod events announcing that a view's page is ready.
		//
		// Fixed rather than chosen by the consumer, because a consumer that has
		// not created a view yet has nowhere to have told us a name. strArg
		// carries the view id, so one handler serves every view a mod owns.
		//
		// BOTH ARE SENT, WITH THE SAME strArg, FROM THE SAME POINT, and that is
		// the only way a mod event can be renamed at all. The name is wire
		// protocol: the consumer writes the string by hand into
		// RegisterForModEvent, so renaming the native that creates the view does
		// not reach it. A .pex built against 1.17.x keeps working, without
		// recompiling, for as long as the old name is still sent. The old one
		// goes away in the next internal major (2.0.0), not before, and not
		// before the one known consumer has migrated.
		constexpr const char* kViewReadyEvent           = "LodestoneWebUIViewReady";
		constexpr const char* kViewReadyEventDeprecated = "LodestonePrismaViewReady";

		// --- Focus ---------------------------------------------------------------

		// The capability name the focus natives gate on.
		//
		// "view-focus" AND NOT "focus", AND THE NAME IS THE WHOLE POINT.
		// "focus-stack" already exists and answers false on every backend, and a
		// capability called plain "focus" sitting next to it reads like the same
		// question shortened. It is not:
		//
		//   focus-stack   can TWO views hold focus independently?   false, always
		//   view-focus    can ONE view receive a click at all?      backend's answer
		//
		// A consumer that asks the first meaning the second is wrong in both
		// eras - it concluded "no input surface" before 1.22.0 because the
		// surface did not exist, and it concludes "they built it and did not wire
		// it up" after, because false is still the honest answer to the question
		// it actually asked.
		constexpr const char* kViewFocusCapability = "view-focus";

		// The view holding focus, or empty. CALLER HOLDS g_mutex.
		//
		// Scanned rather than cached in a variable of its own. The map holds a
		// handful of views and this runs on a player pressing a key, not on a
		// frame; a second copy of the same fact is a second thing to keep in
		// step with FocusChanged, and this module already learned that lesson
		// once with the handle-versus-key problem above.
		std::string FocusHolder()
		{
			for (const auto& entry : g_views) {
				if (entry.second.focused) {
					return entry.first;
				}
			}
			return {};
		}

		// Drops focus from whatever holds it, for a reason worth a log line.
		//
		// USED BY THE AUTOMATIC RELEASES, which is why it takes no view id: the
		// events that call it - a save being loaded, a menu that pauses the game
		// opening - are not about a particular view, they are about the player
		// no longer being in the panel.
		//
		// Safe when nothing holds focus, and silent then. These fire on ordinary
		// gameplay, so a line per menu opened would be log spam of the exact kind
		// this plugin's release log was cleaned up to avoid.
		void ReleaseFocus(const char* a_reason)
		{
			auto* backend = Active();
			if (!backend) {
				return;
			}

			IWebUIBackend::ViewHandle handle = 0;
			std::string               viewId;

			{
				std::scoped_lock lock(g_mutex);
				viewId = FocusHolder();
				if (viewId.empty()) {
					return;
				}
				handle = g_views[viewId].handle;
			}

			spdlog::info("WebUIBridge: releasing focus from view '{}' - {}.", viewId, a_reason);

			// The flag is NOT cleared here. It is cleared when the backend
			// reports the change, like every other transition - clearing it now
			// would be the mirror-of-intent this module refused, and would go
			// wrong the moment a backend declines to let go.
			DispatchToGame([backend, handle]() { backend->ClearFocus(handle); });
		}

		// Watches for a vanilla menu that pauses the game, and drops focus when
		// one opens.
		//
		// WHY "PAUSES THE GAME" AND NOT A LIST OF MENU NAMES. The obvious
		// implementation is a hand-written list - InventoryMenu, MapMenu,
		// Journal Menu, and so on - and it is wrong for the reason a listed path
		// is always wrong: it silently omits whatever nobody thought of,
		// including every menu added by a mod. RE::IMenu carries the answer
		// already. UI_MENU_FLAGS::kPausesGame is set on exactly the menus that
		// take the player out of the world, and RE::IMenu::PausesGame() reads it.
		//
		// WHY NOT ALSO kUsesCursor OR kUsesMenuContext, which sound closer to
		// "takes input": a web UI overlay is plausibly one of those itself. A
		// browser that registers as a cursor-using menu would clear its own focus
		// the moment it opened, and the panel would be dead on arrival with
		// nothing in the log naming the cause. kPausesGame is the narrow test
		// that cannot do that, because an overlay panel does not pause Skyrim -
		// being unpaused is the entire point of one.
		//
		// THAT REASONING IS AN ARGUMENT, NOT A MEASUREMENT, and the TESTPLAN
		// carries it as an item: open the panel with focus and confirm no
		// release line appears.
		class MenuWatch : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static MenuWatch* GetSingleton()
			{
				static MenuWatch singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent*                a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				// Nothing here may throw into the UI's dispatch loop.
				try {
					if (!a_event || !a_event->opening) {
						return RE::BSEventNotifyControl::kContinue;
					}

					auto* ui = RE::UI::GetSingleton();
					if (!ui) {
						return RE::BSEventNotifyControl::kContinue;
					}

					const auto menu = ui->GetMenu(a_event->menuName);
					if (menu && menu->PausesGame()) {
						ReleaseFocus("a menu that pauses the game opened");
					}
				} catch (...) {
					spdlog::error("WebUIBridge: the menu watch threw - focus was not released.");
				}

				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			MenuWatch()                            = default;
			MenuWatch(const MenuWatch&)            = delete;
			MenuWatch& operator=(const MenuWatch&) = delete;
		};

		// Whether the menu watch has been installed. Installed at most once, and
		// never removed: the sink outlives the process and removing it would only
		// create a window where a menu opens unwatched.
		bool g_menuWatchInstalled = false;

		void InstallMenuWatch()
		{
			if (g_menuWatchInstalled) {
				return;
			}

			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				spdlog::error("WebUIBridge: no UI singleton - focus will not be released automatically "
							  "when a menu opens. The backend's panic chord still works.");
				return;
			}

			ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuWatch::GetSingleton());
			g_menuWatchInstalled = true;
			spdlog::info("WebUIBridge: watching menu transitions to release view focus.");
		}

		// --- Natives -------------------------------------------------------------
		//
		// Every fallible body below is wrapped: a C++ exception crossing into the
		// Papyrus VM is undefined behavior and can take the game down. Sentinels
		// follow the framework convention - Bool -> false, Int -> -1,
		// String -> empty.

		// Lodestone.WebUIAvailable() -> Bool
		//
		// Whether a web UI backend is present and its API answered.
		//
		// A PROBE, NOT A FAILURE. Nothing is logged when the answer is False: it
		// is the expected answer on most load orders, and a consumer is meant to
		// call this to decide whether to offer a panel at all.
		//
		// Cannot fail - it reads one pointer and has no error path.
		bool WebUIAvailable(RE::StaticFunctionTag*)
		{
			return Active() != nullptr;
		}

		// Lodestone.WebUICreateView(String, String) -> Bool
		//
		// Reserves asViewId and asks the backend to build the view. asViewPath is
		// relative to the active backend's view root - the backend's own
		// convention, not this module's.
		//
		// Returns True when the request was accepted, NOT when the view is on
		// screen. The view becomes usable when the LodestoneWebUIViewReady mod
		// event fires for this id, or when WebUIIsViewReady() answers True.
		//
		// Idempotent: a second call with the same id answers True and creates
		// nothing.
		//
		// Returns False if no backend is present or either argument is empty.
		bool WebUICreateView(RE::StaticFunctionTag*, RE::BSFixedString a_viewId, RE::BSFixedString a_viewPath)
		{
			try {
				auto* backend = Active();
				if (!backend) {
					return false;
				}

				const std::string viewId   = ToStd(a_viewId);
				const std::string viewPath = ToStd(a_viewPath);
				if (viewId.empty() || viewPath.empty()) {
					spdlog::warn("WebUIBridge: WebUICreateView needs a non-empty view id and view path.");
					return false;
				}

				{
					std::scoped_lock lock(g_mutex);
					if (g_views.find(viewId) != g_views.end()) {
						spdlog::debug("WebUIBridge: view '{}' already exists - create ignored.", viewId);
						return true;
					}
					// Reserved before the task runs, so two calls in the same
					// frame cannot both queue a create. The backend is recorded
					// now, while it is known, rather than read again later.
					g_views.emplace(viewId, ViewRecord{ backend });
				}

				DispatchToGame([backend, viewId, viewPath]() {
					const IWebUIBackend::ViewHandle handle = backend->CreateView(viewId.c_str(), viewPath.c_str());

					// True when the id was released while this create was queued
					// - a destroy ran in between. The view has to be thrown
					// away, but not from under the lock: the backend's ready
					// callback arrives on its own thread and takes the same
					// mutex, so calling back into the framework while holding it
					// is a stall waiting to happen. The decision is made here;
					// the call is made below.
					bool orphaned = false;

					{
						std::scoped_lock lock(g_mutex);
						const auto       it = g_views.find(viewId);
						if (it == g_views.end()) {
							orphaned = true;
						} else if (handle == 0) {
							g_views.erase(it);
							spdlog::error("WebUIBridge: CreateView failed for '{}' - check that the view folder "
										  "exists under the backend's view root ({}) and holds the file named "
										  "by '{}'.",
								viewId, backend->ViewRootHint(), viewPath);
						} else {
							it->second.handle = handle;
							spdlog::info("WebUIBridge: view '{}' created from '{}'.", viewId, viewPath);
						}
					}

					if (orphaned && handle != 0) {
						backend->DestroyView(handle);
						spdlog::info("WebUIBridge: view '{}' was destroyed while being created - "
									 "the finished view was discarded.",
							viewId);
					}
				});

				return true;
			} catch (const std::exception& e) {
				spdlog::error("WebUIBridge: WebUICreateView threw - {}", e.what());
				return false;
			} catch (...) {
				spdlog::error("WebUIBridge: WebUICreateView threw an unknown exception.");
				return false;
			}
		}

		// Lodestone.WebUIIsViewReady(String) -> Bool
		//
		// Whether the view exists and its page has finished loading. This is the
		// poll-shaped answer to the same question LodestoneWebUIViewReady pushes.
		//
		// Returns False for an unknown id, which is indistinguishable from "not
		// ready yet" on purpose: both mean the same thing to a caller, which is
		// that WebUICall would do nothing. WebUIGetViewState tells them apart.
		bool WebUIIsViewReady(RE::StaticFunctionTag*, RE::BSFixedString a_viewId)
		{
			try {
				std::scoped_lock lock(g_mutex);
				const auto       it = g_views.find(ToStd(a_viewId));
				return it != g_views.end() && it->second.handle != 0 && it->second.domReady;
			} catch (...) {
				return false;
			}
		}

		// Lodestone.WebUICall(String, String, String) -> Bool
		//
		// Calls a JavaScript function on the view's JS interop surface, handing
		// it asJson as its single argument.
		//
		// Returns True when the request was accepted. Returns False for an
		// unknown id, for an empty function name, and for a view whose page is
		// not ready - the backend drops those calls, so reporting success would
		// be a lie.
		bool WebUICall(RE::StaticFunctionTag*, RE::BSFixedString a_viewId, RE::BSFixedString a_jsFunction, RE::BSFixedString a_json)
		{
			try {
				if (!Active()) {
					return false;
				}

				const std::string function = ToStd(a_jsFunction);
				if (function.empty()) {
					return false;
				}

				IWebUIBackend*            backend = nullptr;
				IWebUIBackend::ViewHandle handle  = 0;

				{
					std::scoped_lock lock(g_mutex);
					const auto       it = g_views.find(ToStd(a_viewId));
					if (it == g_views.end() || it->second.handle == 0 || !it->second.domReady) {
						return false;
					}
					backend = it->second.backend;
					handle  = it->second.handle;
				}

				DispatchToGame([backend, handle, function, json = ToStd(a_json)]() {
					backend->Call(handle, function.c_str(), json.c_str());
				});

				return true;
			} catch (...) {
				spdlog::error("WebUIBridge: WebUICall threw.");
				return false;
			}
		}

		// Shared body of Show and Hide. a_hide picks which.
		bool SetHidden(const RE::BSFixedString& a_viewId, bool a_hide)
		{
			if (!Active()) {
				return false;
			}

			IWebUIBackend*            backend = nullptr;
			IWebUIBackend::ViewHandle handle  = 0;

			{
				std::scoped_lock lock(g_mutex);
				const auto       it = g_views.find(ToStd(a_viewId));
				if (it == g_views.end() || it->second.handle == 0) {
					return false;
				}
				backend           = it->second.backend;
				handle            = it->second.handle;
				it->second.hidden = a_hide;
			}

			DispatchToGame([backend, handle, a_hide]() {
				if (a_hide) {
					backend->Hide(handle);
				} else {
					backend->Show(handle);
				}
			});


			return true;
		}

		// Lodestone.WebUIShow(String) -> Bool
		//
		// Returns True when the request was accepted, False for an unknown id or
		// a view that has not been built yet.
		bool WebUIShow(RE::StaticFunctionTag*, RE::BSFixedString a_viewId)
		{
			try {
				return SetHidden(a_viewId, false);
			} catch (...) {
				spdlog::error("WebUIBridge: WebUIShow threw.");
				return false;
			}
		}

		// Lodestone.WebUIHide(String) -> Bool
		//
		// Returns True when the request was accepted, False for an unknown id or
		// a view that has not been built yet.
		bool WebUIHide(RE::StaticFunctionTag*, RE::BSFixedString a_viewId)
		{
			try {
				return SetHidden(a_viewId, true);
			} catch (...) {
				spdlog::error("WebUIBridge: WebUIHide threw.");
				return false;
			}
		}

		// Lodestone.WebUIIsViewVisible(String) -> Bool
		//
		// True when the view exists, is ready, and is visible.
		//
		// THE SENSE IS INVERTED FROM THE 1.17.x NAME, ON PURPOSE. PrismaIsHidden
		// answered False both for a hidden-by-nobody unknown id and for a visible
		// view, which made the False useless on its own. Here every failure mode
		// collapses to False and the True means exactly one thing. That was free
		// to change because the old name had no callers at all - measured across
		// the tree in 2026-09-03.
		//
		// This reports the last visibility this module applied - see ViewRecord
		// for why it is mirrored rather than asked. It is exact for every Show
		// and Hide that was issued.
		//
		// WebUIGetViewState is what tells unknown, not-ready and hidden apart.
		bool WebUIIsViewVisible(RE::StaticFunctionTag*, RE::BSFixedString a_viewId)
		{
			try {
				std::scoped_lock lock(g_mutex);
				const auto       it = g_views.find(ToStd(a_viewId));
				return it != g_views.end() && it->second.handle != 0 && it->second.domReady &&
					   !it->second.hidden;
			} catch (...) {
				return false;
			}
		}

		// Lodestone.WebUIDestroyView(String) -> Bool
		//
		// Destroys the view and frees the id, along with every JS listener slot
		// registered against it.
		//
		// Returns True when the request was accepted, False for an unknown id.
		bool WebUIDestroyView(RE::StaticFunctionTag*, RE::BSFixedString a_viewId)
		{
			try {
				if (!Active()) {
					return false;
				}

				const std::string         viewId  = ToStd(a_viewId);
				IWebUIBackend*            backend = nullptr;
				IWebUIBackend::ViewHandle handle  = 0;

				{
					std::scoped_lock lock(g_mutex);
					const auto       it = g_views.find(viewId);
					if (it == g_views.end()) {
						return false;
					}
					backend = it->second.backend;
					handle  = it->second.handle;
					g_views.erase(it);

					// Slots outlive nothing. A freed id can be created again, and
					// a stale slot would fire a mod event for a view that is
					// gone.
					for (auto& slot : g_listeners) {
						if (slot.used && slot.viewId == viewId) {
							slot = ListenerSlot{};
						}
					}
				}

				if (handle != 0) {
					DispatchToGame([backend, handle]() { backend->DestroyView(handle); });
				}

				spdlog::info("WebUIBridge: view '{}' destroyed.", viewId);
				return true;
			} catch (...) {
				spdlog::error("WebUIBridge: WebUIDestroyView threw.");
				return false;
			}
		}

		// Lodestone.WebUIRegisterListener(String, String, String) -> Bool
		//
		// Makes the view's JS call to asJsFunction send the mod event asModEvent,
		// with the JS argument as strArg and numArg 0. How that name becomes
		// callable inside the page is the backend's business.
		//
		// The consumer receives it with RegisterForModEvent, like any other mod
		// event. It is delivered on the game thread, never on the thread the JS
		// callback arrived on - dispatching a mod event from inside a backend
		// callback is exactly the mistake this indirection exists to prevent.
		//
		// Registering the same (view, mod event) pair again reuses its slot
		// rather than taking a second one.
		//
		// Returns False if no backend is present, for an unknown or unbuilt view,
		// for an empty name, or when all listener slots are taken - see
		// kWebUIMaxListeners and WebUIGetListenerSlotsFree.
		bool WebUIRegisterListener(RE::StaticFunctionTag*, RE::BSFixedString a_viewId, RE::BSFixedString a_jsFunction, RE::BSFixedString a_modEvent)
		{
			try {
				if (!Active()) {
					return false;
				}

				const std::string viewId     = ToStd(a_viewId);
				const std::string jsFunction = ToStd(a_jsFunction);
				const std::string modEvent   = ToStd(a_modEvent);
				if (jsFunction.empty() || modEvent.empty()) {
					return false;
				}

				IWebUIBackend*            backend = nullptr;
				IWebUIBackend::ViewHandle handle  = 0;
				std::size_t               slot    = kWebUIMaxListeners;

				{
					std::scoped_lock lock(g_mutex);
					const auto       it = g_views.find(viewId);
					if (it == g_views.end() || it->second.handle == 0) {
						return false;
					}
					backend = it->second.backend;
					handle  = it->second.handle;

					// Reuse the slot if this pair is already registered. A
					// backend keeps one callback per (view, name), so a second
					// registration overwrites it there too - taking a second slot
					// here would leak one per re-registration, and re-registering
					// after a load is the documented pattern.
					for (std::size_t i = 0; i < g_listeners.size(); ++i) {
						if (g_listeners[i].used && g_listeners[i].viewId == viewId &&
							g_listeners[i].modEvent == modEvent) {
							slot = i;
							break;
						}
					}

					if (slot == kWebUIMaxListeners) {
						for (std::size_t i = 0; i < g_listeners.size(); ++i) {
							if (!g_listeners[i].used) {
								slot = i;
								break;
							}
						}
					}

					if (slot == kWebUIMaxListeners) {
						spdlog::error("WebUIBridge: all {} listener slots are in use - '{}' on view '{}' "
									  "was not registered.",
							kWebUIMaxListeners, jsFunction, viewId);
						return false;
					}

					g_listeners[slot] = ListenerSlot{ true, viewId, modEvent };
				}

				DispatchToGame([backend, handle, jsFunction, slot]() {
					backend->RegisterListener(handle, jsFunction.c_str(), slot);
				});

				spdlog::info("WebUIBridge: view '{}' JS '{}' -> mod event '{}' (slot {}).",
					viewId, jsFunction, modEvent, slot);
				return true;
			} catch (...) {
				spdlog::error("WebUIBridge: WebUIRegisterListener threw.");
				return false;
			}
		}

		// --- Natives added in 1.18.0 ---------------------------------------------

		// Lodestone.WebUIGetBackend() -> String
		//
		// Name of the active web UI backend. Empty string when none is present.
		//
		// THIS IS FOR THE LOG, NOT FOR CONTROL FLOW, and the point of having it at
		// all is that a player can paste the answer into a support thread. A
		// consumer that branches on it has moved the vendor name out of nine
		// function names and into a string compare that no compiler checks, which
		// is the coupling this whole surface was renamed to remove.
		// WebUIHasCapability is the one to ask when the answer decides something.
		RE::BSFixedString WebUIGetBackend(RE::StaticFunctionTag*)
		{
			try {
				auto* backend = Active();
				return backend ? RE::BSFixedString(backend->Name()) : RE::BSFixedString("");
			} catch (...) {
				return RE::BSFixedString("");
			}
		}

		// Lodestone.WebUIHasCapability(String) -> Bool
		//
		// Whether the active backend supports a named capability.
		//
		// AN UNKNOWN NAME RETURNS False AND LOGS NOTHING, and that is the property
		// that makes the vocabulary growable without a major. A consumer built
		// against an older Lodestone simply never asks about a name added later;
		// one built against a newer Lodestone asks an older DLL and gets False,
		// which is the correct answer there. Adding a name is therefore never a
		// breaking change - the mechanism is what could not be added later, so it
		// ships now with three names rather than later with thirty.
		//
		// Answered from what the backend can do, never from which backend it is -
		// which is why the answers live in the backend and not here.
		bool WebUIHasCapability(RE::StaticFunctionTag*, RE::BSFixedString a_capability)
		{
			try {
				auto* backend = Active();
				if (!backend) {
					return false;
				}

				return backend->HasCapability(ToStd(a_capability).c_str());
			} catch (...) {
				return false;
			}
		}

		// Lodestone.WebUIGetViewState(String) -> Int
		//
		// The whole state of one view in one call, which is what the three
		// separate Bool answers cannot give: each of them collapses several
		// situations into one False.
		//
		//   -1  unknown id, or no backend present
		//    0  created, page has not loaded yet
		//    1  ready, hidden
		//    2  ready, visible
		std::int32_t WebUIGetViewState(RE::StaticFunctionTag*, RE::BSFixedString a_viewId)
		{
			try {
				if (!Active()) {
					return -1;
				}

				std::scoped_lock lock(g_mutex);
				const auto       it = g_views.find(ToStd(a_viewId));
				if (it == g_views.end()) {
					return -1;
				}
				if (it->second.handle == 0 || !it->second.domReady) {
					return 0;
				}
				return it->second.hidden ? 1 : 2;
			} catch (...) {
				return -1;
			}
		}

		// --- Natives added in 1.22.0 ---------------------------------------------

		// Lodestone.WebUIFocusView(String) -> Bool
		//
		// Asks for the view to receive the game's mouse and keyboard.
		//
		// Returns True when the request was accepted, NOT when the view has
		// focus - same contract as every other native that mutates a view. Poll
		// WebUIIsViewFocused, or simply act when it answers True.
		//
		// FALSE HAS FOUR MEANINGS AND ALL FOUR ARE SYNCHRONOUS, which is what
		// makes this worth calling at all on a backend that cannot do it: no
		// backend, the backend answers false to "view-focus", the view is
		// unknown or not ready or hidden, or another view already holds focus.
		//
		// THE SINGLE-HOLDER RULE IS THIS MODULE'S, NOT THE BACKEND'S. At most one
		// view created through this bridge holds focus at a time, and the second
		// asker is refused rather than queued. That is not the same promise as
		// "focus-stack", which asks whether two could hold it INDEPENDENTLY and
		// answers false everywhere: here the answer is that they may not, on
		// purpose, and the refusal is how a consumer finds out.
		//
		// The rule is not enforced against consumers that do not come through
		// this bridge, and cannot be - a backend that arbitrates does that part
		// itself, and one that does not gives nothing to arbitrate with. That is
		// among the reasons a backend can answer false to the capability.
		bool WebUIFocusView(RE::StaticFunctionTag*, RE::BSFixedString a_viewId)
		{
			try {
				auto* backend = Active();
				if (!backend) {
					return false;
				}

				const std::string viewId = ToStd(a_viewId);

				if (!backend->HasCapability(kViewFocusCapability)) {
					// Logged, because this one is a consumer asking for
					// something the load order cannot give, and the answer is
					// otherwise indistinguishable from a bad view id. Not an
					// error: it is the expected answer on a backend that
					// declines focus, and the consumer is meant to degrade.
					spdlog::info("WebUIBridge: '{}' asked for focus and {} does not offer it - refused. "
								 "Ask WebUIHasCapability(\"view-focus\") first to skip this.",
						viewId, backend->DisplayName());
					return false;
				}

				IWebUIBackend::ViewHandle handle = 0;

				{
					std::scoped_lock lock(g_mutex);

					const auto it = g_views.find(viewId);
					if (it == g_views.end() || it->second.handle == 0 || !it->second.domReady ||
						it->second.hidden) {
						return false;
					}

					if (it->second.focused) {
						// Idempotent, like WebUICreateView: asking for what you
						// already have is not an error and changes nothing.
						return true;
					}

					const std::string holder = FocusHolder();
					if (!holder.empty()) {
						spdlog::info("WebUIBridge: '{}' asked for focus while '{}' holds it - refused. "
									 "One view at a time, by design.",
							viewId, holder);
						return false;
					}

					handle = it->second.handle;
				}

				// NOTHING IS RESERVED HERE, and the race that leaves is
				// deliberate. Two consumers asking in the same frame both pass
				// the check above and both dispatch; the backend decides, and
				// whichever one wins is the one FocusChanged reports. Reserving
				// optimistically would mean inventing an undo for the case where
				// the backend declines, and would put this module's guess ahead
				// of the backend's arbitration - which is exactly backwards on
				// the backend that arbitrates better than this module can.
				DispatchToGame([backend, handle, viewId]() {
					if (!backend->SetFocus(handle)) {
						spdlog::warn("WebUIBridge: the backend refused focus for view '{}'.", viewId);
					}
				});

				return true;
			} catch (...) {
				spdlog::error("WebUIBridge: WebUIFocusView threw.");
				return false;
			}
		}

		// Lodestone.WebUIClearFocus(String) -> Bool
		//
		// Gives the mouse and keyboard back to the game.
		//
		// TAKES A VIEW ID, AND THE ARGUMENT-LESS VERSION WOULD HAVE BEEN A BUG.
		// Every native here is reachable by every mod in the load order, so a
		// bare WebUIClearFocus() would let any consumer drop any other
		// consumer's focus, from a script that never mentioned it. This clears
		// focus only when the named view is the one holding it, and answers
		// False otherwise - which also makes "did I still have it" answerable
		// without a second call.
		//
		// The player's own escape route does not come through here: it is the
		// backend's panic chord, which no consumer can disable.
		bool WebUIClearFocus(RE::StaticFunctionTag*, RE::BSFixedString a_viewId)
		{
			try {
				auto* backend = Active();
				if (!backend) {
					return false;
				}

				const std::string         viewId = ToStd(a_viewId);
				IWebUIBackend::ViewHandle handle = 0;

				{
					std::scoped_lock lock(g_mutex);
					const auto       it = g_views.find(viewId);
					if (it == g_views.end() || !it->second.focused) {
						return false;
					}
					handle = it->second.handle;
				}

				// Cleared by FocusChanged when the backend reports it, not here.
				DispatchToGame([backend, handle]() { backend->ClearFocus(handle); });
				return true;
			} catch (...) {
				spdlog::error("WebUIBridge: WebUIClearFocus threw.");
				return false;
			}
		}

		// Lodestone.WebUIIsViewFocused(String) -> Bool
		//
		// Whether the view is receiving the mouse and keyboard right now.
		//
		// THIS IS THE ONE TO POLL AFTER ASKING, because WebUIFocusView answers
		// "accepted" and the backend may still decline, and because focus can be
		// taken away afterwards by things no consumer initiated - a save being
		// loaded, a menu opening, or the player pressing the panic chord.
		//
		// Answered from what the backend last reported observing, so it can lag
		// a change by a fraction of a second. It cannot be answered by asking the
		// backend directly: that is a call into it, and every one of those goes
		// through the main-thread queue, which a native cannot wait on.
		bool WebUIIsViewFocused(RE::StaticFunctionTag*, RE::BSFixedString a_viewId)
		{
			try {
				std::scoped_lock lock(g_mutex);
				const auto       it = g_views.find(ToStd(a_viewId));
				return it != g_views.end() && it->second.focused;
			} catch (...) {
				return false;
			}
		}

		// Lodestone.WebUIGetListenerSlotsFree() -> Int
		//
		// How many listener slots are still free, out of a finite pool shared by
		// every view and every mod.
		//
		// Diagnostic. A consumer that knows how many listeners it registers can
		// watch this and notice a registration loop before the pool runs out,
		// which is the failure this number replaces promising a fixed 32 in the
		// contract.
		//
		// Returns -1 when no backend is present.
		std::int32_t WebUIGetListenerSlotsFree(RE::StaticFunctionTag*)
		{
			try {
				if (!Active()) {
					return -1;
				}

				std::scoped_lock lock(g_mutex);
				std::int32_t     free = 0;
				for (const auto& slot : g_listeners) {
					if (!slot.used) {
						++free;
					}
				}
				return free;
			} catch (...) {
				return -1;
			}
		}

		// --- Natives added in 1.30.0 ---------------------------------------------

		// The ids of the views in g_views, sorted, optionally narrowed to the ones
		// actually on screen. CALLER HOLDS g_mutex.
		//
		// SORTED BECAUSE g_views HAS NO ORDER TO INHERIT. It is an unordered_map,
		// so the same table walked before and after an insert can hand back a
		// different sequence, and a consumer comparing two snapshots would read
		// that reshuffle as views having come and gone. Sorting by the id the
		// consumer chose makes two calls over an unchanged table compare equal,
		// which is the only promise this surface makes about order.
		//
		// The visible predicate is WebUIIsViewVisible's, to the letter, so that
		// native and this list can never disagree about one view.
		std::vector<std::string> CollectViewIds(bool a_visibleOnly)
		{
			std::vector<std::string> out;
			out.reserve(g_views.size());

			for (const auto& entry : g_views) {
				const bool visible = entry.second.handle != 0 && entry.second.domReady &&
									 !entry.second.hidden;
				if (a_visibleOnly && !visible) {
					continue;
				}
				out.push_back(entry.first);
			}

			std::sort(out.begin(), out.end());
			return out;
		}
		// Lodestone.WebUIGetViewIds() -> String[]
		//
		// Every view this bridge currently knows, in any state: still building,
		// ready and hidden, ready and visible. Sorted by id.
		//
		// ONE CALL, NOT A SWEEP, and that is why this is an array rather than the
		// count-plus-index pair ChannelInfo uses for the same shape of question.
		// Two reasons, and the second is the one that decided it. A native
		// registered without a_callableFromTasklets waits for the main thread, so
		// a count followed by N index calls costs N+1 frames. And those N+1 calls
		// each take the lock separately: a view created or destroyed between two
		// of them shifts every index after it, so the walk can miss a view or read
		// one twice. One call under one lock is a snapshot that cannot disagree
		// with itself, and there is no index-stability caveat to document because
		// there is no index.
		//
		// IT ANSWERS "WHO IS OPEN", NOT "WHERE THEY ARE". This bridge has never
		// known where a view sits on screen, and still does not. That rectangle is
		// CSS inside the consumer's own page; the numbers driving it live in the
		// consumer's Papyrus and are pushed through WebUICall. Neither half is
		// here.
		//
		// Cannot fail. AN EMPTY ARRAY IS NOT A SENTINEL - it means no views, which
		// is also the answer with no backend installed, and for any decision made
		// from this list those two are the same: with no backend the caller's own
		// view does not exist either.
		std::vector<std::string> WebUIGetViewIds(RE::StaticFunctionTag*)
		{
			try {
				std::scoped_lock lock(g_mutex);
				return CollectViewIds(false);
			} catch (...) {
				spdlog::error("WebUIBridge: WebUIGetViewIds threw.");
				return {};
			}
		}

		// Lodestone.WebUIGetVisibleViewIds() -> String[]
		//
		// The subset of WebUIGetViewIds() that is on screen right now: created,
		// page loaded, not hidden. Sorted by id.
		//
		// THIS IS THE ONE TO ASK BEFORE PLACING A PANEL. A view that exists but is
		// hidden takes up no screen, and screen is what a consumer reading this is
		// trying to share.
		//
		// THE OTHER ONE IS HOW YOU READ YOUR OWN ABSENCE. A view missing from this
		// list is hidden, or still building, or was never created, or was
		// destroyed - four situations this list alone cannot tell apart. Ask
		// WebUIGetViewIds, or WebUIGetViewState for one id.
		//
		// Cannot fail; an empty array reads the same way as in WebUIGetViewIds.
		std::vector<std::string> WebUIGetVisibleViewIds(RE::StaticFunctionTag*)
		{
			try {
				std::scoped_lock lock(g_mutex);
				return CollectViewIds(true);
			} catch (...) {
				spdlog::error("WebUIBridge: WebUIGetVisibleViewIds threw.");
				return {};
			}
		}

		// --- Deprecated 1.17.x names ---------------------------------------------
		//
		// Thin forwarding, kept for the whole 1.18.x cycle so a .pex built against
		// 1.17.x keeps working without being recompiled. They go away in the next
		// internal major (2.0.0), and not before the one known consumer has
		// migrated and run.
		//
		// PrismaIsHidden is the exception and is NOT a forward: it keeps its old
		// sense, because WebUIIsViewVisible deliberately answers the opposite
		// question. Forwarding one to the other would silently invert the answer
		// under a .pex that asked the old question - which no .pex does today, but
		// lying to one that might is worse than keeping ten lines.
		//
		// THE NAMES STAY Prisma*, AND THAT IS NOT AN OVERSIGHT NOW THAT A SECOND
		// BACKEND EXISTS. They are the literal strings a shipped .pex calls. A
		// consumer that reaches the bridge through them gets whichever backend is
		// active, which may well not be Prisma - the name is history, not routing.

		bool PrismaAvailable(RE::StaticFunctionTag* a_tag)
		{
			return WebUIAvailable(a_tag);
		}

		bool PrismaCreateView(RE::StaticFunctionTag* a_tag, RE::BSFixedString a_viewId, RE::BSFixedString a_htmlPath)
		{
			return WebUICreateView(a_tag, a_viewId, a_htmlPath);
		}

		bool PrismaIsViewReady(RE::StaticFunctionTag* a_tag, RE::BSFixedString a_viewId)
		{
			return WebUIIsViewReady(a_tag, a_viewId);
		}

		bool PrismaCall(RE::StaticFunctionTag* a_tag, RE::BSFixedString a_viewId, RE::BSFixedString a_function, RE::BSFixedString a_json)
		{
			return WebUICall(a_tag, a_viewId, a_function, a_json);
		}

		bool PrismaShow(RE::StaticFunctionTag* a_tag, RE::BSFixedString a_viewId)
		{
			return WebUIShow(a_tag, a_viewId);
		}

		bool PrismaHide(RE::StaticFunctionTag* a_tag, RE::BSFixedString a_viewId)
		{
			return WebUIHide(a_tag, a_viewId);
		}

		// The old question, with the old answer. See the note above.
		bool PrismaIsHidden(RE::StaticFunctionTag*, RE::BSFixedString a_viewId)
		{
			try {
				std::scoped_lock lock(g_mutex);
				const auto       it = g_views.find(ToStd(a_viewId));
				return it != g_views.end() && it->second.hidden;
			} catch (...) {
				return false;
			}
		}

		bool PrismaDestroy(RE::StaticFunctionTag* a_tag, RE::BSFixedString a_viewId)
		{
			return WebUIDestroyView(a_tag, a_viewId);
		}

		bool PrismaRegisterListener(RE::StaticFunctionTag* a_tag, RE::BSFixedString a_viewId, RE::BSFixedString a_jsFunction, RE::BSFixedString a_modEvent)
		{
			return WebUIRegisterListener(a_tag, a_viewId, a_jsFunction, a_modEvent);
		}
	}

	void Acquire()
	{
		// Every backend gets its chance, and none is chosen here.
		//
		// PROBING IS NOT CHOOSING, and separating them is what the second
		// backend forced. Prisma is settled when its Probe() returns; Meridian's
		// only arms a handshake that finishes two SKSE messages later. Deciding
		// at this seam would always pick Prisma, whatever the order said.
		for (const auto& accessor : kBackendOrder) {
			accessor()->Probe();
		}
	}

	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}

		for (const auto& accessor : kBackendOrder) {
			accessor()->HandleSKSEMessage(a_msg);
		}

		// kInputLoaded is where the choice is made, and it is the earliest seam
		// where it can be honest: it is the message that completes Meridian's
		// handshake, so it is the first moment at which every backend has
		// finished answering.
		//
		// Papyrus cannot have asked anything yet - kDataLoaded is still to come
		// - so no consumer can observe that the answer was null before this
		// point. Which is why WebUIAvailable() answering False early is correct
		// rather than a gap.
		if (a_msg->type == SKSE::MessagingInterface::kInputLoaded) {
			Resolve();
		}

		// The menu watch needs the UI singleton, which does not exist at
		// kInputLoaded. kDataLoaded is the first seam where it does, and it is
		// still before any consumer can have created a view.
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			InstallMenuWatch();
		}

		// THE TWO AUTOMATIC RELEASES THAT A MENU CANNOT COVER.
		//
		// kPreLoadGame is a save about to be loaded and kNewGame is the world
		// being built from nothing. In both the player is leaving whatever they
		// were looking at, and in both this module's view table survives while
		// the world under it does not - so a view left holding focus would hold
		// it into a session where nothing on screen explains why the game is not
		// listening.
		//
		// Cell changes are not listed here and are not forgotten: a load door
		// opens a menu that pauses the game, so the menu watch above already
		// covers them. Adding a cell sink would be a second path to the same
		// release, and a second thing to keep correct.
		if (a_msg->type == SKSE::MessagingInterface::kPreLoadGame) {
			ReleaseFocus("a save is being loaded");
		}

		if (a_msg->type == SKSE::MessagingInterface::kNewGame) {
			ReleaseFocus("a new game is starting");
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("WebUIBridge: null VM, cannot register natives.");
			return false;
		}

		// The 1.18.0 surface.
		a_vm->RegisterFunction("WebUIAvailable", "Lodestone", WebUIAvailable);
		a_vm->RegisterFunction("WebUICreateView", "Lodestone", WebUICreateView);
		a_vm->RegisterFunction("WebUIIsViewReady", "Lodestone", WebUIIsViewReady);
		a_vm->RegisterFunction("WebUICall", "Lodestone", WebUICall);
		a_vm->RegisterFunction("WebUIShow", "Lodestone", WebUIShow);
		a_vm->RegisterFunction("WebUIHide", "Lodestone", WebUIHide);
		a_vm->RegisterFunction("WebUIIsViewVisible", "Lodestone", WebUIIsViewVisible);
		a_vm->RegisterFunction("WebUIDestroyView", "Lodestone", WebUIDestroyView);
		a_vm->RegisterFunction("WebUIRegisterListener", "Lodestone", WebUIRegisterListener);
		a_vm->RegisterFunction("WebUIGetBackend", "Lodestone", WebUIGetBackend);
		a_vm->RegisterFunction("WebUIHasCapability", "Lodestone", WebUIHasCapability);
		a_vm->RegisterFunction("WebUIGetViewState", "Lodestone", WebUIGetViewState);
		a_vm->RegisterFunction("WebUIGetListenerSlotsFree", "Lodestone", WebUIGetListenerSlotsFree);

		// The 1.22.0 additions.
		a_vm->RegisterFunction("WebUIFocusView", "Lodestone", WebUIFocusView);
		a_vm->RegisterFunction("WebUIClearFocus", "Lodestone", WebUIClearFocus);
		a_vm->RegisterFunction("WebUIIsViewFocused", "Lodestone", WebUIIsViewFocused);

		a_vm->RegisterFunction("WebUIGetViewIds", "Lodestone", WebUIGetViewIds);
		a_vm->RegisterFunction("WebUIGetVisibleViewIds", "Lodestone", WebUIGetVisibleViewIds);

		// The 1.17.x surface, deprecated. Removed in 2.0.0, not before.
		a_vm->RegisterFunction("PrismaAvailable", "Lodestone", PrismaAvailable);
		a_vm->RegisterFunction("PrismaCreateView", "Lodestone", PrismaCreateView);
		a_vm->RegisterFunction("PrismaIsViewReady", "Lodestone", PrismaIsViewReady);
		a_vm->RegisterFunction("PrismaCall", "Lodestone", PrismaCall);
		a_vm->RegisterFunction("PrismaShow", "Lodestone", PrismaShow);
		a_vm->RegisterFunction("PrismaHide", "Lodestone", PrismaHide);
		a_vm->RegisterFunction("PrismaIsHidden", "Lodestone", PrismaIsHidden);
		a_vm->RegisterFunction("PrismaDestroy", "Lodestone", PrismaDestroy);
		a_vm->RegisterFunction("PrismaRegisterListener", "Lodestone", PrismaRegisterListener);

		spdlog::info("WebUIBridge: natives registered (27 - 18 current, 9 deprecated).");
		return true;
	}
}

// --- The way back in from a backend ------------------------------------------
//
// Defined outside the anonymous namespace above because a backend calls them,
// and declared in WebUIBackend.h. They are the only three entry points a
// backend has into the coordinator.
//
// WRAPPING IS THE BACKEND'S DUTY, NOT THEIRS. Both of these are called from a
// vendor's thread and return into vendor code, so the try/catch that keeps an
// exception from unwinding into third-party code belongs at the call site, where
// the vendor boundary actually is. See PrismaUIBackend.cpp.
namespace Lodestone::Core::WebUIBackendCallbacks
{
	void ViewReady(IWebUIBackend* a_backend, IWebUIBackend::ViewHandle a_view)
	{
		using namespace Lodestone::Core::WebUIBridge;

		std::string viewId;

		{
			std::scoped_lock lock(g_mutex);
			for (auto& entry : g_views) {
				if (entry.second.backend == a_backend && entry.second.handle == a_view) {
					entry.second.domReady = true;
					viewId                = entry.first;
					break;
				}
			}
		}

		if (viewId.empty()) {
			// A view this module did not create, or one destroyed between the
			// framework's call and this lock. Nothing to announce either way.
			spdlog::warn("WebUIBridge: page ready for an unknown view handle - ignored.");
			return;
		}

		spdlog::info("WebUIBridge: view '{}' is ready.", viewId);
		SendModEvent(kViewReadyEvent, viewId);
		SendModEvent(kViewReadyEventDeprecated, viewId);
	}

	void ListenerFired(std::size_t a_slot, const char* a_argument)
	{
		using namespace Lodestone::Core::WebUIBridge;

		std::string modEvent;
		std::string viewId;

		{
			std::scoped_lock lock(g_mutex);
			if (a_slot >= g_listeners.size() || !g_listeners[a_slot].used) {
				return;
			}
			modEvent = g_listeners[a_slot].modEvent;
			viewId   = g_listeners[a_slot].viewId;
		}

		spdlog::debug("WebUIBridge: view '{}' fired slot {} -> mod event '{}'.", viewId, a_slot, modEvent);
		SendModEvent(std::move(modEvent), a_argument ? std::string(a_argument) : std::string());
	}

	void FocusChanged(IWebUIBackend* a_backend, IWebUIBackend::ViewHandle a_view, bool a_focused)
	{
		using namespace Lodestone::Core::WebUIBridge;

		std::string viewId;

		{
			std::scoped_lock lock(g_mutex);
			for (auto& entry : g_views) {
				if (entry.second.backend == a_backend && entry.second.handle == a_view) {
					// Edge-triggered. A backend that observes by polling calls
					// this on every pass, so the level would be a log line ten
					// times a second for as long as a panel is open.
					if (entry.second.focused == a_focused) {
						return;
					}
					entry.second.focused = a_focused;
					viewId               = entry.first;
					break;
				}
			}
		}

		if (viewId.empty()) {
			// A view this module did not create, or one destroyed between the
			// backend's observation and this lock. Not warned about, unlike
			// ViewReady's equivalent: a view being destroyed while it holds
			// focus produces exactly this and is ordinary.
			return;
		}

		// info rather than debug, ON PURPOSE, because release builds drop debug
		// and this is the one line that explains a panel that stopped taking
		// input. It is edge-triggered and a player opens a panel a few times an
		// hour, so it cannot flood.
		spdlog::info("WebUIBridge: view '{}' {} focus.", viewId, a_focused ? "took" : "lost");
	}
}
