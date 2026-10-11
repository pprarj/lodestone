// MenuPrompt.cpp
// Lodestone - Shared SKSE framework
//
// Implementation of the player prompt menus. Read MenuPrompt.h first - it
// carries what was measured in game and why the shape is what it is.
//
// Phase L-U8, 1.26.0.

// The framework's client header drags in <windows.h>, which is why it is
// included here and never from MenuPrompt.h: nothing else in this plugin
// should inherit those macros. The two defines below are the same guard the
// phase probe used.
//
// It also uses std::filesystem and names RE::InputEvent WITHOUT including
// either, so it only parses after CommonLibSSE has been seen. PCH.h is
// force-included in every file of this target, which is what satisfies that.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "SKSEMenuFramework.h"

#include "MenuPrompt.h"

// IVirtualMachine.h only declares ReturnLatentResult; its template body, and
// the LatentStatus enum, live in NativeLatentFunction.h. That header's
// RegisterLatentFunction is NOT used here - see LatentNative below for why.
#include "RE/N/NativeLatentFunction.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

namespace Lodestone::Core::MenuPrompt
{
	namespace
	{
		namespace SMF = SKSEMenuFramework;
		namespace IG = ImGuiMCP;

		// The text edit buffer, in BYTES. Text comes back as UTF-8 and its
		// length is counted in bytes, not characters - measured, see the
		// header. Generous for a name, and fixed so the render callback never
		// allocates.
		constexpr std::size_t kTextBufferBytes = 512;

		enum class Kind
		{
			kList,
			kText
		};

		// -------------------------------------------------------------------
		// THE ACTIVE PROMPT - at most one, ever.
		//
		// Everything here is written by the caller's thread in Begin* and read
		// by the render callback on a thread that is not the caller's. The
		// lock covers all of it. The render callback holds the lock for the
		// duration of one frame's drawing, which is cheap because the only
		// contention is a Begin* that is about to be refused anyway.
		//
		// THE ATOMIC IN FRONT OF THE LOCK exists so that Available() and
		// IsBusy() - which a script may call every frame while deciding
		// whether to offer a menu at all - never wait on a render.
		// -------------------------------------------------------------------
		struct Active
		{
			Kind                     kind{ Kind::kList };
			std::string              title;
			std::vector<std::string> entries;
			Completion               done;

			// Text prompt only. Owned by the render callback after Begin*
			// seeds it.
			std::array<char, kTextBufferBytes> buffer{};

			// Set on the first frame the window draws, so the render callback
			// can do its once-per-open work without a second flag per field.
			bool firstFrame{ true };

			// Cached on the first frame: the width the longest entry needs.
			// Measuring 300 strings every frame would be paid on every frame.
			float widest{ 0.0f };

			// Cached on the first frame: the width the title needs. The list
			// window is sized to whichever of the two is wider - sized to the
			// entries alone, a sentence title came out cut to its first words
			// (L-U8 E6, measured).
			float titleWidth{ 0.0f };

			// List prompt only: the size handed to ImGui on the frame the
			// window appeared. Compared with the size at close to tell a
			// window the player resized from one that only widened to fit.
			float placedWidth{ 0.0f };
			float placedHeight{ 0.0f };
		};

		std::mutex              g_lock;
		std::unique_ptr<Active> g_active;

		// The list size the player last dragged to, in pixels. Zero until the
		// player resizes one. Shared by every list prompt from every mod, and
		// kept for this run of the game only - nothing is written to disk, by
		// design. Guarded by g_lock.
		float g_listWidth{ 0.0f };
		float g_listHeight{ 0.0f };
		std::atomic<bool>       g_busy{ false };

		SMF::Model::WindowInterface* g_listWindow{ nullptr };
		SMF::Model::WindowInterface* g_textWindow{ nullptr };
		std::atomic<bool>            g_ready{ false };

		std::uint32_t Tid()
		{
			return static_cast<std::uint32_t>(::GetCurrentThreadId());
		}

		// Runs a_work on the main game thread.
		//
		// The same shape Core/WebUIBridge uses, and here for the same reason:
		// a completion is produced on the framework's render thread and the
		// consumer's continuation belongs on the game thread. AddTask is used
		// to change THREAD - never to wait, because it does not defer to the
		// next frame.
		//
		// NOTHING ESCAPES THIS FUNCTION. Its caller is a vendor's callback.
		void DispatchToGame(std::function<void()> a_work)
		{
			try {
				auto* task = SKSE::GetTaskInterface();
				if (!task) {
					spdlog::error("MenuPrompt: no SKSE task interface - result dropped.");
					return;
				}
				task->AddTask(std::move(a_work));
			} catch (...) {
				spdlog::error("MenuPrompt: AddTask threw - result dropped.");
			}
		}

		// Copies a_source into a_buffer, truncating at a UTF-8 character
		// boundary rather than mid-character.
		//
		// A cut through a multi-byte character produces bytes that are not
		// valid UTF-8, and those would travel into the VM as the player's
		// answer. Continuation bytes are 0b10xxxxxx, so backing off over them
		// lands on the start of whichever character did not fit.
		void CopyTruncated(std::array<char, kTextBufferBytes>& a_buffer, const std::string& a_source)
		{
			a_buffer.fill('\0');
			std::size_t len = a_source.size();
			if (len >= a_buffer.size()) {
				len = a_buffer.size() - 1;
				while (len > 0 && (static_cast<unsigned char>(a_source[len]) & 0xC0) == 0x80) {
					--len;
				}
				spdlog::info("MenuPrompt: suggestion of {} bytes truncated to {} - buffer is {} bytes.",
					a_source.size(), len, a_buffer.size());
			}
			if (len > 0) {
				std::memcpy(a_buffer.data(), a_source.data(), len);
			}
		}

		// Ends the active prompt and hands the result to the game thread.
		//
		// CALLED WITH g_lock HELD, and it takes the completion out of the
		// active prompt before releasing it, so the callback runs with no lock
		// of ours held. A consumer's continuation may well ask for another
		// prompt, and that must not deadlock against this one.
		void FinishLocked(Result a_result, const char* a_reason)
		{
			if (!g_active) {
				return;
			}

			Completion done = std::move(g_active->done);
			const Kind kind = g_active->kind;
			g_active.reset();
			g_busy.store(false);

			if (g_listWindow) {
				g_listWindow->IsOpen = false;
			}
			if (g_textWindow) {
				g_textWindow->IsOpen = false;
			}

			if (kind == Kind::kList && a_result.accepted) {
				spdlog::info("MenuPrompt: list prompt finished - answered index {} ({}).",
					a_result.index, a_reason);
			} else {
				spdlog::info("MenuPrompt: {} prompt finished - {} ({}).",
					kind == Kind::kList ? "list" : "text",
					a_result.accepted ? "answered" : "cancelled", a_reason);
			}

			if (done) {
				DispatchToGame([done = std::move(done), result = std::move(a_result)]() mutable {
					try {
						done(std::move(result));
					} catch (...) {
						spdlog::error("MenuPrompt: completion threw - swallowed.");
					}
				});
			}
		}

		// The size the framework is drawing at. Falls back to a common
		// resolution rather than to zero, so a missing io never produces a
		// window placed off screen.
		void ScreenSize(float& a_width, float& a_height)
		{
			auto* io = IG::GetIO();
			a_width = io ? io->DisplaySize.x : 1920.0f;
			a_height = io ? io->DisplaySize.y : 1080.0f;
		}

		// Centres the next window and gives it a size, ON THE FRAME IT APPEARS
		// ONLY - after that the player owns both. Width is capped at most of
		// the screen so that a long entry widens the window instead of
		// overflowing it; the ~40 character ceiling of the surface this
		// replaces is one of the reasons the phase exists.
		//
		// A height of zero means "let it size itself", which is what the text
		// prompt wants together with AlwaysAutoResize.
		void PlaceWindow(float a_width, float a_height, float a_screenW, float a_screenH)
		{
			if (a_width > a_screenW * 0.9f) {
				a_width = a_screenW * 0.9f;
			}

			IG::SetNextWindowSize(IG::ImVec2{ a_width, a_height }, IG::ImGuiCond_Appearing);
			IG::SetNextWindowPos(IG::ImVec2{ a_screenW * 0.5f, a_screenH * 0.5f }, IG::ImGuiCond_Appearing,
				IG::ImVec2{ 0.5f, 0.5f });
		}

		// Writes the input state ImGui holds, so that a prompt the gamepad
		// could not drive leaves a different line from one it could.
		//
		// Added after L-U8 E6 row 10: the same build, controller and framework
		// version failed twice and worked once, and the log of the failures was
		// identical to the log of the success. The phase probe wrote these
		// fields and the product module did not.
		//
		// Called on the first frame AND when the player closes the prompt. On
		// the first frame NavActive and NavVisible read false even when the
		// gamepad goes on to work (E1 probe, measured), so the first-frame line
		// answers only for the flags; the closing line is the one that says
		// whether navigation was live while the window was up.
		//
		// RENDER THREAD ONLY. The io belongs to the frame being drawn.
		void LogInputState(const char* a_kind, const char* a_when)
		{
			auto* io = IG::GetIO();
			if (!io) {
				spdlog::info("MenuPrompt: {} prompt input state {} - no io.", a_kind, a_when);
				return;
			}
			spdlog::info("MenuPrompt: {} prompt input state {} - ConfigFlags=0x{:X} NavEnableKeyboard={} "
						 "NavEnableGamepad={} NavActive={} NavVisible={}.",
				a_kind, a_when, static_cast<unsigned>(io->ConfigFlags),
				(io->ConfigFlags & IG::ImGuiConfigFlags_NavEnableKeyboard) != 0,
				(io->ConfigFlags & IG::ImGuiConfigFlags_NavEnableGamepad) != 0,
				io->NavActive, io->NavVisible);
		}

		// Escape cancels. The framework does not close these windows on it -
		// measured - so the module reads the key itself, from inside the
		// window, which is also the only place ImGui will report it.
		bool EscapePressed()
		{
			return IG::IsKeyPressed(IG::ImGuiKey_Escape, false);
		}

		void __stdcall RenderList()
		{
			try {
				std::lock_guard lock(g_lock);
				if (!g_active || g_active->kind != Kind::kList) {
					return;
				}
				auto& active = *g_active;

				if (active.firstFrame) {
					active.firstFrame = false;
					for (const auto& entry : active.entries) {
						const float w = IG::CalcTextSize(entry.c_str()).x;
						if (w > active.widest) {
							active.widest = w;
						}
					}
					// Text after "##" is an ImGui id and is not drawn, so it is
					// not measured either.
					active.titleWidth = IG::CalcTextSize(active.title.c_str(), nullptr, true).x;
					spdlog::info("MenuPrompt: list prompt drawing - {} entries, widest {} px, title {} px, render tid={}.",
						active.entries.size(), active.widest, active.titleWidth, Tid());
					LogInputState("list", "on open");
				}

				float screenW = 0.0f;
				float screenH = 0.0f;
				ScreenSize(screenW, screenH);
				// The player's last size if there is one, otherwise a default
				// wider than tall - sized to the entries alone, a short menu
				// came out as a narrow column 70% of the screen high. Either
				// way the width still grows when an entry or the title would
				// not fit, and that growth is not remembered as a preference.
				const float contentWidth = active.titleWidth > active.widest ? active.titleWidth : active.widest;
				float width = g_listWidth > 0.0f ? g_listWidth : screenW * 0.37f;
				const float height = g_listHeight > 0.0f ? g_listHeight : screenH * 0.42f;
				if (width < contentWidth + 80.0f) {
					width = contentWidth + 80.0f;
				}
				if (width > screenW * 0.9f) {
					width = screenW * 0.9f;
				}
				if (active.placedWidth == 0.0f) {
					active.placedWidth = width;
					active.placedHeight = height;
				}
				PlaceWindow(width, height, screenW, screenH);

				int  picked = -1;
				bool cancelled = false;

				// No close box in the title bar on purpose: the Cancel button
				// below is the one way out that is visible on a gamepad, and
				// two of them would be two things to keep in step.
				IG::Begin(active.title.c_str(), nullptr,
					IG::ImGuiWindowFlags_NoCollapse | IG::ImGuiWindowFlags_NoSavedSettings);

				if (IG::Button("Cancel")) {
					cancelled = true;
				}
				IG::Separator();

				IG::BeginChild("##LodestoneListBody", IG::ImVec2{ 0.0f, 0.0f }, IG::ImGuiChildFlags_Border, 0);
				auto* clipper = IG::ImGuiListClipperManager::Create();
				if (clipper) {
					IG::ImGuiListClipperManager::Begin(clipper, static_cast<int>(active.entries.size()), -1.0f);
					while (IG::ImGuiListClipperManager::Step(clipper)) {
						for (int i = clipper->DisplayStart; i < clipper->DisplayEnd; ++i) {
							const auto index = static_cast<std::size_t>(i);
							if (IG::Selectable(active.entries[index].c_str(), false)) {
								picked = i;
							}
							if (i == 0) {
								IG::SetItemDefaultFocus();
							}
						}
					}
					IG::ImGuiListClipperManager::End(clipper);
					IG::ImGuiListClipperManager::Destroy(clipper);
				}
				IG::EndChild();

				if (EscapePressed()) {
					cancelled = true;
				}
				// Read inside the window: GetWindowSize answers for the window
				// being drawn, and after End that is no longer this one.
				const auto size = IG::GetWindowSize();
				IG::End();

				if (picked >= 0 || cancelled) {
					LogInputState("list", "on close");
					// Remembered only when the player changed it. A window that
					// kept the size it was given - default, remembered, or
					// widened to fit - leaves the preference as it was.
					const float dw = size.x - active.placedWidth;
					const float dh = size.y - active.placedHeight;
					if (dw > 1.0f || dw < -1.0f || dh > 1.0f || dh < -1.0f) {
						g_listWidth = size.x;
						g_listHeight = size.y;
						spdlog::info("MenuPrompt: list prompt resized by the player to {} x {} px - kept for this session.",
							size.x, size.y);
					}
				}
				if (picked >= 0) {
					Result result;
					result.accepted = true;
					result.index = picked;
					result.text = active.entries[static_cast<std::size_t>(picked)];
					FinishLocked(std::move(result), "entry picked");
				} else if (cancelled) {
					FinishLocked(Result{}, "dismissed by the player");
				}
			} catch (...) {
				spdlog::error("MenuPrompt: exception in list render - swallowed.");
			}
		}

		void __stdcall RenderText()
		{
			try {
				std::lock_guard lock(g_lock);
				if (!g_active || g_active->kind != Kind::kText) {
					return;
				}
				auto& active = *g_active;

				const bool appearing = active.firstFrame;
				if (appearing) {
					active.firstFrame = false;
					spdlog::info("MenuPrompt: text prompt drawing - render tid={}.", Tid());
					LogInputState("text", "on open");
				}

				float screenW = 0.0f;
				float screenH = 0.0f;
				ScreenSize(screenW, screenH);
				PlaceWindow(screenW * 0.4f, 0.0f, screenW, screenH);

				bool accepted = false;
				bool cancelled = false;

				IG::Begin(active.title.c_str(), nullptr,
					IG::ImGuiWindowFlags_NoCollapse | IG::ImGuiWindowFlags_NoSavedSettings |
						IG::ImGuiWindowFlags_AlwaysAutoResize);

				// The box takes the keyboard the moment it appears, and the
				// suggestion comes up selected, so typing replaces it and
				// Enter alone accepts it unchanged.
				if (IG::IsWindowAppearing()) {
					IG::SetKeyboardFocusHere(0);
				}
				if (IG::InputText("##LodestoneTextInput", active.buffer.data(), active.buffer.size(),
						IG::ImGuiInputTextFlags_EnterReturnsTrue | IG::ImGuiInputTextFlags_AutoSelectAll)) {
					accepted = true;
				}

				if (IG::Button("OK")) {
					accepted = true;
				}
				IG::SameLine();
				if (IG::Button("Cancel")) {
					cancelled = true;
				}

				if (EscapePressed()) {
					cancelled = true;
				}
				IG::End();

				if (accepted || cancelled) {
					LogInputState("text", "on close");
				}
				if (accepted) {
					Result result;
					result.accepted = true;
					result.text = std::string(active.buffer.data());
					spdlog::info("MenuPrompt: text prompt accepted {} bytes.", result.text.size());
					FinishLocked(std::move(result), "text accepted");
				} else if (cancelled) {
					FinishLocked(Result{}, "dismissed by the player");
				}
			} catch (...) {
				spdlog::error("MenuPrompt: exception in text render - swallowed.");
			}
		}

		// Starts a prompt, or refuses. Returns false WITHOUT calling a_done.
		bool Begin(Kind a_kind, std::string a_title, std::vector<std::string> a_entries,
			const std::string& a_suggestion, Completion a_done)
		{
			if (!g_ready.load()) {
				return false;
			}

			std::lock_guard lock(g_lock);
			if (g_active) {
				spdlog::info("MenuPrompt: prompt refused - another one is open. One at a time, by design.");
				return false;
			}

			auto active = std::make_unique<Active>();
			active->kind = a_kind;
			active->title = a_title.empty() ? std::string("Lodestone") : std::move(a_title);
			active->entries = std::move(a_entries);
			active->done = std::move(a_done);
			if (a_kind == Kind::kText) {
				CopyTruncated(active->buffer, a_suggestion);
			}

			g_active = std::move(active);
			g_busy.store(true);

			auto* window = (a_kind == Kind::kList) ? g_listWindow : g_textWindow;
			if (!window) {
				g_active.reset();
				g_busy.store(false);
				spdlog::error("MenuPrompt: window missing for a prompt the module said it could show.");
				return false;
			}
			window->IsOpen = true;
			return true;
		}

		// -------------------------------------------------------------------
		// THE PAPYRUS SURFACE - the first latent natives in this plugin
		//
		// A prompt has to BLOCK the script that asked for it. The surface this
		// replaces blocks, and a consumer's loop that reopens a list until the
		// player is done only works that way. A latent native is how the VM
		// does it: the calling stack is parked, the VM keeps running, and the
		// answer is handed back later against a stack id.
		//
		// TWO RULES BELOW ARE NOT PREFERENCES. Both exist because the engine's
		// half of this is not in CommonLibSSE: NativeFunctionBase::Call and
		// IVirtualMachine::ReturnFromLatent are relocations into the game
		// binary, so neither question can be settled by reading a header.
		//
		//   1. THE CALLBACK ALWAYS RETURNS kStarted, NEVER kFailed. The
		//      documented effect of kFailed is "return NONE and log error", and
		//      a NONE handed to an Int is 0 - which is a perfectly good list
		//      index. A refusal arriving at the script as "the player picked
		//      the first entry" is the worst answer this module could give, so
		//      the path that can produce it is never taken.
		//
		//   2. NO RESULT IS RETURNED FROM INSIDE THE CALLBACK, not even an
		//      immediate refusal. Whether the VM has finished parking the stack
		//      by the time the callback returns is not knowable from here, so
		//      every answer goes back through the game thread instead. The one
		//      published plugin found doing this (MCM Unlocked, read 2026-09-15)
		//      returns from a queued task as well, and the header's own guidance
		//      is that the callback returns as soon as possible and does no work
		//      of its own.
		//
		// A SCRIPT PARKED HERE IS RELEASED BY WHATEVER ENDS THE PROMPT - a
		// pick, a cancel, a refusal, or a load. There is no path that starts a
		// prompt and never answers, which matters more than usual: an answer
		// that never arrives leaves that script's stack parked with nothing
		// left to wake it.
		// -------------------------------------------------------------------

		constexpr std::int32_t kCancelled = -1;
		constexpr std::int32_t kRefused = -2;

		// Accepted text waits here until the script takes it by ticket.
		//
		// WHY A TICKET RATHER THAN THE STRING ITSELF: an accepted EMPTY line is
		// an answer, and a String return cannot tell it apart from a
		// cancellation - which is exactly the ambiguity the surface this
		// replaces forces on its consumers. So the latent call answers with a
		// number, where the three outcomes are distinct, and the text is
		// fetched with that number.
		//
		// AND THE TICKET IS WHAT MAKES IT RACE-FREE. A bare "give me the last
		// text" would be read after the waiting script resumes, and another
		// script may have opened and finished its own prompt in between.
		struct TextResult
		{
			std::int32_t ticket{ 0 };
			std::string  text;
		};

		// Small on purpose. A ticket that is never taken is a consumer bug, and
		// the cost of that bug should be bounded rather than unbounded.
		constexpr std::size_t kTextResultsKept = 4;

		std::mutex             g_textLock;
		std::deque<TextResult> g_textResults;

		// Starts at 1, so that zero is never a ticket a consumer can hold.
		std::int32_t g_nextTicket{ 1 };

		std::int32_t StoreText(std::string a_text)
		{
			std::lock_guard lock(g_textLock);

			const std::int32_t ticket = g_nextTicket++;
			if (g_nextTicket < 1) {
				g_nextTicket = 1;
			}

			g_textResults.push_back(TextResult{ ticket, std::move(a_text) });
			while (g_textResults.size() > kTextResultsKept) {
				spdlog::info("MenuPrompt: text of ticket {} was never taken - dropped.", g_textResults.front().ticket);
				g_textResults.pop_front();
			}

			return ticket;
		}

		bool TakeText(std::int32_t a_ticket, std::string& a_out)
		{
			std::lock_guard lock(g_textLock);
			for (auto it = g_textResults.begin(); it != g_textResults.end(); ++it) {
				if (it->ticket == a_ticket) {
					a_out = std::move(it->text);
					g_textResults.erase(it);
					return true;
				}
			}
			return false;
		}

		std::string ToStd(const RE::BSFixedString& a_string)
		{
			const char* text = a_string.c_str();
			return text ? std::string(text) : std::string();
		}

		// Wakes the parked script. MUST be called on the game thread - every
		// caller here either is a completion, which is already dispatched
		// there, or wraps this in DispatchToGame.
		void ReturnToScript(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, std::int32_t a_value)
		{
			if (!a_vm) {
				spdlog::error("MenuPrompt: no VM to answer stack {} - that script stays parked.", a_stackID);
				return;
			}

			try {
				a_vm->ReturnLatentResult(a_stackID, a_value);
			} catch (...) {
				spdlog::error("MenuPrompt: returning {} to stack {} threw - that script stays parked.",
					a_value, a_stackID);
			}
		}

		bool MenuPromptAvailable(RE::StaticFunctionTag*)
		{
			return Available();
		}

		bool MenuPromptBusy(RE::StaticFunctionTag*)
		{
			return IsBusy();
		}

		std::string MenuPromptTakeText(RE::StaticFunctionTag*, std::int32_t a_ticket)
		{
			std::string text;
			if (!TakeText(a_ticket, text)) {
				spdlog::info("MenuPrompt: nothing held for ticket {} - already taken, or that prompt did not end "
							 "in an answer.",
					a_ticket);
				return {};
			}
			return text;
		}

		RE::BSScript::LatentStatus MenuPromptList(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
			RE::StaticFunctionTag*, RE::BSFixedString a_title, std::vector<RE::BSFixedString> a_entries)
		{
			bool started = false;

			try {
				// Copied here, on the VM's thread, because the render callback
				// is not on it. The vector the binding hands over is already a
				// copy of the script's array, but its strings are the VM's.
				std::vector<std::string> entries;
				entries.reserve(a_entries.size());
				for (const auto& entry : a_entries) {
					entries.emplace_back(ToStd(entry));
				}

				started = BeginList(ToStd(a_title), std::move(entries), [a_vm, a_stackID](Result a_result) {
					ReturnToScript(a_vm, a_stackID,
						a_result.accepted ? static_cast<std::int32_t>(a_result.index) : kCancelled);
				});
			} catch (...) {
				spdlog::error("MenuPrompt: exception starting a list prompt - refused.");
				started = false;
			}

			if (!started) {
				DispatchToGame([a_vm, a_stackID]() { ReturnToScript(a_vm, a_stackID, kRefused); });
			}

			return RE::BSScript::LatentStatus::kStarted;
		}

		RE::BSScript::LatentStatus MenuPromptText(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
			RE::StaticFunctionTag*, RE::BSFixedString a_title, RE::BSFixedString a_suggestion)
		{
			bool started = false;

			try {
				started = BeginText(ToStd(a_title), ToStd(a_suggestion), [a_vm, a_stackID](Result a_result) {
					std::int32_t answer = kCancelled;
					if (a_result.accepted) {
						answer = StoreText(std::move(a_result.text));
					}
					ReturnToScript(a_vm, a_stackID, answer);
				});
			} catch (...) {
				spdlog::error("MenuPrompt: exception starting a text prompt - refused.");
				started = false;
			}

			if (!started) {
				DispatchToGame([a_vm, a_stackID]() { ReturnToScript(a_vm, a_stackID, kRefused); });
			}

			return RE::BSScript::LatentStatus::kStarted;
		}

		// ---------------------------------------------------------------------
		// THE LIBRARY'S LATENT BINDING DOES NOT COMPILE, SO THIS IS OUR OWN
		// ---------------------------------------------------------------------
		//
		// IVirtualMachine::RegisterLatentFunction instantiates a class whose
		// constructor writes
		//
		//     this->_retType = GetRawType<latentR>();
		//
		// GetRawType is a function OBJECT, so that line builds a temporary of
		// it instead of calling it, and no assignment to TypeInfo accepts one.
		// It fails for every result type (NativeLatentFunction.h:31 of the
		// pinned submodule). The non-latent binding right beside it has the
		// call written correctly - NativeFunction.h:71, GetRawType<...>{}() -
		// and the same broken line is upstream in both CommonLib forks as of
		// this phase, unchanged since 2023. These are the first latent natives
		// this plugin registers, which is why it had not come up here.
		//
		// The submodule is not patched: a local edit there does not travel
		// with a clone. Instead this class does what the library's class was
		// meant to - derive from the same NativeFunction base it derives from,
		// with the callback returning LatentStatus, then set the two fields
		// that make it latent. Everything else, the argument marshalling
		// included, is the library's own code unchanged.
		//
		// If the library is fixed, this class can go and the two registrations
		// below become RegisterLatentFunction<std::int32_t> again.
		template <class... Args>
		class LatentNative final :
			public RE::BSScript::NativeFunction<
				true,
				RE::BSScript::LatentStatus(RE::BSScript::Internal::VirtualMachine*, RE::VMStackID, RE::StaticFunctionTag*, Args...),
				std::underlying_type_t<RE::BSScript::LatentStatus>,
				RE::StaticFunctionTag*,
				Args...>
		{
			using super = RE::BSScript::NativeFunction<
				true,
				RE::BSScript::LatentStatus(RE::BSScript::Internal::VirtualMachine*, RE::VMStackID, RE::StaticFunctionTag*, Args...),
				std::underlying_type_t<RE::BSScript::LatentStatus>,
				RE::StaticFunctionTag*,
				Args...>;

		public:
			LatentNative(std::string_view a_fnName, std::string_view a_className, typename super::function_type* a_callback) :
				super(a_fnName, a_className, a_callback)
			{
				// What the SCRIPT receives, which is not what the callback
				// returns. It has to match the type later handed to
				// ReturnLatentResult exactly - an Int here, std::int32_t there.
				this->_retType = RE::BSScript::GetRawType<std::int32_t>{}();
				this->_isLatent = true;
			}

			~LatentNative() override = default;
		};
	}

	bool Available()
	{
		return g_ready.load();
	}

	bool IsBusy()
	{
		return g_busy.load();
	}

	bool BeginList(std::string a_title, std::vector<std::string> a_entries, Completion a_done)
	{
		if (a_entries.empty()) {
			spdlog::info("MenuPrompt: list prompt refused - no entries to show.");
			return false;
		}
		return Begin(Kind::kList, std::move(a_title), std::move(a_entries), std::string{}, std::move(a_done));
	}

	bool BeginText(std::string a_title, std::string a_suggestion, Completion a_done)
	{
		return Begin(Kind::kText, std::move(a_title), {}, a_suggestion, std::move(a_done));
	}

	void Install()
	{
		try {
			// IsInstalled() looks at the file on disk, which is not the same
			// question as "did the DLL load" - a disabled or failed plugin
			// leaves the file exactly where it was. Both are checked.
			const bool onDisk = SMF::IsInstalled();
			const bool loaded = ::GetModuleHandleW(L"SKSEMenuFramework") != nullptr;
			if (!onDisk || !loaded) {
				spdlog::info("MenuPrompt: SKSE Menu Framework not present (on disk={}, loaded={}) - "
							 "module inactive, natives return their sentinels.",
					onDisk, loaded);
				return;
			}

			g_listWindow = SMF::AddWindow(&RenderList, true);
			g_textWindow = SMF::AddWindow(&RenderText, true);
			if (!g_listWindow || !g_textWindow) {
				spdlog::error("MenuPrompt: the framework did not return a window (list={}, text={}) - "
							  "module inactive.",
					g_listWindow != nullptr, g_textWindow != nullptr);
				g_listWindow = nullptr;
				g_textWindow = nullptr;
				return;
			}

			g_ready.store(true);

			// The number below is what the framework's own version function
			// returns. It is NOT the mod's version: it answered 3.7 on an
			// installation running 3.13. Logged because it is the only
			// version signal the client side has, and labelled because
			// comparing it against the Nexus version would be wrong.
			spdlog::info("MenuPrompt: active - two windows registered, game pauses while a prompt is open. "
						 "Framework version function returns {} (not the mod version).",
				SMF::GetMenuFrameworkVersion());
		} catch (...) {
			spdlog::error("MenuPrompt: exception during install - module inactive.");
			g_ready.store(false);
		}
	}

	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}

		// A save being loaded and a new game starting are the two moments the
		// world under an open prompt goes away. A menu watch would not cover
		// them: the prompt is not a game menu, and nothing else clears it.
		const bool loading = a_msg->type == SKSE::MessagingInterface::kPreLoadGame;
		const bool newGame = a_msg->type == SKSE::MessagingInterface::kNewGame;
		if (!loading && !newGame) {
			return;
		}

		std::lock_guard lock(g_lock);
		if (!g_active) {
			return;
		}
		FinishLocked(Result{}, loading ? "a save is being loaded" : "a new game is starting");
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("MenuPrompt: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("MenuPromptAvailable", "Lodestone", MenuPromptAvailable);
		a_vm->RegisterFunction("MenuPromptBusy", "Lodestone", MenuPromptBusy);
		a_vm->RegisterFunction("MenuPromptTakeText", "Lodestone", MenuPromptTakeText);

		// The two that park the caller, bound through LatentNative rather than
		// RegisterLatentFunction - see the class for why. The template
		// arguments are the Papyrus parameters, in order. Not callable from
		// tasklets, which is the library's own default.
		a_vm->BindNativeMethod(new LatentNative<RE::BSFixedString, std::vector<RE::BSFixedString>>(
			"MenuPromptList", "Lodestone", MenuPromptList));
		a_vm->BindNativeMethod(new LatentNative<RE::BSFixedString, RE::BSFixedString>(
			"MenuPromptText", "Lodestone", MenuPromptText));

		spdlog::info("MenuPrompt: natives registered (MenuPromptAvailable, MenuPromptBusy, MenuPromptList, "
					 "MenuPromptText, MenuPromptTakeText). The two prompts are latent - a caller waits.");
		return true;
	}
}
