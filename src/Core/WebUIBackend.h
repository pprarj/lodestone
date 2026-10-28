// WebUIBackend.h
// Lodestone - Shared SKSE framework
//
// The seam between the WebUI bridge and whichever web UI framework is actually
// installed. See WebUIBridge.h for what the bridge is and why it exists.
//
// WHY THIS SEAM EXISTS AT ALL. The 1.18.0 rename argued that "swapping one
// pointer for a vector of adapters later is invisible to every consumer". That
// was a promise, and this file is the first half of testing it: the bridge is
// split here without adding a single behavior, so that if the promise was wrong
// it fails now, with one backend and one consumer, instead of later with four.
//
// WHAT BELONGS ON WHICH SIDE OF IT. The split is not "engine vs logic" - it is
// "does the answer change when the installed framework changes".
//
//   Backend      the vendor's API, its handle type, its callback shapes, its
//                view root, its name, and what it can and cannot do.
//   Coordinator  the view table keyed by the consumer's own string id, the
//                listener slot pool, the mod events, the main-thread dispatch,
//                the single-holder focus policy, and all 25 natives. None of
//                that changes with the vendor.
//
// Test, when it is not obvious: if the code would read identically for a
// framework nobody has written yet, it is the coordinator's.
//
// THE OPERATIONS ARE CALLED ON THE MAIN GAME THREAD, ALWAYS. The coordinator
// owns that decision and every one of them arrives through its task queue - see
// DispatchToGame in WebUIBridge.cpp for why that is not optional for a native
// running on the Papyrus VM thread. A backend never has to queue anything of its
// own, and must not assume it may block.
//
// REPORTING BACK RUNS THE OTHER WAY AND ON ANY THREAD. A backend calls the
// three functions in WebUIBackendCallbacks below from whatever thread its
// framework hands it, which is generally not the game thread. Those three are
// the only entry points a backend has into the coordinator.

#pragma once

#include <cstddef>
#include <cstdint>

namespace Lodestone::Core
{
	// Maximum number of JS listener slots, shared by every view and every mod.
	//
	// It lives here rather than in the coordinator because a backend whose
	// callback type carries no context argument has to emit one distinct
	// function per slot, and therefore has to know the count at compile time.
	// Prisma UI's JSListenerCallback is exactly that shape.
	//
	// 32 was chosen as the smallest number that is not going to be met.
	// Consumers register a handful of callbacks per panel. Raising it costs one
	// constant and a recompile.
	inline constexpr std::size_t kWebUIMaxListeners = 32;

	// One installed web UI framework, as the bridge needs to see it.
	//
	// Implementations are stateless singletons owned by their own translation
	// unit and handed out by the accessors at the bottom of this file. They are
	// never created, destroyed or copied: a backend outlives every view it
	// makes, and the process ends before it does.
	class IWebUIBackend
	{
	public:
		// A view, as the backend knows it.
		//
		// WIDE ON PURPOSE, AND NEVER SEEN BY PAPYRUS. Prisma UI's PrismaView is
		// a uint64_t; other frameworks use narrower handles. The widest type
		// carries all of them without a cast at the call site. It is not the
		// key of anything - the consumer names its view with a string it chose,
		// which is what the coordinator's map is keyed by, because the Papyrus
		// Int is 32 bits and would truncate this.
		//
		// Zero means "no view". Every backend here must treat it that way.
		using ViewHandle = std::uint64_t;

		virtual ~IWebUIBackend() = default;

		// --- Identity ---------------------------------------------------------

		// The token WebUIGetBackend() hands to Papyrus. Stable, no spaces: a
		// consumer may print it, and a player may paste it into a bug report.
		virtual const char* Name() const = 0;

		// The vendor's own spelling, for the log line a human reads.
		//
		// Separate from Name() because the two genuinely differ - "PrismaUI" is
		// the wire token, "Prisma UI" is what the mod is called - and collapsing
		// them would change one of the two in a way somebody is already reading.
		virtual const char* DisplayName() const = 0;

		// Where this backend looks for view files, phrased to be dropped into an
		// error message. The bridge cannot say this itself: the path root is the
		// vendor's convention, and it is the single most useful thing to print
		// when a view fails to build.
		virtual const char* ViewRootHint() const = 0;

		// --- Lifecycle --------------------------------------------------------

		// Looks for the framework and takes hold of its API.
		//
		// Called once, from the bridge's Acquire(). Absent is a normal outcome
		// and not an error: it leaves the backend unavailable and says so at
		// info level, never at error level. A probe that logs a failure teaches
		// users to report a non-problem.
		//
		// WHEN THIS RUNS IS THE BACKEND'S BUSINESS, and the two backends here
		// differ completely. Prisma UI answers a direct request and is settled
		// when Probe() returns. Meridian UI answers a two-step SKSE handshake
		// and is not available until kInputLoaded, so its Probe() only arms the
		// handshake. That is why the bridge does not choose a backend here -
		// see WebUIBridge.cpp, Resolve().
		virtual void Probe() = 0;

		// Every SKSE message, forwarded from plugin.cpp through the bridge.
		//
		// EXISTS BECAUSE ONE BACKEND IS ACQUIRED BY MESSAGE RATHER THAN BY
		// REQUEST. Meridian's loader has to see kPostPostLoad to ask for a
		// version and kInputLoaded to ask for the API; nothing else in this
		// plugin needs those two seams. Prisma's implementation is empty, and
		// that is the honest shape - a backend that needs no lifecycle sees
		// none.
		virtual void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg) = 0;

		// Whether this backend is present and usable right now.
		//
		// Read on every native call. Must be cheap and must not block. May go
		// from false to true during load, which is exactly what the handshake
		// backend does.
		virtual bool IsAvailable() const = 0;

		// --- Capability -------------------------------------------------------

		// Whether this backend supports a named capability.
		//
		// AN UNKNOWN NAME RETURNS false AND LOGS NOTHING. That property is what
		// lets the vocabulary grow without a major version: a consumer built
		// against a newer Lodestone asks an older DLL about a name it never
		// heard of and gets the correct answer.
		//
		// Answered from what the backend can do, never from which backend it is.
		virtual bool HasCapability(const char* a_capability) const = 0;

		// --- View operations --------------------------------------------------
		//
		// All six run on the main game thread. All six are given a handle this
		// backend itself produced, except CreateView which produces one.

		// Builds a view from a path relative to this backend's view root.
		//
		// THE VIEW ID IS PASSED BECAUSE ONE BACKEND NEEDS IT AS A NAME. Meridian
		// keys browsers by a unique string and returns the same browser for the
		// same name; the consumer's own view id is exactly that string, and
		// handing it over is what makes a repeated create idempotent on that
		// side too. Prisma has no such concept and ignores it.
		//
		// Returns 0 on failure. The backend is responsible for arranging that
		// WebUIBackendCallbacks::ViewReady fires for the returned handle once
		// the page has finished loading - by registering the framework's own
		// callback, or, where the framework offers none, by watching for it.
		virtual ViewHandle CreateView(const char* a_viewId, const char* a_viewPath) = 0;

		// Tears the view down and releases the handle.
		virtual void DestroyView(ViewHandle a_view) = 0;

		virtual void Show(ViewHandle a_view) = 0;
		virtual void Hide(ViewHandle a_view) = 0;

		// Calls a_function on the page's JS interop surface with a_json as its
		// single argument.
		virtual void Call(ViewHandle a_view, const char* a_function, const char* a_json) = 0;

		// Makes the page's JS call to a_jsFunction reach
		// WebUIBackendCallbacks::ListenerFired with a_slot.
		//
		// The slot index is chosen by the coordinator, which owns the pool. The
		// backend's only job is to get that number back out again when the page
		// fires - through a per-slot function, a context pointer, or whatever
		// its framework provides.
		//
		// a_slot is always below kWebUIMaxListeners.
		virtual void RegisterListener(ViewHandle a_view, const char* a_jsFunction, std::size_t a_slot) = 0;

		// --- Focus ------------------------------------------------------------
		//
		// Whether the view receives the game's mouse and keyboard. Both run on
		// the main game thread, like every other operation above.
		//
		// A BACKEND THAT ANSWERS false TO "view-focus" IS NEVER ASKED. The
		// coordinator checks the capability before it dispatches anything, so on
		// such a backend these two are unreachable rather than merely unused, and
		// their bodies say that instead of pretending to work.
		//
		// THIS IS NOT "focus-stack", AND THE TWO ARE DIFFERENT QUESTIONS. That
		// capability asks whether TWO views can hold focus independently, and it
		// answers false on every backend here. These two ask for ONE view to
		// receive input, which is a thing a backend can do while having no stack
		// at all. See HasCapability in each backend for the trap this pair of
		// names sets, and Lodestone.psc for the same warning facing consumers.

		// Gives the view the mouse and keyboard.
		//
		// Returns whether the backend accepted the request. False means nothing
		// changed, and the coordinator logs it - the return value cannot reach
		// the Papyrus caller, because by then the native has long since answered
		// on another thread.
		//
		// THE TRUTH ABOUT WHO HOLDS FOCUS COMES BACK THROUGH FocusChanged, NOT
		// FROM HERE. A backend that accepts may still lose focus a frame later,
		// to its own arbitration or to the player.
		virtual bool SetFocus(ViewHandle a_view) = 0;

		// Takes the mouse and keyboard away from the view.
		//
		// Must be safe to call on a view that does not hold focus: the
		// coordinator calls it on save load and on menu transitions without
		// asking first, because asking would cost a round trip to learn
		// something it is about to overwrite anyway.
		virtual void ClearFocus(ViewHandle a_view) = 0;
	};

	// The only way back in. Called by a backend, from any thread.
	namespace WebUIBackendCallbacks
	{
		// A view's page has finished loading.
		//
		// The handle is enough to find the view: handles are unique within a
		// backend, and only one backend is active per session. An unknown handle
		// is ignored - it means the view was destroyed between the framework's
		// call and this one.
		void ViewReady(IWebUIBackend* a_backend, IWebUIBackend::ViewHandle a_view);

		// A page fired the JS function registered against a_slot.
		//
		// a_argument may be null. It does not outlive the call, so the
		// coordinator copies it before doing anything asynchronous.
		void ListenerFired(std::size_t a_slot, const char* a_argument);

		// A view gained or lost the mouse and keyboard.
		//
		// SENT FOR EVERY CHANGE, INCLUDING THE ONES THE BRIDGE DID NOT CAUSE,
		// and that is the entire reason this exists rather than the coordinator
		// simply remembering what it last asked for. Focus moves without the
		// bridge in at least three ways: a backend that arbitrates hands it to
		// somebody else, the player presses the panic chord, and a
		// view is torn down while holding it. A mirror of intent would go on
		// claiming a view has focus that the player already escaped, and the
		// coordinator would then refuse the next consumer forever.
		//
		// A backend reports this from wherever it notices, which for one of them
		// is the same game-thread poll that already watches for page loads. An
		// unknown handle is ignored, like ViewReady's.
		void FocusChanged(IWebUIBackend* a_backend, IWebUIBackend::ViewHandle a_view, bool a_focused);
	}

	// The backends this DLL was built with.
	//
	// One accessor per backend rather than a table, because the set is fixed at
	// compile time and naming them here is what makes a missing one a link
	// error instead of an empty list at runtime. The bridge holds the order.
	namespace WebUIBackends
	{
		IWebUIBackend* PrismaUI();
		IWebUIBackend* MagelightUI();
		IWebUIBackend* MeridianUI();
	}
}
