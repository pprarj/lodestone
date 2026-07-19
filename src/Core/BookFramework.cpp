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

#include <safetyhook.hpp>

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

		// debug: per-open lines are absent from a Release build, since Log.cpp
		// caps the level at info when NDEBUG is defined.
		bool ShouldLogOpens()
		{
			auto* logger = spdlog::default_logger_raw();
			return logger && logger->should_log(spdlog::level::debug);
		}

		// The inline hook object owns the original; call it through this.
		SafetyHookInline g_openBookHook{};

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
		//
		// VALIDATED IN GAME. Two things were unproven here, not one: the address
		// did not come from the shipped headers, and with eight arguments the
		// signature was itself a claim. Both were settled by a log-only pass:
		//
		//   OPEN '2920, First Seed, v3'   (0x0001ACE5) ref=0x00000000 10189 chars
		//                                 scale=0.44 defaultPos=false
		//   OPEN 'The Seed'               (0x0001ACFA) ref=0x00000000  6961 chars
		//                                 scale=0.59 defaultPos=false
		//   OPEN 'The Cabin in the Woods' (0x000EF638) ref=0x0300910A  3682 chars
		//                                 scale=1.00 defaultPos=true
		//
		// The description argument held real book markup ("[pagebreak]<p
		// align=\"center\">...") for every one, which is the first argument
		// confirming itself. The strongest evidence is the third: `ref` came back
		// null for both books opened from the inventory and populated for the one
		// opened from a world reference - so an argument in the middle of the list
		// behaves the way the header says it should. That is the whole signature
		// being confirmed, not just its first slot.
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

				g_openBookHook.call<void, const RE::BSString&, const RE::ExtraDataList*, RE::TESObjectREFR*,
					RE::TESObjectBOOK*, const RE::NiPoint3&, const RE::NiMatrix3&, float, bool>(
					*desc, a_extraList, a_ref, a_book, a_pos, a_rot, a_scale, a_useDefaultPos);
			}
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

			// The guard that used to stand here is gone, along with the branch hook
			// it protected. It refused to install because the first byte here is
			// 0x40 - a REX prefix, an ordinary function prologue - which is what
			// proved in game that this address is a function body and not a call
			// site. Trampoline::write_branch cannot detour a body.
			//
			// BookMenu::OpenBookMenu is a static function, so there is no vtable
			// slot to swap either. An inline hook is the right instrument:
			// SafetyHook relocates the displaced prologue and suspends other
			// threads while patching.
			//
			// The address is still UNPROVEN - it came from an outside source, not
			// from the shipped headers - which is why the thunk only logs for now.
			g_openBookHook = safetyhook::create_inline(
				reinterpret_cast<void*>(target.address()),
				reinterpret_cast<void*>(&OpenBookMenuHook::thunk));

			if (!g_openBookHook) {
				spdlog::error("BookFramework: SafetyHook refused to hook the book-open target - "
							  "book text stays vanilla.");
				return;
			}

			spdlog::info("BookFramework: hook installed on the book-open function. "
						 "No text stored yet - passthrough until a consumer calls Lodestone.SetBookText.");
		} catch (const std::exception& e) {
			spdlog::error("BookFramework: failed to install: {} - book text will not be modified.", e.what());
		} catch (...) {
			spdlog::error("BookFramework: failed to install (unknown exception) - book text will not be modified.");
		}
	}
}
