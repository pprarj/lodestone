// MeridianUIBackend.cpp
// Lodestone - Shared SKSE framework
//
// The Meridian UI half of the WebUI bridge. See WebUIBackend.h for the seam and
// WebUIBridge.h for what the bridge is.
//
// Meridian UI is an independently maintained fork of NirnLabUIPlatform, backed
// by CEF rather than Ultralight. Its API headers are vendored in
// extern/meridianui-api/ under their own MIT license - unlike Prisma's, which
// cannot be tracked here. See the README in that folder.
//
// NEITHER BACKEND IS A DEPENDENCY. Meridian is detected at runtime and its
// absence is the ordinary case, exactly like Prisma's.
//
// THREE THINGS DIFFER FROM PRISMA, AND THEY ARE WHAT THIS FILE IS FOR:
//
//   1. Acquisition is a two-step SKSE handshake, not a request. The pointer
//      does not exist until kInputLoaded. See Probe() and HandleSKSEMessage().
//   2. There is NO page-ready callback anywhere in the API - only the
//      IsPageLoaded() poll. So this file watches, on the game thread, and
//      reports readiness itself. See the watcher below.
//   3. JS bindings live under an object, not on the bare window, and calls into
//      the page go through ExecuteJavaScript rather than a dedicated entry
//      point. Both are papered over so a page written for one backend works on
//      the other. See kJsObjectName and Call().

#include "WebUIBackend.h"

#include "Config.h"
#include "WebUIPanicKeys.h"

#include "API.h"
#include "SKSELoader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Lodestone::Core
{
	namespace
	{
		// The scheme every view of this bridge is served over. The HOST comes
		// from the consumer - see BuildViewUrl below.
		//
		// IT WAS "mod://Lodestone/" UNTIL 1.23.0, ONE HOST FOR THE WHOLE BRIDGE,
		// AND THAT WAS WRONG. The reasoning was that a Papyrus consumer has no
		// name Meridian knows, so the framework would own the host and consumers
		// would stay apart by the folder their own asViewPath names. It produced
		// this, in the field:
		//
		//   Data\MeridianUI\Lodestone\IntelligenceMatters\   from one mod
		//   Data\MeridianUI\Lodestone\StrengthMatters\       from another
		//
		// Two mods, one directory. Under MO2 exactly ONE of them is visible and
		// which one depends on mod order - measured in game on 2026-09-08, three
		// times, with the roles swapping when the order was inverted.
		//
		// THE CAUSE IS THE VENDOR'S OWN TRAVERSAL GUARD, not a bug and not the
		// VFS. Section 4 of MeridianUI-AuthorGuide.md: the filesystem layer
		// "canonicalizes both your mod root and the resolved file through the OS
		// and refuses anything that does not land inside the root". The host root
		// canonicalizes to ONE real folder; the other mod's file, reachable
		// through the VFS, canonicalizes under a different real folder, lands
		// outside, and is refused. Prisma has no such guard, which is why four
		// consumers share Data\PrismaUI\views\ without trouble - that asymmetry
		// is real and is stated in Lodestone.psc.
		//
		// The same section states the layout that was never ours to redefine:
		// "Ship your UI assets under a folder named for your mod",
		// Data\MeridianUI\<YourModName>\, and "the host component is your mod
		// folder name". One level. A framework-owned host with consumers nested
		// under it is not a layout that guide ever described.
		constexpr const char* kUrlScheme = "mod://";

		// The JS object a page's inbound bindings hang off: Lodestone.<fn>(...).
		//
		// MERIDIAN REQUIRES A NON-EMPTY OBJECT NAME - JSFuncInfo rejects a null
		// or empty one - so the bare window global that Prisma injects is not
		// directly available. Rather than make consumers write two pages, every
		// registration also gets a bare-window alias installed on page load, so
		// window.<fn>(...) works here exactly as it does under Prisma. The
		// object form is the canonical one; the alias is compatibility.
		constexpr const char* kJsObjectName = "Lodestone";

		// How often a loading view is asked whether its page is up, and how long
		// it is given before this module stops watching.
		//
		// MEASURED IN TIME, NOT IN POLLS, AND THE FIRST VERSION OF THIS GOT IT
		// WRONG IN A WAY WORTH KEEPING WRITTEN DOWN. It counted polls and
		// requeued itself through SKSE's task interface, on the assumption that
		// a task added from inside a task runs on the NEXT frame. It does not:
		// the queue is drained in a loop, and a task added during the drain is
		// picked up by the same drain. Measured in game on 2026-09-07 - a
		// budget of 1800 "frames" was spent in ONE MILLISECOND:
		//
		//   [07:39:34.671] view 'StrengthMatters' created ...
		//   [07:39:34.672] a view did not finish loading after 1800 frames
		//
		// So the view was abandoned a millisecond after being asked for, the
		// ready event never fired, and the consumer waited forever for it. The
		// page was fine and never got a chance to load.
		//
		// Two things changed. The budget is wall-clock, which is what the
		// question was always about. And the waiting is done by a thread that
		// SLEEPS, which posts one task per interval to do the actual asking -
		// because the browser is only safe to touch on the game thread, and
		// re-queuing was a busy spin on it.
		constexpr auto kReadyPollInterval = std::chrono::milliseconds(100);
		constexpr auto kReadyTimeout      = std::chrono::seconds(30);

		// Giving up does NOT destroy the view: it stops the watching. The view
		// stays in the bridge's table, WebUIGetViewState keeps answering 0
		// (created, not loaded), and the consumer can still destroy it.

		// The SKSE plugin Meridian UI ships. Its presence in the process is the
		// cheapest honest answer to "is Meridian installed", and asking before
		// acting is the whole reason this constant exists - see g_installed.
		constexpr const char* kPluginModule = "MeridianUIPlugin.dll";

		// Whether that module is loaded. Settled in Probe() and never revisited:
		// SKSE has loaded every plugin before kPostLoad, so the answer cannot
		// change afterwards.
		//
		// NOTHING HAPPENS WHEN THIS IS FALSE, AND THAT IS A BUG FIX RATHER THAN
		// AN OPTIMIZATION. Both halves of the handshake name Meridian as their
		// peer, and CommonLibSSE-NG logs when a peer is not there:
		//
		//   [error]   Failed to register messaging listener for MeridianUI
		//   [warning] Failed to dispatch message to MeridianUI
		//
		// Those come from Interfaces.cpp:304 and :285 of the pinned v6.7.1, not
		// from this plugin, so they cannot be silenced by changing what this
		// file logs - only by not making the calls. Measured in game on
		// 2026-09-07 with Meridian absent: one error and one warning on every
		// load, which is the common case for anyone who never installs it.
		//
		// That is exactly what WebUIBridge.h forbids: an absent backend is the
		// normal state, and a probe that logs a failure teaches users to report
		// a non-problem.
		bool g_installed = false;

		// Written on the game thread during load, read from the Papyrus VM
		// thread by IsAvailable(). Null means Meridian is not installed, or has
		// not answered yet, or has shut down - all three are the same answer to
		// a caller.
		std::atomic<Meridian::UI::IUIPlatformAPI*> g_api{ nullptr };

		// Handed to Meridian by pointer, with its size, during the handshake, so
		// it has to outlive the dispatch. Defaults are deliberate: the
		// RingBuffer renderer, no remote debugging port, and no remote content -
		// the last of which is what keeps every browser pinned to mod://.
		Meridian::UI::Settings g_settings{};

		// One view, as this backend knows it.
		//
		// NO LOCK, AND THAT IS A CONTRACT RATHER THAN AN OMISSION. Every entry
		// point that touches this vector - the six view operations - is called
		// by the coordinator on the main game thread, and so is the watcher
		// below. The two things that arrive on other threads, the JS thunks and
		// IsAvailable(), do not read it.
		struct BrowserRecord
		{
			IWebUIBackend::ViewHandle handle  = 0;
			Meridian::CEF::IBrowser*  browser = nullptr;

			// Whether the page is loaded RIGHT NOW. This is a state, not a
			// milestone, and the difference is the whole reason the watcher
			// keeps looking after the first success - see aliases below.
			bool loaded = false;

			// Whether ViewReady has been reported. Once only: a consumer is
			// told a view became usable, and being told twice is a change to
			// published behavior, not a bug fix.
			bool announced = false;

			// Whether the timeout has already been logged, so a view that
			// never loads says so once instead of every hundred milliseconds.
			bool gaveUp = false;

			// When the browser was asked for, which is what kReadyTimeout is
			// measured against.
			std::chrono::steady_clock::time_point createdAt = std::chrono::steady_clock::now();

			// Every bare-window alias this view has been given, kept for the
			// life of the view rather than consumed on first use.
			//
			// RE-INSTALLED ON EVERY PAGE LOAD, AND THAT IS A BUG FIX. An alias
			// is a script, so it belongs to the document that was loaded when
			// it ran - a page load replaces that document and the alias is
			// gone. The binding underneath it survives, because the platform
			// replays AddFunctionCallback registrations on load (IBrowser.h),
			// so the two would silently drift apart: Lodestone.<fn> keeps
			// working and window.<fn> stops existing.
			//
			// That failure is invisible from the game. The page calls
			// window.<fn> and nothing happens - no error, no log line, and the
			// consumer's inbound channel is simply dead. It was found by
			// reasoning from a position reset reported on
			// 2026-09-07, not by anything failing loudly.
			std::vector<std::string> aliases;

			// Whether the browser held focus at the last poll.
			//
			// OBSERVED, NOT REQUESTED, and that distinction is the reason the
			// bridge can be honest about focus at all. Meridian moves focus for
			// reasons this file never sees: it arbitrates between every
			// consumer in the process, and its own panic chord toggles focus
			// straight inside the platform. Both are invisible from here except
			// by asking IsBrowserFocused(), which is what the watcher does.
			bool focused = false;

			// Whether the panic chord is currently registered on this browser.
			//
			// ARMED ONLY WHILE THE BROWSER ACTUALLY HOLDS FOCUS. Arming every
			// browser at creation was the first design and is wrong: the chord
			// is a TOGGLE, and Meridian evaluates a registered chord for every
			// browser, so a press would also toggle focus ON for a hidden view -
			// capturing input with nothing on screen, which is the exact
			// stranded state the chord exists to escape.
			bool chordArmed = false;
		};

		std::vector<BrowserRecord> g_browsers;

		BrowserRecord* Find(IWebUIBackend::ViewHandle a_view)
		{
			for (auto& record : g_browsers) {
				if (record.handle == a_view) {
					return &record;
				}
			}
			return nullptr;
		}

		// --- Building the view URL ----------------------------------------------

		// Splits a consumer's view path into the mod:// host and the rest.
		//
		// THE FIRST SEGMENT BECOMES THE HOST, and it is not an arbitrary pick
		// between two equally good options. The alternative was a_viewId, which
		// this backend already receives and already hands over as the browser
		// name - and measuring the consumers of the day settled it:
		//
		//   viewId "IntelligenceMatters"            path "IntelligenceMatters/index.html"
		//   viewId "IntelligenceMattersMeditation"  path "IntelligenceMatters/meditation.html"
		//   viewId "IntelligenceMattersResearch"    path "IntelligenceMatters/research.html"
		//   viewId "StrengthMatters"                path "StrengthMatters/index.html"
		//
		// TWO OF THE FOUR DIVERGE, and they diverge for a good reason: a view id
		// has to be UNIQUE PER VIEW - it keys the coordinator's table - while a
		// host has to be SHARED PER MOD. They are different things, and that
		// consumer had already treated them as different. Using the id as host
		// would have given one mod three separate host folders, which is the
		// opposite of what the guide asks for.
		//
		// Returns false when the path has no separator, which means the consumer
		// named a file at the root with no mod folder around it. The caller
		// reports that; it is not something to paper over, because the guide has
		// no default host any more than it has a default document.
		bool SplitViewPath(const char* a_viewPath, std::string& a_host, std::string& a_rest)
		{
			if (!a_viewPath || !*a_viewPath) {
				return false;
			}

			const std::string_view path(a_viewPath);

			// Backslashes are normalized first, exactly as the scheme handler
			// does, so a consumer that wrote a Windows-style path gets the same
			// answer here as it would get there.
			std::size_t split = path.find_first_of("/\\");
			if (split == std::string_view::npos || split == 0 || split + 1 >= path.size()) {
				return false;
			}

			a_host.assign(path.substr(0, split));
			a_rest.assign(path.substr(split + 1));

			for (auto& c : a_rest) {
				if (c == '\\') {
					c = '/';
				}
			}

			return true;
		}

		// Whether a string may be used as a mod:// host.
		//
		// THIS CHECK IS NEW IN 1.23.0 AND IT IS NOT DEFENSIVE HABIT. Until then
		// the host was a constant of this plugin; now it is derived from a string
		// a consumer wrote, and the vendor's guard rejects a whole family of
		// them - section 4 lists an empty host, a host containing a separator,
		// and one ending in a dot or a space, which Windows silently strips at
		// open time.
		//
		// WHAT MAKES IT WORTH THE CODE IS THE FAILURE MODE, NOT THE ODDS. An
		// invalid host produces a 404, and a 404 in this backend is HTML that
		// loads: IsPageLoaded() answers true and the bridge announces the view as
		// ready, exactly as it would for a page that worked. So the alternative
		// to failing loudly here is not "a clear error later" - it is a panel
		// that never appears and a framework that says it did.
		//
		// The characters below are the vendor's list, not a guess at it. What is
		// NOT checked is percent-escaping: a consumer's view path is a folder
		// name, not a URL, so it cannot arrive pre-escaped, and rejecting '%'
		// outright would refuse a legal folder name.
		bool IsValidModHost(std::string_view a_host, const char*& a_reason)
		{
			if (a_host.empty()) {
				a_reason = "it is empty";
				return false;
			}

			if (a_host.find_first_of("/\\") != std::string_view::npos) {
				a_reason = "it contains a path separator";
				return false;
			}

			if (a_host.find(':') != std::string_view::npos) {
				a_reason = "it contains a colon";
				return false;
			}

			// Windows drops a trailing dot or space when opening, so the string
			// layer refuses them rather than let the two layers disagree.
			const char last = a_host.back();
			if (last == '.' || last == ' ') {
				a_reason = "it ends in a dot or a space";
				return false;
			}

			if (a_host == "." || a_host == "..") {
				a_reason = "it is a relative path segment";
				return false;
			}

			for (const unsigned char c : a_host) {
				if (c < 0x20 || c == 0x7F) {
					a_reason = "it contains a control character";
					return false;
				}
			}

			a_reason = nullptr;
			return true;
		}

		// --- Turning a Papyrus payload into JavaScript --------------------------

		// Whether a name is safe to paste into a script as an identifier.
		//
		// THE NAME COMES FROM PAPYRUS, so it is caller data, and Call() builds a
		// script out of it. Anything but a plain identifier is refused rather
		// than escaped: there is no legitimate view function named with a
		// parenthesis, and refusing is the only answer that cannot be wrong.
		bool IsSafeJsIdentifier(const char* a_name)
		{
			if (!a_name || !*a_name) {
				return false;
			}

			for (const char* p = a_name; *p; ++p) {
				const unsigned char c  = static_cast<unsigned char>(*p);
				const bool          ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
					(c >= '0' && c <= '9') || c == '_' || c == '$';
				if (!ok) {
					return false;
				}
			}

			const unsigned char first = static_cast<unsigned char>(*a_name);
			return !(first >= '0' && first <= '9');
		}

		// Wraps a payload as a JavaScript string literal.
		//
		// The bridge's contract is that the page receives the payload as ONE
		// string argument - the example view parses it with JSON.parse - so this
		// has to produce a string literal, not spliced-in JSON.
		//
		// U+2028 AND U+2029 ARE ESCAPED BY BYTE PATTERN, and they are the reason
		// this is not a three-case switch. Both are legal inside a JSON string
		// and both terminate a JavaScript string literal, so a payload carrying
		// one would produce a syntax error inside the page with nothing in any
		// log to explain it. Papyrus hands over bytes, so they are matched as
		// the UTF-8 sequences they arrive as.
		std::string ToJsStringLiteral(const char* a_raw)
		{
			std::string out;
			out.push_back('"');

			for (const unsigned char* p = reinterpret_cast<const unsigned char*>(a_raw ? a_raw : ""); *p; ++p) {
				if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0xA8 || p[2] == 0xA9)) {
					out += (p[2] == 0xA8) ? "\\u2028" : "\\u2029";
					p += 2;
					continue;
				}

				switch (*p) {
				case '"':
					out += "\\\"";
					break;
				case '\\':
					out += "\\\\";
					break;
				case '\n':
					out += "\\n";
					break;
				case '\r':
					out += "\\r";
					break;
				case '\t':
					out += "\\t";
					break;
				default:
					if (*p < 0x20) {
						char buf[8]{};
						std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(*p));
						out += buf;
					} else {
						out.push_back(static_cast<char>(*p));
					}
					break;
				}
			}

			out.push_back('"');
			return out;
		}

		// Installs window.<fn> as a forwarder to Lodestone.<fn>.
		//
		// This is what makes a page written against Prisma work here unchanged.
		// Guarded on the object existing, because a page that loaded before the
		// binding was replayed would otherwise throw on the assignment.
		void InstallAlias(Meridian::CEF::IBrowser* a_browser, const std::string& a_function)
		{
			std::string script;
			script += "if (window.";
			script += kJsObjectName;
			script += " && typeof window.";
			script += kJsObjectName;
			script += ".";
			script += a_function;
			script += " === 'function') { window.";
			script += a_function;
			script += " = function () { return window.";
			script += kJsObjectName;
			script += ".";
			script += a_function;
			script += ".apply(null, arguments); }; }";

			a_browser->ExecuteJavaScript(script.c_str());
		}

		// --- Watching for the page to load --------------------------------------

		// How many live views there are. Read by the watcher thread, written on
		// the game thread.
		//
		// EVERY VIEW IS WATCHED FOR ITS WHOLE LIFE, not just until it first
		// loads, because a page can be replaced afterwards and the aliases have
		// to go back in when it is. The cost of that is two virtual calls per
		// view per hundred milliseconds - measured against a game frame, it is
		// nothing, and it buys a channel that would otherwise die in silence.
		std::atomic<int> g_watchedViews{ 0 };

		// Whether the watcher thread has been started. It is started at most
		// once per process.
		std::atomic<bool> g_watcherStarted{ false };

		// --- The panic chord ----------------------------------------------------
		//
		// THE PLAYER'S WAY OUT, AND NO CONSUMER CAN TURN IT OFF. A view holding
		// focus swallows mouse and keyboard, so a panel whose script stops
		// running - a broken page, a Papyrus error, a mod uninstalled mid-save -
		// would otherwise leave killing the game as the only way to move again.
		//
		// IT IS THE BACKEND'S, NOT THIS PLUGIN'S, AND THAT IS WHY IT WORKS.
		// IBrowser::ToggleBrowserFocusByKeys registers the chord inside Meridian,
		// and the header states the guarantee as a 1.0 contract: the chord is
		// evaluated for EVERY browser before any focused browser can swallow the
		// event and consumes it, so the page never sees the chord. A sink of
		// Lodestone's promises neither: on the other backend, in game on
		// 2026-09-17, a focused page received the same chord the sink saw.
		//
		// THE OTHER BACKEND HAS NO SUCH CHORD, and until 1.27.0 that was the
		// reason it answered false to "view-focus". Since 1.27.0 it gets its
		// escape hatch from an input sink of Lodestone's, which releases rather
		// than toggles - see PrismaUIBackend.cpp. The setting is the same one: the
		// reader moved to WebUIPanicKeys.cpp, and the lines it writes here are
		// unchanged.

		// Arms or disarms the chord on one browser. GAME THREAD ONLY.
		//
		// Driven by what the watcher OBSERVES, never by what was requested, so
		// the chord follows the browser that actually holds focus even when this
		// plugin did not put it there.
		void SetChord(BrowserRecord& a_record, bool a_arm)
		{
			if (!a_record.browser || a_record.chordArmed == a_arm) {
				return;
			}

			if (a_arm) {
				// Read at the first arming, as it always was.
				const auto chord = WebUIPanicKeys::Get("MeridianUIBackend");
				a_record.browser->ToggleBrowserFocusByKeys(chord.first, chord.second);
			} else {
				// Zeros disable, per IBrowser.h.
				a_record.browser->ToggleBrowserFocusByKeys(0, 0);
			}

			a_record.chordArmed = a_arm;
		}

		// One pass over every view still loading. GAME THREAD ONLY - it is
		// posted there by the watcher thread and touches g_browsers, which
		// belongs to that thread.
		//
		// WHY ASKING AT ALL, AND IT IS NOT A SHORTCUT: the Meridian API has no
		// page-ready callback anywhere. IBrowser offers IsBrowserReady() and
		// IsPageLoaded(), both of them questions. Prisma hands the bridge a
		// callback; this file has to earn the same event.
		void PollReady()
		{
			const auto now = std::chrono::steady_clock::now();

			for (auto& record : g_browsers) {
				if (!record.browser) {
					continue;
				}

				const bool nowLoaded = record.browser->IsBrowserReady() && record.browser->IsPageLoaded();

				// The rising edge, and it happens more than once. The first one
				// is the view becoming usable; every later one is the page
				// having been replaced under us, which is when the aliases have
				// to go back in.
				if (nowLoaded && !record.loaded) {
					record.loaded = true;

					for (const auto& function : record.aliases) {
						InstallAlias(record.browser, function);
					}

					if (!record.announced) {
						record.announced = true;
						try {
							WebUIBackendCallbacks::ViewReady(WebUIBackends::MeridianUI(), record.handle);
						} catch (...) {
							spdlog::error("MeridianUIBackend: reporting a ready view threw - the view "
										  "is usable but no ready event was sent.");
						}
					} else {
						// Worth a line, because it is the only visible trace
						// that a consumer's page state was thrown away. The
						// bridge does NOT re-announce readiness here - see the
						// note on `announced`.
						spdlog::info("MeridianUIBackend: a view's page reloaded - JS bindings were "
									 "re-installed, but anything the page itself was holding is gone.");
					}
					continue;
				}

				if (!nowLoaded && record.loaded) {
					record.loaded = false;
					continue;
				}

				if (!record.announced && !record.gaveUp && now - record.createdAt >= kReadyTimeout) {
					record.gaveUp = true;
					spdlog::error("MeridianUIBackend: a view did not finish loading after {} seconds - "
								  "no ready event will be sent for it. Check that the page exists "
								  "under Data\\MeridianUI\\Lodestone.",
						std::chrono::duration_cast<std::chrono::seconds>(kReadyTimeout).count());
				}
			}
		}

		// One pass over every view's focus. GAME THREAD ONLY, same as PollReady
		// and posted by the same thread on the same interval.
		//
		// WHY POLLING AND NOT A CALLBACK: there is none. Meridian has no
		// focus-changed notification anywhere in the API - the same gap that
		// forced PollReady to earn the page-ready event by asking. IBrowser
		// offers IsBrowserFocused(), a question, and this is where it gets asked.
		//
		// WHY IT HAS TO BE ASKED AT ALL, rather than the bridge remembering what
		// it requested. Focus moves without this plugin at least three ways: the
		// platform arbitrates it away to another consumer, the player presses the
		// panic chord, and a browser loses it on teardown. A remembered value
		// would claim a consumer still holds focus the player escaped, and the
		// bridge's single-holder rule would then refuse everybody, forever, with
		// nothing in the log to say why.
		//
		// The cost is one virtual call per view per hundred milliseconds, on top
		// of the two PollReady already makes, and it buys the panic chord its
		// arming: the chord follows the browser that actually holds focus.
		void PollFocus()
		{
			for (auto& record : g_browsers) {
				if (!record.browser) {
					continue;
				}

				const bool nowFocused = record.browser->IsBrowserFocused();
				if (nowFocused == record.focused) {
					continue;
				}

				record.focused = nowFocused;

				// Armed on the way in, disarmed on the way out. Done before the
				// bridge is told, so the escape hatch is live by the time any
				// consumer can learn it has focus.
				SetChord(record, nowFocused);

				try {
					WebUIBackendCallbacks::FocusChanged(
						WebUIBackends::MeridianUI(), record.handle, nowFocused);
				} catch (...) {
					spdlog::error("MeridianUIBackend: reporting a focus change threw - the bridge's "
								  "view of who holds focus is now stale.");
				}
			}
		}

		// Sleeps, and posts one PollReady to the game thread per interval while
		// anything is loading.
		//
		// IT NEVER EXITS, AND THAT IS THE CHEAPER OF TWO CORRECT ANSWERS. A
		// thread that stops when the last view is ready and restarts on the next
		// create has a window where a create lands just as the thread decides to
		// leave, and the view is then never watched. Closing that costs a
		// compare-exchange dance; keeping the thread costs one atomic read every
		// hundred milliseconds, for the rest of the session, and only in a
		// process where Meridian is installed AND a view was actually created.
		//
		// It touches nothing but the two atomics and the task interface. All
		// browser access happens inside the posted task, on the game thread.
		void WatcherLoop()
		{
			for (;;) {
				std::this_thread::sleep_for(kReadyPollInterval);

				if (g_watchedViews.load(std::memory_order_acquire) <= 0) {
					continue;
				}

				// Shutting down. Nothing may touch a browser after this, and
				// there is nothing left to report.
				if (!g_api.load(std::memory_order_acquire)) {
					continue;
				}

				if (auto* task = SKSE::GetTaskInterface()) {
					task->AddTask([]() {
						PollReady();
						PollFocus();
					});
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

		// --- JS listener thunks -------------------------------------------------
		//
		// Meridian's JSFuncCallback is void(*)(const char** args, int count) -
		// like Prisma's, it carries NO context argument, so a capturing lambda
		// cannot be handed over and the slot has to be baked into the function's
		// type. Same table, same reason, different signature.
		//
		// executeInGameThread is left at its default of true (JSTypes.h:15), so
		// these arrive on the game thread rather than a renderer thread. That
		// does NOT let this file skip the bridge's task queue: SendModEvent
		// still goes through it, because being on the game thread is a property
		// of this backend and not of the bridge's contract.
		//
		// Wrapped because they return into Meridian.
		template <std::size_t N>
		void ListenerThunk(const char** a_args, int a_argsCount)
		{
			try {
				const char* argument = (a_args && a_argsCount > 0) ? a_args[0] : nullptr;
				WebUIBackendCallbacks::ListenerFired(N, argument);
			} catch (...) {
				spdlog::error("MeridianUIBackend: a JS listener threw on slot {} - the mod event was "
							  "not sent.",
					N);
			}
		}

		template <std::size_t... I>
		constexpr std::array<Meridian::JS::JSFuncCallback, sizeof...(I)> MakeThunkTable(std::index_sequence<I...>)
		{
			return { &ListenerThunk<I>... };
		}

		const auto g_thunks = MakeThunkTable(std::make_index_sequence<kWebUIMaxListeners>{});

		// The names Meridian holds by pointer. JSFuncInfo takes const char* and
		// the platform keeps the binding past the call, so the strings backing
		// those pointers have to outlive it.
		std::array<std::string, kWebUIMaxListeners> g_slotNames{};

		// --- The API arriving ---------------------------------------------------

		void OnShutdown()
		{
			// "After this callback, you should stop using any browser" - API.h.
			// Dropping the pointer makes IsAvailable() false, and every native
			// then answers its sentinel instead of touching a dead browser.
			g_api.store(nullptr, std::memory_order_release);
			g_browsers.clear();
			spdlog::info("MeridianUIBackend: Meridian UI is shutting down - bridge inactive.");
		}

		void OnApiReady(Meridian::UI::IUIPlatformAPI* a_api)
		{
			if (!a_api) {
				return;
			}

			a_api->RegisterOnShutdown(&OnShutdown);
			g_api.store(a_api, std::memory_order_release);
		}

		// --- The backend --------------------------------------------------------

		class MeridianUIBackend final : public IWebUIBackend
		{
		public:
			const char* Name() const override { return "MeridianUI"; }
			const char* DisplayName() const override { return "Meridian UI"; }
			// The root a consumer installs into, phrased to drop into an error
			// message. It lost the Lodestone level in 1.23.0 - see kUrlScheme.
			const char* ViewRootHint() const override { return "Data\\MeridianUI for Meridian UI"; }

			// Arms the handshake and nothing else.
			//
			// NO POINTER EXISTS YET WHEN THIS RETURNS, and that is the whole
			// difference from Prisma. This registers the listener that will
			// receive Meridian's replies; the requests that provoke them go out
			// from HandleSKSEMessage below, at kPostPostLoad and kInputLoaded.
			void Probe() override
			{
				// Ask whether Meridian is here before saying its name to
				// anything. See g_installed for the two log lines this silences
				// and why they are not ours to silence any other way.
				g_installed = REX::W32::GetModuleHandleA(kPluginModule) != nullptr;
				if (!g_installed) {
					return;
				}

				Meridian::UI::SKSELoader::GetUIPlatformAPIWithVersionCheck(&OnApiReady);
			}

			// The loader's own dispatcher, which owns both halves of the
			// handshake: it sends RequestVersion at kPostPostLoad and RequestAPI
			// at kInputLoaded, and it checks the reply's version against the
			// headers this DLL was built with.
			//
			// IT HAS TO BE CALLED FROM HERE and not only from the listener that
			// Probe() registered. That listener is registered for messages whose
			// sender is "MeridianUI", so it never sees an SKSE lifecycle message
			// - the two branches inside ProcessSKSEMessage are fed from two
			// different places by design.
			void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg) override
			{
				// Same guard as Probe(), and for the same reason: the dispatch
				// inside names Meridian as the receiver, and a receiver that is
				// not loaded is a warning in the user's log.
				if (!g_installed) {
					return;
				}

				Meridian::UI::SKSELoader::ProcessSKSEMessage(a_msg, &g_settings);
			}

			bool IsAvailable() const override
			{
				return g_api.load(std::memory_order_acquire) != nullptr;
			}

			bool HasCapability(const char* a_capability) const override
			{
				if (!a_capability) {
					return false;
				}

				const std::string_view capability(a_capability);

				// "focus-stack": can two views hold focus independently.
				//
				// False, and for a different reason than Prisma's. Meridian
				// arbitrates rather than failing to stack: exactly one browser
				// holds focus at a time across every consumer, by design. A
				// consumer asking this wants to know whether it can rely on two
				// views being independently focusable, and the answer is no on
				// both backends - which is the point of asking about the
				// capability instead of the backend's name.
				// IT STAYS false IN 1.22.0, WHICH IS THE VERSION MOST LIKELY TO
				// MAKE SOMEBODY "FIX" IT. This backend gained a working focus
				// surface in that version and answers true to "view-focus"
				// below, so the two lines now sit next to each other looking
				// contradictory. They are not. Meridian giving ONE view the
				// keyboard is precisely what it does; TWO views holding it
				// independently is precisely what its arbitration forbids.
				if (capability == "focus-stack") {
					return false;
				}

				// "view-focus": can ONE view be given the mouse and keyboard.
				//
				// THE PAIR OF NAMES IS A TRAP AND THIS IS THE SIGN ON IT:
				//
				//   focus-stack   can TWO views hold focus independently?   false
				//   view-focus    can ONE view receive a click at all?       true
				//
				// A consumer that asks the first meaning the second gets a
				// wrong answer here in the most expensive direction: it
				// concludes this backend cannot take a click, on the one
				// backend that can, and ships a panel that never offers input.
				//
				// True, and the three things that make it honest rather than
				// optimistic: focus is per browser (SetBrowserFocused,
				// IBrowser.h:57); it is observable, so the bridge can tell who
				// really holds it (IsBrowserFocused, :58); and there is an
				// unswallowable way out for the player
				// (ToggleBrowserFocusByKeys, :71, whose header states the
				// evaluation order as a 1.0 contract). The third is not a nicety
				// - it is what the other backend lacks, and why it answers
				// false.
				if (capability == "view-focus") {
					return true;
				}

				// "view-order": IBrowser::SetBrowserZOrder, higher draws on top.
				if (capability == "view-order") {
					return true;
				}

				// "inspector": a developer inspector opened on a view, the way
				// Prisma's CreateInspectorView does.
				//
				// False. Meridian's Settings::remoteDebuggingPort attaches an
				// external CEF debugger over a port; that is a different thing
				// from opening an inspector on a view, and answering True would
				// promise a consumer something it cannot get from this API.
				if (capability == "inspector") {
					return false;
				}

				return false;
			}

			ViewHandle CreateView(const char* a_viewId, const char* a_viewPath) override
			{
				auto* api = g_api.load(std::memory_order_acquire);
				if (!api || !a_viewId || !*a_viewId || !a_viewPath || !*a_viewPath) {
					return 0;
				}

				// THE FIRST SEGMENT OF THE VIEW PATH BECOMES THE HOST, and the
				// view id does NOT enter the URL. It is the browser's unique
				// name and nothing else.
				//
				// The consumer still passes ONE string and it is the same string
				// on both backends - what changed in 1.23.0 is where each
				// backend roots it:
				//
				//   Prisma UI    Data\PrismaUI\views\<asViewPath>
				//   Meridian UI  Data\MeridianUI\<asViewPath>
				//
				// So "IntelligenceMatters/index.html" becomes
				// mod://IntelligenceMatters/index.html here and
				// Data\PrismaUI\views\IntelligenceMatters\index.html there. The
				// consumer's folder moves up one level under Meridian and does
				// not move at all under Prisma.
				std::string host;
				std::string rest;
				if (!SplitViewPath(a_viewPath, host, rest)) {
					spdlog::error("MeridianUIBackend: view path '{}' has no mod folder in it. Meridian "
								  "serves a page from Data\\MeridianUI\\<YourMod>\\<file>, so the path "
								  "needs at least one folder before the file.",
						a_viewPath);
					return 0;
				}

				const char* reason = nullptr;
				if (!IsValidModHost(host, reason)) {
					// LOUD ON PURPOSE. See IsValidModHost: the alternative is a
					// 404 that this backend reports as a page that loaded.
					spdlog::error("MeridianUIBackend: '{}' cannot be a mod:// host because {}. The first "
								  "folder of a view path is the host, and Meridian's own guard rejects "
								  "this one - the view was not created.",
						host, reason);
					return 0;
				}

				std::string url = kUrlScheme;
				url += host;
				url += '/';
				url += rest;

				Meridian::CEF::IBrowser* browser = nullptr;

				// No bindings at creation: the bridge registers listeners later,
				// when a consumer asks, and AddFunctionCallback handles that.
				const auto handle = api->AddOrGetBrowser(a_viewId, nullptr, 0, url.c_str(), browser);

				if (handle == Meridian::UI::IUIPlatformAPI::InvalidBrowserRefHandle || !browser) {
					return 0;
				}

				g_browsers.push_back(BrowserRecord{ static_cast<ViewHandle>(handle), browser });

				// HIDDEN ON CREATE, AND THIS WAS MEASURED RATHER THAN ASSUMED.
				// Meridian does not document whether a browser starts visible.
				// The first version of this file asked and logged the answer
				// instead of guessing, and in game on 2026-09-07 the answer came
				// back TRUE - a browser is visible the moment it exists, and
				// with no rect set it is fullscreen. So a view that a consumer
				// created but has not shown yet would paint over the whole
				// screen as soon as its page loaded.
				//
				// Hiding here makes "created but not shown" mean the same thing
				// on both backends, which is what the .psc promises: a view
				// becomes visible when WebUIShow is called, not when it is
				// built.
				const bool visibleAtCreation = browser->IsBrowserVisible();
				browser->SetBrowserVisible(false);

				spdlog::info("MeridianUIBackend: view '{}' created from '{}' - hidden until WebUIShow "
							 "(the backend made it visible at creation: {}).",
					a_viewId, url, visibleAtCreation);

				g_watchedViews.fetch_add(1, std::memory_order_acq_rel);
				StartWatcher();
				return static_cast<ViewHandle>(handle);
			}

			void DestroyView(ViewHandle a_view) override
			{
				auto* api = g_api.load(std::memory_order_acquire);
				if (!api) {
					return;
				}

				for (std::size_t i = 0; i < g_browsers.size(); ++i) {
					if (g_browsers[i].handle == a_view) {
						// The view stops being watched when it stops existing,
						// and not before - see g_watchedViews.
						g_watchedViews.fetch_sub(1, std::memory_order_acq_rel);
						g_browsers.erase(g_browsers.begin() + static_cast<std::ptrdiff_t>(i));
						break;
					}
				}

				api->ReleaseBrowserHandle(static_cast<Meridian::UI::IUIPlatformAPI::BrowserRefHandle>(a_view));
			}

			void Show(ViewHandle a_view) override
			{
				if (auto* record = Find(a_view); record && record->browser) {
					record->browser->SetBrowserVisible(true);
				}
			}

			void Hide(ViewHandle a_view) override
			{
				if (auto* record = Find(a_view); record && record->browser) {
					record->browser->SetBrowserVisible(false);
				}
			}

			// ExecuteJavaScript, and NOT ExecEventFunction, which looks like the
			// closer fit and is not usable here.
			//
			// An event function has to be declared in the JSFuncInfo array at
			// creation, with isEventFunction set. The bridge does not know any
			// function name at creation: a consumer names one when it calls
			// WebUICall, arbitrarily far into the session. Declaring names that
			// have not been chosen yet is not possible, so the general path is
			// the only one, and it produces the same thing the .psc promises -
			// calling that name on the page, with the payload as its single
			// string argument.
			void Call(ViewHandle a_view, const char* a_function, const char* a_json) override
			{
				auto* record = Find(a_view);
				if (!record || !record->browser) {
					return;
				}

				if (!IsSafeJsIdentifier(a_function)) {
					spdlog::error("MeridianUIBackend: refusing to call '{}' - a view function name has "
								  "to be a plain JavaScript identifier.",
						a_function ? a_function : "");
					return;
				}

				std::string script = "if (typeof window.";
				script += a_function;
				script += " === 'function') { window.";
				script += a_function;
				script += "(";
				script += ToJsStringLiteral(a_json);
				script += "); }";

				record->browser->ExecuteJavaScript(script.c_str());
			}

			void RegisterListener(ViewHandle a_view, const char* a_jsFunction, std::size_t a_slot) override
			{
				auto* record = Find(a_view);
				if (!record || !record->browser || a_slot >= g_thunks.size()) {
					return;
				}

				if (!IsSafeJsIdentifier(a_jsFunction)) {
					spdlog::error("MeridianUIBackend: refusing to bind '{}' - a JS function name has to "
								  "be a plain JavaScript identifier.",
						a_jsFunction ? a_jsFunction : "");
					return;
				}

				// Meridian holds these by pointer, so the strings have to
				// outlive the call. One per slot, overwritten when the slot is
				// reused, which is safe because the bridge frees a slot only
				// after the binding it named is gone with its view.
				g_slotNames[a_slot] = a_jsFunction;

				Meridian::JS::JSFuncInfo info{};
				info.objectName                       = kJsObjectName;
				info.funcName                         = g_slotNames[a_slot].c_str();
				info.callbackData.callback            = g_thunks[a_slot];
				info.callbackData.executeInGameThread = true;
				info.callbackData.isEventFunction     = false;

				record->browser->AddFunctionCallback(info);

				// The bare-window alias that makes a Prisma-shaped page work
				// here. Remembered for the life of the view, because it is a
				// script and every page load throws it away - the watcher puts
				// it back on each one. Installed now as well if there is already
				// a document to install it into.
				if (std::find(record->aliases.begin(), record->aliases.end(), g_slotNames[a_slot]) ==
					record->aliases.end()) {
					record->aliases.push_back(g_slotNames[a_slot]);
				}

				if (record->loaded) {
					InstallAlias(record->browser, g_slotNames[a_slot]);
				}
			}

			// --- Focus ------------------------------------------------------
			//
			// Two one-line calls, and the machinery that makes them trustworthy
			// is elsewhere: PollFocus observes what actually happened, SetChord
			// keeps the player's escape hatch on the browser that holds focus,
			// and FocusChanged tells the bridge. Nothing below writes
			// record->focused - the watcher owns it, and a value written here
			// would be intent rather than observation.

			bool SetFocus(ViewHandle a_view) override
			{
				auto* record = Find(a_view);
				if (!record || !record->browser) {
					return false;
				}

				// A browser that has not finished loading has no page to give
				// input to, and focusing one would capture the mouse against a
				// blank surface.
				if (!record->loaded) {
					spdlog::warn("MeridianUIBackend: refusing focus for a view whose page has not "
								 "loaded yet.");
					return false;
				}

				// NO RETURN VALUE TO CHECK - SetBrowserFocused is void
				// (IBrowser.h:57), so "the platform accepted" is not a thing
				// this API can be asked. True here means the call was made, and
				// whether it took is answered by the next PollFocus, which is
				// the only honest answer available.
				record->browser->SetBrowserFocused(true);
				return true;
			}

			void ClearFocus(ViewHandle a_view) override
			{
				if (auto* record = Find(a_view); record && record->browser) {
					record->browser->SetBrowserFocused(false);
				}
			}
		};

		MeridianUIBackend g_backend;
	}

	namespace WebUIBackends
	{
		IWebUIBackend* MeridianUI()
		{
			return &g_backend;
		}
	}
}
