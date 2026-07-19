// BookFramework.cpp
// Lodestone - Shared SKSE framework
//
// Production implementation of the dynamic book-text capability (Stage C.2). See
// BookFramework.h for the contract, the design rationale, and the persistence and
// validation notes.
//
// This SUPERSEDES the Part 1.5 trace (a passive MenuOpenCloseEvent sink that only
// logged book opens). That trace answered where dynamic book text has to be
// injected - the display path, because the game's book-open function takes the
// page text as a raw BSString the record cannot hold. Its implementation is in git
// history. Production hooks that function directly.
//
// EXCEPTION SAFETY: a C++ exception escaping the hook into the engine is undefined
// behavior. The thunk wraps its body and always calls the original exactly once.
//
// Phase L1 - Stage C.2

#include "BookFramework.h"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Lodestone::Core::BookFramework
{
	namespace
	{
		// -------------------------------------------------------------------
		// The text store - book FormID -> runtime text
		//
		// Written by the natives (on a Papyrus VM thread), read by the hook (on the
		// game thread), so it is guarded by a mutex. Book opens are infrequent
		// (nothing like the per-cast volume CastTime sees), so a lock on the open
		// path costs nothing that matters.
		//
		// Empty store / no entry for a book => passthrough, vanilla text. That is
		// the same silent degradation CastTime has: "no text set" reads like
		// "plugin not installed".
		//
		// Session-scoped: not serialized to the save. The consumer re-establishes a
		// book's text after load (see BookFramework.h - PERSISTENCE).
		// -------------------------------------------------------------------
		std::unordered_map<RE::FormID, std::string> g_bookText;
		std::mutex                                  g_bookTextLock;

		// Copies a book's stored text out under the lock. Returns false when the
		// book has none (the caller then leaves the vanilla text alone).
		bool LookupText(RE::FormID a_book, std::string& a_out)
		{
			std::lock_guard<std::mutex> lock(g_bookTextLock);
			auto it = g_bookText.find(a_book);
			if (it == g_bookText.end()) {
				return false;
			}
			a_out = it->second;
			return true;
		}

		bool ShouldLogOpens()
		{
			auto* logger = spdlog::default_logger_raw();
			return logger && logger->should_log(spdlog::level::debug);
		}

		// -------------------------------------------------------------------
		// Hook: the game's book-open function (behind BookMenu::OpenBookMenu)
		//
		// The page text arrives as the first argument, a raw BSString. If the book
		// has stored text, we hand the original the stored string instead; otherwise
		// we pass the argument through unchanged. The original is called exactly
		// once on every path, so a book with no stored text - or any failure here -
		// opens exactly as vanilla would.
		//
		// The signature mirrors BookMenu::OpenBookMenu (RE/B/BookMenu.h). BSString
		// caps at 64 KB (its size field is 16-bit); that is the engine's own limit
		// on book text, not one this module adds.
		// -------------------------------------------------------------------
		struct OpenBookMenuHook
		{
			static void thunk(const RE::BSString& a_description, const RE::ExtraDataList* a_extraList,
				RE::TESObjectREFR* a_ref, RE::TESObjectBOOK* a_book, const RE::NiPoint3& a_pos,
				const RE::NiMatrix3& a_rot, float a_scale, bool a_useDefaultPos)
			{
				const RE::BSString* desc = &a_description;
				RE::BSString        replacement;

				try {
					if (a_book) {
						std::string text;
						if (LookupText(a_book->GetFormID(), text)) {
							replacement = RE::BSString{ std::string_view{ text } };
							desc = &replacement;

							if (ShouldLogOpens()) {
								const char* name = a_book->GetName();
								spdlog::debug("BookFramework: serving stored text for '{}' (0x{:08X}), {} chars.",
									(name && *name) ? name : "<unnamed>",
									a_book->GetFormID(),
									text.size());
							}
						}
					}
				} catch (...) {
					// Never take the game down over book text. Fall back to vanilla.
					desc = &a_description;
				}

				func(*desc, a_extraList, a_ref, a_book, a_pos, a_rot, a_scale, a_useDefaultPos);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		// -------------------------------------------------------------------
		// Natives
		//
		// Error convention (Stage B.3): failure by return value, never by throwing.
		// A null Book yields false (Bool) or "" (String).
		// -------------------------------------------------------------------

		// Lodestone.SetBookText(Book, String) -> Bool
		// Replaces the stored text for a book. Shown next time the book opens.
		bool SetBookText(RE::StaticFunctionTag*, RE::TESObjectBOOK* a_book, RE::BSFixedString a_text)
		{
			if (!a_book) {
				spdlog::warn("BookFramework: SetBookText got a None book - ignored.");
				return false;
			}

			std::lock_guard<std::mutex> lock(g_bookTextLock);
			g_bookText[a_book->GetFormID()] = a_text.c_str() ? a_text.c_str() : "";
			return true;
		}

		// Lodestone.AppendBookText(Book, String) -> Bool
		// Appends to the stored text for a book, starting a new entry if none yet.
		bool AppendBookText(RE::StaticFunctionTag*, RE::TESObjectBOOK* a_book, RE::BSFixedString a_text)
		{
			if (!a_book) {
				spdlog::warn("BookFramework: AppendBookText got a None book - ignored.");
				return false;
			}

			if (a_text.c_str()) {
				std::lock_guard<std::mutex> lock(g_bookTextLock);
				g_bookText[a_book->GetFormID()] += a_text.c_str();
			}
			return true;
		}

		// Lodestone.ClearBookText(Book) -> Bool
		// Drops the stored text for a book; it reverts to its record text.
		bool ClearBookText(RE::StaticFunctionTag*, RE::TESObjectBOOK* a_book)
		{
			if (!a_book) {
				spdlog::warn("BookFramework: ClearBookText got a None book - ignored.");
				return false;
			}

			std::lock_guard<std::mutex> lock(g_bookTextLock);
			g_bookText.erase(a_book->GetFormID());
			return true;
		}

		// Lodestone.GetBookText(Book) -> String
		// Reads back the stored text, or "" if the book has none. Session-scoped -
		// after a load, a book reads back "" until the consumer re-sets it.
		RE::BSFixedString GetBookText(RE::StaticFunctionTag*, RE::TESObjectBOOK* a_book)
		{
			if (!a_book) {
				return RE::BSFixedString("");
			}

			std::lock_guard<std::mutex> lock(g_bookTextLock);
			auto it = g_bookText.find(a_book->GetFormID());
			return RE::BSFixedString(it != g_bookText.end() ? it->second.c_str() : "");
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("BookFramework: null VM, cannot register natives.");
			return false;
		}

		// Registered on the "Lodestone" script, so consumers call them as
		// Lodestone.SetBookText(book, text), etc.
		a_vm->RegisterFunction("SetBookText", "Lodestone", SetBookText);
		a_vm->RegisterFunction("AppendBookText", "Lodestone", AppendBookText);
		a_vm->RegisterFunction("ClearBookText", "Lodestone", ClearBookText);
		a_vm->RegisterFunction("GetBookText", "Lodestone", GetBookText);

		spdlog::info("BookFramework: natives registered (SetBookText, AppendBookText, ClearBookText, GetBookText).");
		return true;
	}

	void Install()
	{
		try {
			// The game function behind BookMenu::OpenBookMenu. The ID is not in the
			// shipped 3.5.3 headers; it is taken from the CommonLibSSE-NG source and
			// is pending in-game confirmation (see BookFramework.h).
			REL::Relocation<std::uintptr_t> target{ REL::RelocationID(50122, 51053) };

			// GUARD - added after the L3 trace crashed the game doing exactly what
			// the line below does.
			//
			// Trampoline::write_branch does NOT detour a function body. It assumes
			// the address already holds a 5-byte rel32 branch, decodes that
			// displacement, and returns the branch's original target:
			//
			//     const auto disp   = (std::int32_t*)(a_src + N - 4);
			//     const auto func   = (a_src + N) + *disp;
			//
			// Given a function PROLOGUE it reads four prologue bytes as a
			// displacement and hands back an address in no loaded module - which
			// the thunk then calls. That is an instant access violation, and it is
			// silent until the hooked path first runs.
			//
			// This address is a function ID, so it almost certainly points at a
			// prologue rather than a call site. Rather than delete a hook that has
			// never been proven either way, the check refuses to write unless the
			// target really is a rel32 branch, and says so in the log. Book text
			// then degrades to vanilla instead of crashing the game.
			const auto opcode = *reinterpret_cast<const std::uint8_t*>(target.address());
			if (opcode != 0xE8 && opcode != 0xE9) {
				spdlog::error("BookFramework: NOT installing the book-open hook - the target is not a rel32 branch "
							  "(first byte 0x{:02X}, expected 0xE8 call or 0xE9 jmp). write_branch only redirects an "
							  "existing call site; pointed at a function body it returns a garbage original and "
							  "crashes on first use. Book text stays vanilla until a real call site is identified.",
					opcode);
				return;
			}

			auto& trampoline = SKSE::GetTrampoline();
			OpenBookMenuHook::func =
				trampoline.write_branch<5>(target.address(), OpenBookMenuHook::thunk);

			spdlog::info("BookFramework: hook installed on the book-open function. "
						 "No text stored yet - passthrough until a consumer calls Lodestone.SetBookText.");
		} catch (const std::exception& e) {
			spdlog::error("BookFramework: failed to install: {} - book text will not be modified.", e.what());
		} catch (...) {
			spdlog::error("BookFramework: failed to install (unknown exception) - book text will not be modified.");
		}
	}
}
