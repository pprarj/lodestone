// PrismaUIBackend.cpp
// Lodestone - Shared SKSE framework
//
// The Prisma UI half of the WebUI bridge. Everything vendor-specific about
// Prisma lives here and nowhere else; see WebUIBackend.h for what the seam is
// and WebUIBridge.h for what the bridge is.
//
// THIS FILE IS THE ONLY ONE THAT INCLUDES PrismaUI_API.h, and that is worth more
// than tidiness. That header is third-party, under a proprietary
// source-available license, and is deliberately NOT tracked in this repository -
// whoever builds copies it into extern/prismaui-api/ by hand. Confining it to
// one translation unit means the rest of the bridge compiles and reads without
// it, and a future decision to drop this backend is a file deletion.
//
// NOTHING HERE KNOWS ABOUT VIEW IDS, LISTENER OWNERSHIP OR MOD EVENTS. Those are
// the coordinator's, keyed by the string the consumer chose. This file deals in
// PrismaView handles and slot indices and nothing else.

#include "WebUIBackend.h"
#include "WebUIPanicKeys.h"

#include "PrismaUI_API.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Lodestone::Core
{
	namespace
	{
		// Written once by Probe() on kPostLoad, read from the Papyrus VM thread,
		// the Ultralight thread and the main game thread afterwards. Null means
		// Prisma UI is not installed, which is a supported state and not an
		// error.
		PRISMA_UI_API::IVPrismaUI1* g_api = nullptr;

		// Prisma's DOM-ready callback, which it invokes on the Ultralight thread.
		//
		// THIS CALLBACK CARRIES THE VIEW, which is what makes one shared function
		// enough here and is exactly why the JS listeners below need a table and
		// this does not.
		//
		// Wrapped because it returns into Prisma: an exception leaving it unwinds
		// into third-party code, which is undefined behavior for the same reason
		// letting one cross into the Papyrus VM is.
		void OnDomReady(PrismaView a_view)
		{
			try {
				WebUIBackendCallbacks::ViewReady(WebUIBackends::PrismaUI(), a_view);
			} catch (...) {
				spdlog::error("PrismaUIBackend: OnDomReady threw - the view is usable but no ready "
							  "event was sent.");
			}
		}

		// --- JS listener thunks -------------------------------------------------
		//
		// PrismaUI's JSListenerCallback is void(*)(const char*). It carries NO
		// context argument, so there is no way to hand the framework a lambda
		// that knows which slot it belongs to - a capturing lambda does not
		// convert to a plain function pointer at all.
		//
		// The way out is a fixed table of distinct functions, each of which knows
		// its own index at compile time. That is the reason kWebUIMaxListeners
		// has to be a compile-time constant: the number of listeners is the
		// number of functions the compiler was asked to emit.
		//
		// Wrapped for the same reason OnDomReady is - these return into Prisma.
		template <std::size_t N>
		void ListenerThunk(const char* a_argument)
		{
			try {
				WebUIBackendCallbacks::ListenerFired(N, a_argument);
			} catch (...) {
				spdlog::error("PrismaUIBackend: a JS listener threw on slot {} - the mod event was "
							  "not sent.",
					N);
			}
		}

		template <std::size_t... I>
		constexpr std::array<PRISMA_UI_API::JSListenerCallback, sizeof...(I)> MakeThunkTable(std::index_sequence<I...>)
		{
			return { &ListenerThunk<I>... };
		}

		const auto g_thunks = MakeThunkTable(std::make_index_sequence<kWebUIMaxListeners>{});

		// --- The views this backend made ----------------------------------------
		//
		// Kept only since 1.27.0, and only for focus: this backend used to need
		// nothing but the handle the coordinator hands back, and still needs
		// nothing else for the other operations.
		//
		// NO LOCK, the same contract as the Meridian backend's table. Everything
		// that touches it runs on the main game thread: the view operations
		// arrive there from the coordinator, and the poll and the panic release
		// are posted there. The input sink below does NOT read it - it only posts.
		struct ViewRecord
		{
			IWebUIBackend::ViewHandle handle = 0;

			// Whether the view held focus at the last poll.
			//
			// OBSERVED, NOT REQUESTED. Focus moves here for reasons this file
			// never initiates: another Prisma consumer focusing its own view, the
			// panic chord, a teardown. Only HasFocus() sees those, which is why
			// the poll asks it rather than remembering what SetFocus sent.
			bool focused = false;
		};

		std::vector<ViewRecord> g_views;

		ViewRecord* Find(IWebUIBackend::ViewHandle a_view)
		{
			for (auto& record : g_views) {
				if (record.handle == a_view) {
					return &record;
				}
			}
			return nullptr;
		}

		// --- Watching focus -----------------------------------------------------
		//
		// WHY A POLL, AND THE SAME ONE THE MERIDIAN BACKEND RUNS. Prisma has a
		// DOM-ready callback but no focus-changed one; HasFocus() is a question,
		// so the answer has to be asked for. The alternative - reporting what
		// SetFocus and ClearFocus requested - is the mirror of intent the
		// coordinator refused (WebUIBackend.h, FocusChanged), and here it would
		// be wrong in a measured way: Focus and Unfocus are asynchronous, and a
		// read in the same frame returns the value from before the command.
		//
		// DESIGNED TO CONVERGE, NOT TO BE INSTANT. The mirror lags a real change
		// by up to one interval. What it must never do is claim focus that does
		// not exist, and it cannot: it only ever reports what HasFocus answered.

		// How often the views are asked. Same interval as the Meridian backend,
		// for the same reason: a player cannot tell a tenth of a second from
		// instant, and the cost is one call per view.
		constexpr auto kFocusPollInterval = std::chrono::milliseconds(100);

		// How many live views there are. Read by the watcher thread, written on
		// the game thread.
		std::atomic<int> g_watchedViews{ 0 };

		// Started at most once per process, at the first view.
		std::atomic<bool> g_watcherStarted{ false };

		// One pass over every view's focus. GAME THREAD ONLY.
		void PollFocus()
		{
			if (!g_api) {
				return;
			}

			for (auto& record : g_views) {
				const bool nowFocused = g_api->HasFocus(static_cast<PrismaView>(record.handle));
				if (nowFocused == record.focused) {
					continue;
				}

				record.focused = nowFocused;

				try {
					WebUIBackendCallbacks::FocusChanged(WebUIBackends::PrismaUI(), record.handle, nowFocused);
				} catch (...) {
					spdlog::error("PrismaUIBackend: reporting a focus change threw - the bridge's view "
								  "of who holds focus is now stale.");
				}
			}
		}

		// Sleeps, and posts one PollFocus to the game thread per interval while
		// any view exists. Never exits - the reasoning is the Meridian backend's
		// WatcherLoop, and it holds unchanged: a thread that stops and restarts
		// has a window where a new view goes unwatched.
		void WatcherLoop()
		{
			for (;;) {
				std::this_thread::sleep_for(kFocusPollInterval);

				if (g_watchedViews.load(std::memory_order_acquire) <= 0) {
					continue;
				}

				if (auto* task = SKSE::GetTaskInterface()) {
					task->AddTask([]() { PollFocus(); });
				}
			}
		}

		void StartWatcher()
		{
			bool expected = false;
			if (g_watcherStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
				std::thread(&WatcherLoop).detach();
			}
		}

		// --- The panic chord ----------------------------------------------------
		//
		// THE PLAYER'S WAY OUT, AND NO CONSUMER CAN TURN IT OFF - the same promise
		// the Meridian backend keeps, kept a different way because Prisma
		// publishes no chord of its own.
		//
		// AN INPUT SINK OF LODESTONE'S, and it was measured before it was built.
		// Phase L-U7, E1, in game on 2026-09-17, Prisma UI 1.5.0: with a Prisma
		// panel focused and a word typed into its text field, a sink on
		// BSInputDeviceManager received every key, and saw Ctrl+Backspace
		// complete with HasAnyActiveFocus() true. The probe is parked in the
		// private work area, not in this repository.
		//
		// IT ONLY RELEASES. IT NEVER TOGGLES. Meridian's chord is a toggle and is
		// safe there only because it is armed on the browser holding focus. This
		// sink sees every key in the game; a toggle would hand focus to a view
		// that did not have it, which is the stranded state the chord exists to
		// undo. For the player the effect is the same on both backends: the chord
		// gives the game its controls back.
		//
		// WHAT IT RELEASES: the views of THIS bridge that hold focus, and nothing
		// else. A Prisma panel of another mod - the measurement used one - is not
		// this plugin's to take input from, and the same E1 showed why the test
		// cannot be HasAnyActiveFocus(): it was already true with only a
		// third-party overlay on screen. It answers "somebody", not "whom".
		// Per-view HasFocus() is the question that names the view.
		//
		// IT DOES NOT CONSUME THE KEY, AND COULD NOT PROMISE TO. Returning kStop
		// only skips the sinks registered after this one on the same source
		// (BSTEvent.h, SendEvent), and the focused page in E1 received the chord
		// while the sink saw it - a text field lost its word and gained a stray
		// control character. Whatever Prisma reads keys through is not behind
		// this sink. Stopping the event anyway would starve the game's own sinks
		// of half a key press and prevent nothing on the page.

		// The chord, read on the game thread when the sink is installed, read by
		// the sink on whatever thread the input manager dispatches from. Atomic
		// so that question does not need an answer.
		std::atomic<std::uint32_t> g_chordFirst{ 0 };
		std::atomic<std::uint32_t> g_chordSecond{ 0 };

		// Drops focus from every view of this bridge that holds it. GAME THREAD
		// ONLY - posted there by the sink.
		//
		// Silent when none does: pressing the chord with no panel of this bridge
		// focused is ordinary, and so is pressing it over another mod's panel.
		void ReleaseByChord()
		{
			if (!g_api) {
				return;
			}

			std::size_t released = 0;
			for (auto& record : g_views) {
				const auto view = static_cast<PrismaView>(record.handle);

				// Either answer is enough. The poll's value can lag a focus that
				// just arrived; HasFocus can lag one that just left. An unfocus
				// sent to a view that was about to lose focus anyway costs
				// nothing a player could see.
				if (!record.focused && !g_api->HasFocus(view)) {
					continue;
				}

				g_api->Unfocus(view);
				++released;
			}

			if (released > 0) {
				spdlog::info("PrismaUIBackend: panic chord pressed - released focus from {} view(s) of this "
							 "bridge.",
					released);
			}
		}

		class PanicSink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static PanicSink* GetSingleton()
			{
				static PanicSink singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
				RE::BSTEventSource<RE::InputEvent*>*) override
			{
				// Nothing here may throw into the input dispatch loop.
				try {
					const auto first  = g_chordFirst.load(std::memory_order_acquire);
					const auto second = g_chordSecond.load(std::memory_order_acquire);
					if (!a_event || first == 0 || second == 0) {
						return RE::BSEventNotifyControl::kContinue;
					}

					for (auto* event = *a_event; event; event = event->next) {
						if (event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton ||
							event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
							continue;
						}

						const auto*         button = static_cast<const RE::ButtonEvent*>(event);
						const std::uint32_t code   = button->GetIDCode();

						bool* held = nullptr;
						if (code == first) {
							held = &_firstHeld;
						} else if (code == second) {
							held = &_secondHeld;
						} else {
							continue;
						}

						if (button->IsUp()) {
							*held = false;
							continue;
						}

						if (!button->IsDown()) {
							continue;
						}

						*held = true;

						// Complete on whichever key goes down last, and again on
						// every re-press while the other is still held - the shape
						// Meridian's header states for its own chord. The same
						// key set twice is a one-key chord.
						const bool complete = first == second ? _firstHeld : (_firstHeld && _secondHeld);
						if (!complete) {
							continue;
						}

						if (auto* task = SKSE::GetTaskInterface()) {
							task->AddTask([]() { ReleaseByChord(); });
						}
					}
				} catch (...) {
					spdlog::error("PrismaUIBackend: the panic chord sink threw - that key press was not "
								  "checked.");
				}

				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			PanicSink()                            = default;
			PanicSink(const PanicSink&)            = delete;
			PanicSink& operator=(const PanicSink&) = delete;

			// Held state, from the events this sink sees. Only the dispatching
			// thread touches these.
			bool _firstHeld  = false;
			bool _secondHeld = false;
		};

		// Whether the sink is registered. GAME THREAD ONLY. Installed at most
		// once and never removed, like the coordinator's menu watch.
		bool g_panicSinkInstalled = false;

		// Installs the sink if it is not in yet. GAME THREAD ONLY.
		//
		// At the first view rather than at a load seam, for two reasons: the
		// input device manager is null early in the load (a fact of the engine),
		// and a session whose backend is Meridian, or that never opens a panel,
		// has no business watching the keyboard.
		void InstallPanicSink()
		{
			if (g_panicSinkInstalled) {
				return;
			}

			auto* manager = RE::BSInputDeviceManager::GetSingleton();
			if (!manager) {
				spdlog::error("PrismaUIBackend: the input device manager does not exist yet - the panic "
							  "chord is not installed, and focus will be refused until it is.");
				return;
			}

			const auto chord = WebUIPanicKeys::Get("PrismaUIBackend");
			g_chordFirst.store(chord.first, std::memory_order_release);
			g_chordSecond.store(chord.second, std::memory_order_release);

			manager->AddEventSink(PanicSink::GetSingleton());
			g_panicSinkInstalled = true;

			spdlog::info("PrismaUIBackend: panic chord installed on scan codes 0x{:02X} + 0x{:02X}.",
				chord.first, chord.second);
		}

		// --- The backend --------------------------------------------------------

		class PrismaUIBackend final : public IWebUIBackend
		{
		public:
			const char* Name() const override { return "PrismaUI"; }
			const char* DisplayName() const override { return "Prisma UI"; }

			// Phrased to be dropped straight into the coordinator's CreateView
			// error, which is where a consumer with a typo in a path first
			// looks.
			const char* ViewRootHint() const override { return "Data\\PrismaUI\\views for Prisma UI"; }

			// Prisma answers a direct request, so presence is settled here and
			// IsAvailable() never changes afterwards.
			//
			// NO VIEW IS CREATED HERE, and that is the trap this module was
			// written around. At kPostLoad the D3D device and the Ultralight
			// renderer do not exist yet, and CreateView at that moment queues
			// forever or blocks the load chain - established from the Add Item
			// Menu's own source and paid for again by a sibling project of this
			// tree.
			void Probe() override
			{
				g_api = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI1>();
			}

			// Nothing. Prisma is settled by Probe() and needs no seam of its
			// own - see the interface, where this is the expected shape for a
			// backend acquired by direct request rather than by handshake.
			void HandleSKSEMessage(SKSE::MessagingInterface::Message*) override {}

			bool IsAvailable() const override { return g_api != nullptr; }

			bool HasCapability(const char* a_capability) const override
			{
				if (!a_capability) {
					return false;
				}

				const std::string_view capability(a_capability);

				// "focus-stack": can two views hold focus independently.
				//
				// False, and this is measured, not assumed. Prisma exposes Focus
				// and Unfocus per view, but its focus menu is a single kModal
				// with no stack: unfocusing one closes it for all of them. The
				// capability exists; the stacking does not. See WebUIBridge.h.
				//
				// IT STAYS false, AND 1.22.0 IS EXACTLY WHEN SOMEBODY WILL TRY
				// TO "FIX" IT. That version added "view-focus" below, so this
				// line now sits next to a focus capability that answers
				// differently, and the two look like they disagree. They do not.
				// They are different questions - see the trap written out under
				// "view-focus" - and this one has the same answer on both
				// backends for two different reasons.
				if (capability == "focus-stack") {
					return false;
				}

				// "view-focus": can ONE view be given the mouse and keyboard.
				//
				// THIS IS NOT "focus-stack" WITH A SHORTER NAME, and reading it
				// that way is the mistake this comment exists to stop:
				//
				//   focus-stack   can TWO views hold focus independently?
				//   view-focus    can ONE view receive a click at all?
				//
				// A backend can answer no to the first and yes to the second,
				// and the other one does. A consumer that asks "focus-stack"
				// meaning "can my panel take a click" gets a wrong answer on
				// every backend, in both directions over time: false before
				// 1.22.0 because the surface did not exist, and false after it
				// because that is genuinely the answer to a question it did not
				// mean to ask.
				//
				// TRUE SINCE 1.27.0. It answered false from 1.22.0 to 1.26.x,
				// for three reasons kept with the code they governed - see the
				// focus section below, where they stay as history. The one that
				// decided it was the missing escape hatch, and that is what 1.27.0
				// added: an input sink of Lodestone's with the WebUIPanicKeys
				// chord. The other two are still true and are now the consumer's
				// to weigh, stated in Lodestone.psc.
				//
				// A consumer that already asked this question needs no change:
				// it gets true and takes the interactive path.
				if (capability == "view-focus") {
					return true;
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
			}

			// The view id is unused here: Prisma names nothing and hands back an
			// opaque handle. It is in the signature for Meridian's sake.
			ViewHandle CreateView(const char*, const char* a_viewPath) override
			{
				if (!g_api) {
					return 0;
				}

				const auto handle = static_cast<ViewHandle>(g_api->CreateView(a_viewPath, &OnDomReady));
				if (handle == 0) {
					return 0;
				}

				// Watched from birth, so a view that some other path focuses is
				// reported like one the bridge focused. The escape hatch goes in
				// with the first view, before any consumer can ask for focus.
				if (!Find(handle)) {
					g_views.push_back(ViewRecord{ handle });
					g_watchedViews.fetch_add(1, std::memory_order_acq_rel);
				}
				InstallPanicSink();
				StartWatcher();

				return handle;
			}

			void DestroyView(ViewHandle a_view) override
			{
				if (!g_api) {
					return;
				}

				const auto view = static_cast<PrismaView>(a_view);

				for (std::size_t i = 0; i < g_views.size(); ++i) {
					if (g_views[i].handle == a_view) {
						// A view torn down while holding focus would take the
						// capture with it into a handle nobody can name any more,
						// and the chord only reaches views still in this table.
						// Whether Prisma releases on its own is not known here, so
						// it is not left to chance.
						if (g_views[i].focused || g_api->HasFocus(view)) {
							g_api->Unfocus(view);
						}

						g_watchedViews.fetch_sub(1, std::memory_order_acq_rel);
						g_views.erase(g_views.begin() + static_cast<std::ptrdiff_t>(i));
						break;
					}
				}

				g_api->Destroy(view);
			}

			void Show(ViewHandle a_view) override
			{
				if (g_api) {
					g_api->Show(static_cast<PrismaView>(a_view));
				}
			}

			void Hide(ViewHandle a_view) override
			{
				if (g_api) {
					g_api->Hide(static_cast<PrismaView>(a_view));
				}
			}

			void Call(ViewHandle a_view, const char* a_function, const char* a_json) override
			{
				if (g_api) {
					g_api->InteropCall(static_cast<PrismaView>(a_view), a_function, a_json);
				}
			}

			void RegisterListener(ViewHandle a_view, const char* a_jsFunction, std::size_t a_slot) override
			{
				if (g_api && a_slot < g_thunks.size()) {
					g_api->RegisterJSListener(static_cast<PrismaView>(a_view), a_jsFunction, g_thunks[a_slot]);
				}
			}

			// --- Focus -------------------------------------------------------
			//
			// HISTORY FIRST, BECAUSE IT EXPLAINS THE CODE BELOW. From 1.22.0 to
			// 1.26.x this backend answered false to "view-focus" and these two
			// were stubs that logged an error if reached. The comment that
			// justified it is kept as it was written, between the two rules
			// below, and it is still right about facts 1 and 2. Fact 3 stopped
			// being true in 1.27.0 (2026-09-17, phase L-U7): Lodestone now has
			// an input sink, measured in game to see the chord with a Prisma
			// panel focused - see the panic chord section above.
			//
			// ------------------------------------------------------------------
			// WHY THE CAPABILITY IS false, AND IT IS NOT THAT THE API IS
			// MISSING. PrismaUI_API.h has Focus, Unfocus, HasFocus and
			// HasAnyActiveFocus, all of them per view, and calling them would
			// compile and would do something. Three measured facts say not to.
			//
			// EACH FACT CARRIES THE VERSION IT WAS MEASURED AGAINST, because a
			// fact about a vendor's behaviour with no version on it cannot be
			// retested and goes stale in silence. Facts 1 and 2 were measured
			// against Prisma UI 1.4.1 and 1.5.0; fact 3 is dated in its own text.
			//
			//   1. Focus is not a local operation here. The framework routes
			//      input for the whole PROCESS, not per view: one elected view
			//      id, one capture flag, one "a text field has focus" flag.
			//      Focusing one view takes the keyboard from every other Prisma
			//      consumer in the game, including ones that never heard of
			//      Lodestone.
			//      RECONFIRMED ON 1.5.1 (2026-09-22): with the focus rework in
			//      that release - MainThreadQueue, gameplayControlsOwner - a
			//      third-party SKSE mod hotkey still fired while a bridge view
			//      held focus. Capture by process survived the fix.
			//   2. Unfocus closes the framework's single kModal focus menu for
			//      every view at once, so a second view on screen is left with a
			//      stranded cursor. A sibling project of this tree exhausted the
			//      four-way flag matrix in game - both pauseGame and
			//      disableFocusMenu, all combinations - and found no mitigation.
			//      The two public flags do not touch the broken path.
			//      NOT RETESTED SINCE. Dating fact 1 does not date this one: the
			//      quick reopen that 1.5.1 claims to fix has not been exercised
			//      by any run of this tree.
			//   3. There is no panic key to escape with. The other backend
			//      publishes an unswallowable chord of its own
			//      (ToggleBrowserFocusByKeys, IBrowser.h); this API has no
			//      equivalent, and Lodestone installs no input sink anywhere, so
			//      a stranded cursor here would leave killing the process as the
			//      only way out.
			//
			// Answering false costs a consumer an interactive panel on this
			// backend and costs it nothing else: it asks, it gets an honest no,
			// and it degrades. Answering true would trade that for a failure
			// mode the player pays for and cannot escape.
			//
			// THIS IS A DECISION, NOT A CEILING. It reverses the day somebody
			// measures the deferred-unfocus path in game and can say what
			// HasAnyActiveFocus() actually reports while an unfocus is still
			// queued. Growing false into true is invisible to every consumer
			// that already asks first, which is why the capability shipped
			// before the feature.
			// ------------------------------------------------------------------
			//
			// WHAT 1.27.0 DOES WITH FACTS 1 AND 2: says them to the consumer
			// instead of deciding for it. Focusing a view here takes the keyboard
			// from every Prisma panel in the game, including mods that never
			// heard of Lodestone, and an unfocus closes the focus menu for all of
			// them. Lodestone.psc states both where a consumer reads
			// "view-focus".
			//
			// THE ASYNCHRONY, measured by a sibling project of this tree and
			// relied on below: Focus and Unfocus take effect a task-queue turn
			// later, and HasFocus read in the same frame answers from before the
			// command. Focus returns early without reinstalling keyboard capture
			// when HasFocus already answers true, even stale. And Focus on a
			// document that has not loaded silently loses the framework's
			// focus-tracking script - which the coordinator already rules out, by
			// dispatching only for a view whose DOM-ready has fired.
			//
			// THE DEFERRED UNFOCUS, MEASURED HERE, which is the question the
			// comment above said the false was waiting on. Phase L-U7, E4, in
			// game on 2026-09-17, Prisma UI 1.5.0, with a temporary probe around
			// every Unfocus this file sends: six calls, four from ClearFocus and
			// two from the panic chord. In ALL SIX, HasFocus and
			// HasAnyActiveFocus both still answered true in the same call, right
			// after Unfocus returned. In all six the next poll saw HasFocus false
			// and HasAnyActiveFocus false, 49 to 73 ms later. So the unfocus is
			// deferred, it lands within one poll interval, and the mirror
			// converges - it never went on claiming a focus that had left.
			//
			// What that window means for the code: inside those tens of
			// milliseconds a read of HasFocus is stale true. SetFocus in that
			// window answers true without focusing, and the poll then reports
			// the loss; ClearFocus or the chord in that window send a second
			// Unfocus, which changes nothing a player can see. Both converge.
			//
			// THE FLAGS: pauseGame false and disableFocusMenu false. Not pausing
			// is what the Meridian backend does through this same bridge - a panel
			// is an overlay, and the coordinator's automatic release keys on menus
			// that pause. The focus menu stays enabled because it is what gives
			// the player a cursor; the four-way matrix changed nothing about the
			// stranded cursor, so there is no mitigation to choose instead.
			bool SetFocus(ViewHandle a_view) override
			{
				if (!g_api || !Find(a_view)) {
					return false;
				}

				// No escape hatch, no focus. This is the condition the capability
				// was false for, so it is checked where the focus is given, not
				// only assumed from the view having been created.
				InstallPanicSink();
				if (!g_panicSinkInstalled) {
					spdlog::error("PrismaUIBackend: refusing focus - the panic chord is not installed, "
								  "and focus without a way out is what this backend may not give.");
					return false;
				}

				const auto view = static_cast<PrismaView>(a_view);

				// Already focused, or a stale true from an unfocus still in the
				// queue. Either way Focus would return without doing anything,
				// so the call is skipped and the poll reports what settles.
				if (g_api->HasFocus(view)) {
					return true;
				}

				return g_api->Focus(view, false, false);
			}

			// Safe on a view that does not hold focus, as the interface requires
			// - and here that takes a guard, not just a call. Unfocus closes the
			// focus menu of EVERY Prisma view, so an unguarded one on a view that
			// was never focused would take the cursor from another mod's panel.
			void ClearFocus(ViewHandle a_view) override
			{
				auto* record = Find(a_view);
				if (!g_api || !record) {
					return;
				}

				const auto view = static_cast<PrismaView>(a_view);
				if (!record->focused && !g_api->HasFocus(view)) {
					return;
				}

				g_api->Unfocus(view);
			}
		};

		PrismaUIBackend g_backend;
	}

	namespace WebUIBackends
	{
		IWebUIBackend* PrismaUI()
		{
			return &g_backend;
		}
	}
}
