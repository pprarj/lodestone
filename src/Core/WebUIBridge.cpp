// WebUIBridge.cpp
// Lodestone - Shared SKSE framework
//
// Native implementations of the Prisma UI bridge. See WebUIBridge.h for why
// this module exists, why it is Core, and why it exposes no focus surface.

#include "WebUIBridge.h"

#include "PrismaUI_API.h"

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace Lodestone::Core::WebUIBridge
{
	namespace
	{
		// --- State -------------------------------------------------------------
		//
		// Written once on kPostLoad, read from the Papyrus VM thread and from the
		// Ultralight thread afterwards. Null means Prisma UI is not installed,
		// which is a supported state and not an error.
		PRISMA_UI_API::IVPrismaUI1* g_api = nullptr;

		// Guards g_views and g_listeners. Both are reached from at least three
		// threads: the Papyrus VM (natives), the Ultralight thread (JS listeners
		// and the DOM-ready callback) and the main game thread (the task queue).
		std::mutex g_mutex;

		// What Lodestone knows about one view.
		//
		// THE HANDLE IS NOT THE KEY, AND THIS IS THE WHOLE REASON FOR THE MAP.
		// PrismaView is a uint64_t; the Papyrus Int is 32 bits. Handing the handle
		// back to a consumer truncates it and corrupts it. The consumer names its
		// view with a string it chose, which also removes any question of what to
		// do with a handle across a save: there is nothing to keep.
		struct ViewRecord
		{
			// Zero until the create task has run on the main thread. A record with
			// a zero handle is reserved, not usable.
			PrismaView handle = 0;

			// Set by the DOM-ready callback. InteropCall before this point is
			// dropped by the framework, so this is what gates PrismaCall.
			bool domReady = false;

			// Mirror of the last Show/Hide this module applied.
			//
			// WHY MIRRORED RATHER THAN ASKED. IsHidden(view) would have to be
			// called on the Prisma API, and every call into that API from this
			// module goes through the main-thread task queue - see DispatchToGame.
			// A native cannot wait for a queued answer without blocking the VM
			// thread on the game thread, which is a deadlock waiting for a bad
			// day. The mirror is exact for every transition this module made, and
			// this module is the only thing that can move a view it created.
			bool hidden = false;
		};

		std::unordered_map<std::string, ViewRecord> g_views;

		// --- JS listener slots --------------------------------------------------
		//
		// PrismaUI's JSListenerCallback is void(*)(const char*). It carries NO
		// context argument, so there is no way to hand the framework a lambda that
		// knows which view and which mod event a callback belongs to - a capturing
		// lambda does not convert to a plain function pointer at all.
		//
		// The way out is a fixed table of distinct functions, each of which knows
		// its own index at compile time and looks the rest up. That is why there is
		// a maximum at all: the number of listeners is the number of functions the
		// compiler was asked to emit.
		//
		// 32 was chosen as the smallest number that is not going to be met.
		// Consumers register a handful of callbacks per panel, and the first
		// consumer of this module is read-only and registers none. Raising it costs
		// one constant and a recompile.
		constexpr std::size_t kMaxListeners = 32;

		struct ListenerSlot
		{
			bool        used = false;
			std::string viewId;
			std::string modEvent;
		};

		std::array<ListenerSlot, kMaxListeners> g_listeners{};

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
		// EVERY CALL INTO THE PRISMA API GOES THROUGH HERE, and the reason is not
		// symmetry. Natives run on the Papyrus VM thread and JS callbacks arrive on
		// the Ultralight thread; neither is the thread the renderer belongs to. The
		// sibling project that established the CreateView timing trap drives Prisma
		// from an input sink, which is already the main thread, so it never had to
		// answer this question and its precedent does not cover us.
		//
		// The consequence is deliberate and is stated in Lodestone.psc: a native
		// that mutates a view reports that the request was accepted, not that the
		// framework has finished acting on it.
		//
		// NOTHING ESCAPES THIS FUNCTION, and that is not defensive habit. Part of
		// its callers are the framework's own callbacks, running on the Ultralight
		// thread - an exception leaving here would unwind back into Prisma, which
		// is the same undefined behavior as letting one cross into the Papyrus VM.
		// AddTask is treated as able to throw because DetectionRead already treats
		// it that way in this plugin.
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
		// Arguments are taken by value because the caller is usually the Ultralight
		// thread, holding a const char* that does not outlive the callback.
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

		// The mod events announcing that a view's DOM is ready.
		//
		// Fixed rather than chosen by the consumer, because a consumer that has not
		// created a view yet has nowhere to have told us a name. strArg carries the
		// view id, so one handler serves every view a mod owns.
		//
		// BOTH ARE SENT, WITH THE SAME strArg, FROM THE SAME POINT, and that is the
		// only way a mod event can be renamed at all. The name is wire protocol: the
		// consumer writes the string by hand into RegisterForModEvent, so renaming
		// the native that creates the view does not reach it. A .pex built against
		// 1.17.x keeps working, without recompiling, for as long as the old name is
		// still sent. The old one goes away in the next internal major (2.0.0), not
		// before, and not before the one known consumer has migrated.
		constexpr const char* kViewReadyEvent           = "LodestoneWebUIViewReady";
		constexpr const char* kViewReadyEventDeprecated = "LodestonePrismaViewReady";

		// --- Callbacks from the framework ---------------------------------------

		// Called by Prisma on the Ultralight thread when a view's DOM is ready.
		//
		// This callback DOES carry the view, which is what makes a single shared
		// function enough here, and is why the JS listeners below need a table and
		// this does not.
		// Wrapped for the same reason DispatchToGame is: this returns into Prisma,
		// so an exception leaving it is undefined behavior.
		void OnDomReady(PrismaView a_view)
		{
			try {
				std::string viewId;

				{
					std::scoped_lock lock(g_mutex);
					for (auto& entry : g_views) {
						if (entry.second.handle == a_view) {
							entry.second.domReady = true;
							viewId                = entry.first;
							break;
						}
					}
				}

				if (viewId.empty()) {
					// A view this module did not create, or one destroyed between
					// the framework's call and this lock. Nothing to announce
					// either way.
					spdlog::warn("WebUIBridge: DOM ready for an unknown view handle - ignored.");
					return;
				}

				spdlog::info("WebUIBridge: view '{}' is ready.", viewId);
				SendModEvent(kViewReadyEvent, viewId);
				SendModEvent(kViewReadyEventDeprecated, viewId);
			} catch (...) {
				spdlog::error("WebUIBridge: OnDomReady threw - the view is usable but no ready "
							  "event was sent.");
			}
		}

		// Turns one slot's callback into a mod event. Called on the Ultralight
		// thread, through one of the thunks below, and wrapped for the same reason
		// they all are - it returns into Prisma.
		void DispatchListener(std::size_t a_slot, const char* a_argument)
		{
			try {
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
			} catch (...) {
				spdlog::error("WebUIBridge: a JS listener threw on slot {} - the mod event was not sent.", a_slot);
			}
		}

		// One distinct function per slot, each carrying its index in its type.
		template <std::size_t N>
		void ListenerThunk(const char* a_argument)
		{
			DispatchListener(N, a_argument);
		}

		template <std::size_t... I>
		constexpr std::array<PRISMA_UI_API::JSListenerCallback, sizeof...(I)> MakeThunkTable(std::index_sequence<I...>)
		{
			return { &ListenerThunk<I>... };
		}

		const auto g_thunks = MakeThunkTable(std::make_index_sequence<kMaxListeners>{});

		// --- Natives -------------------------------------------------------------
		//
		// Every fallible body below is wrapped: a C++ exception crossing into the
		// Papyrus VM is undefined behavior and can take the game down. Sentinels
		// follow the framework convention - Bool -> false.

		// Lodestone.WebUIAvailable() -> Bool
		//
		// Whether a web UI backend is present and its API answered.
		//
		// A PROBE, NOT A FAILURE. Nothing is logged when the answer is False: it is
		// the expected answer on most load orders, and a consumer is meant to call
		// this to decide whether to offer a panel at all.
		//
		// Cannot fail - it reads one pointer and has no error path.
		bool WebUIAvailable(RE::StaticFunctionTag*)
		{
			return g_api != nullptr;
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
				if (!g_api) {
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
					// Reserved before the task runs, so two calls in the same frame
					// cannot both queue a create.
					g_views.emplace(viewId, ViewRecord{});
				}

				DispatchToGame([viewId, viewPath]() {
					const PrismaView handle = g_api->CreateView(viewPath.c_str(), &OnDomReady);

					// True when the id was released while this create was queued -
					// a destroy ran in between. The view has to be thrown away,
					// but not from under the lock: OnDomReady arrives on the
					// Ultralight thread and takes the same mutex, so calling back
					// into the framework while holding it is a stall waiting to
					// happen. The decision is made here; the call is made below.
					bool orphaned = false;

					{
						std::scoped_lock lock(g_mutex);
						const auto       it = g_views.find(viewId);
						if (it == g_views.end()) {
							orphaned = true;
						} else if (handle == 0) {
							g_views.erase(it);
							spdlog::error("WebUIBridge: CreateView failed for '{}' - check that the view folder "
										  "exists under the backend's view root (Data\\PrismaUI\\views for "
										  "Prisma UI) and holds the file named by '{}'.",
								viewId, viewPath);
						} else {
							it->second.handle = handle;
							spdlog::info("WebUIBridge: view '{}' created from '{}'.", viewId, viewPath);
						}
					}

					if (orphaned && handle != 0) {
						g_api->Destroy(handle);
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
		// Calls a JavaScript function on the view's JS interop surface, handing it
		// asJson as its single argument.
		//
		// Returns True when the request was accepted. Returns False for an unknown
		// id, for an empty function name, and for a view whose page is not ready -
		// the backend drops those calls, so reporting success would be a lie.
		bool WebUICall(RE::StaticFunctionTag*, RE::BSFixedString a_viewId, RE::BSFixedString a_jsFunction, RE::BSFixedString a_json)
		{
			try {
				if (!g_api) {
					return false;
				}

				const std::string function = ToStd(a_jsFunction);
				if (function.empty()) {
					return false;
				}

				PrismaView handle = 0;

				{
					std::scoped_lock lock(g_mutex);
					const auto       it = g_views.find(ToStd(a_viewId));
					if (it == g_views.end() || it->second.handle == 0 || !it->second.domReady) {
						return false;
					}
					handle = it->second.handle;
				}

				DispatchToGame([handle, function, json = ToStd(a_json)]() {
					g_api->InteropCall(handle, function.c_str(), json.c_str());
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
			if (!g_api) {
				return false;
			}

			PrismaView handle = 0;

			{
				std::scoped_lock lock(g_mutex);
				const auto       it = g_views.find(ToStd(a_viewId));
				if (it == g_views.end() || it->second.handle == 0) {
					return false;
				}
				handle            = it->second.handle;
				it->second.hidden = a_hide;
			}

			DispatchToGame([handle, a_hide]() {
				if (a_hide) {
					g_api->Hide(handle);
				} else {
					g_api->Show(handle);
				}
			});

			return true;
		}

		// Lodestone.WebUIShow(String) -> Bool
		//
		// Returns True when the request was accepted, False for an unknown id or a
		// view that has not been built yet.
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
		// Returns True when the request was accepted, False for an unknown id or a
		// view that has not been built yet.
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
		// collapses to False and the True means exactly one thing. That was free to
		// change because the old name had no callers at all - measured across the
		// tree in 2026-09-03.
		//
		// This reports the last visibility this module applied - see ViewRecord for
		// why it is mirrored rather than asked. It is exact for every Show and Hide
		// that was issued.
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
				if (!g_api) {
					return false;
				}

				const std::string viewId = ToStd(a_viewId);
				PrismaView        handle = 0;

				{
					std::scoped_lock lock(g_mutex);
					const auto       it = g_views.find(viewId);
					if (it == g_views.end()) {
						return false;
					}
					handle = it->second.handle;
					g_views.erase(it);

					// Slots outlive nothing. A freed id can be created again, and a
					// stale slot would fire a mod event for a view that is gone.
					for (auto& slot : g_listeners) {
						if (slot.used && slot.viewId == viewId) {
							slot = ListenerSlot{};
						}
					}
				}

				if (handle != 0) {
					DispatchToGame([handle]() { g_api->Destroy(handle); });
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
		// Registering the same (view, mod event) pair again reuses its slot rather
		// than taking a second one.
		//
		// Returns False if no backend is present, for an unknown or unbuilt view,
		// for an empty name, or when all listener slots are taken - see
		// kMaxListeners and WebUIGetListenerSlotsFree.
		bool WebUIRegisterListener(RE::StaticFunctionTag*, RE::BSFixedString a_viewId, RE::BSFixedString a_jsFunction, RE::BSFixedString a_modEvent)
		{
			try {
				if (!g_api) {
					return false;
				}

				const std::string viewId     = ToStd(a_viewId);
				const std::string jsFunction = ToStd(a_jsFunction);
				const std::string modEvent   = ToStd(a_modEvent);
				if (jsFunction.empty() || modEvent.empty()) {
					return false;
				}

				PrismaView  handle = 0;
				std::size_t slot   = kMaxListeners;

				{
					std::scoped_lock lock(g_mutex);
					const auto       it = g_views.find(viewId);
					if (it == g_views.end() || it->second.handle == 0) {
						return false;
					}
					handle = it->second.handle;

					// Reuse the slot if this pair is already registered. The
					// framework keeps one callback per (view, name), so a second
					// RegisterJSListener overwrites it there too - taking a second
					// slot here would leak one per re-registration, and
					// re-registering after a load is the documented pattern.
					for (std::size_t i = 0; i < g_listeners.size(); ++i) {
						if (g_listeners[i].used && g_listeners[i].viewId == viewId &&
							g_listeners[i].modEvent == modEvent) {
							slot = i;
							break;
						}
					}

					if (slot == kMaxListeners) {
						for (std::size_t i = 0; i < g_listeners.size(); ++i) {
							if (!g_listeners[i].used) {
								slot = i;
								break;
							}
						}
					}

					if (slot == kMaxListeners) {
						spdlog::error("WebUIBridge: all {} listener slots are in use - '{}' on view '{}' "
									  "was not registered.",
							kMaxListeners, jsFunction, viewId);
						return false;
					}

					g_listeners[slot] = ListenerSlot{ true, viewId, modEvent };
				}

				DispatchToGame([handle, jsFunction, slot]() {
					g_api->RegisterJSListener(handle, jsFunction.c_str(), g_thunks[slot]);
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
				return g_api ? RE::BSFixedString("PrismaUI") : RE::BSFixedString("");
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
		// Answered from what the backend can do, never from which backend it is.
		bool WebUIHasCapability(RE::StaticFunctionTag*, RE::BSFixedString a_capability)
		{
			try {
				if (!g_api) {
					return false;
				}

				const std::string capability = ToStd(a_capability);

				// "focus-stack": can two views hold focus independently.
				//
				// False, and this is measured, not assumed. Prisma exposes Focus
				// and Unfocus per view, but its focus menu is a single kModal with
				// no stack: unfocusing one closes it for all of them. The capability
				// exists; the stacking does not. See the header.
				if (capability == "focus-stack") {
					return false;
				}

				// "view-order": can a view's stacking order be set.
				// PrismaUI_API.h SetOrder / GetOrder.
				if (capability == "view-order") {
					return true;
				}

				// "inspector": can a developer inspector be opened on a view.
				// PrismaUI_API.h CreateInspectorView.
				if (capability == "inspector") {
					return true;
				}

				return false;
			} catch (...) {
				return false;
			}
		}

		// Lodestone.WebUIGetViewState(String) -> Int
		//
		// The whole state of one view in one call, which is what the three separate
		// Bool answers cannot give: each of them collapses several situations into
		// one False.
		//
		//   -1  unknown id, or no backend present
		//    0  created, page has not loaded yet
		//    1  ready, hidden
		//    2  ready, visible
		std::int32_t WebUIGetViewState(RE::StaticFunctionTag*, RE::BSFixedString a_viewId)
		{
			try {
				if (!g_api) {
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

		// Lodestone.WebUIGetListenerSlotsFree() -> Int
		//
		// How many listener slots are still free, out of a finite pool shared by
		// every view and every mod.
		//
		// Diagnostic. A consumer that knows how many listeners it registers can
		// watch this and notice a registration loop before the pool runs out, which
		// is the failure this number replaces promising a fixed 32 in the contract.
		//
		// Returns -1 when no backend is present.
		std::int32_t WebUIGetListenerSlotsFree(RE::StaticFunctionTag*)
		{
			try {
				if (!g_api) {
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
		g_api = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI1>();

		if (g_api) {
			spdlog::info("WebUIBridge: Prisma UI found - bridge active.");
		} else {
			// Not an error. See the header: this is the common case, and a consumer
			// asking PrismaAvailable() is expecting it.
			spdlog::info("WebUIBridge: Prisma UI not present - bridge inactive, "
						 "natives return their sentinels.");
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

		spdlog::info("WebUIBridge: natives registered (22 - 13 current, 9 deprecated).");
		return true;
	}
}
