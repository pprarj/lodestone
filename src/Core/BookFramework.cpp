// BookFramework.cpp
// Lodestone - Shared SKSE framework
//
// TRACE-ONLY implementation for Stage C.2 / L1, Part 1.5. Observes Book Menu
// opens and logs; changes nothing. See BookFramework.h for the rationale (the
// record-vs-display design question, why this trace is a passive event sink rather
// than the eventual OpenBookMenu hook, and how to read the result).
//
// Phase L1 - Stage C.2 / Part 1.5 (TRACE)

#include "BookFramework.h"

#include <string>

namespace Lodestone::Core::BookFramework
{
	namespace
	{
		// One-line, ASCII-safe preview of arbitrary game text for the log. Book
		// bodies carry newlines, formatting tags ([pagebreak], <p>, <font ...>) and
		// possibly non-ASCII bytes; none of that belongs in a log line. Any byte
		// outside printable ASCII becomes '.', and the preview is capped. The raw
		// length is logged separately so a truncated preview is never mistaken for
		// the whole text.
		std::string PreviewAscii(const char* a_text, std::size_t a_max = 200)
		{
			if (!a_text) {
				return "<null>";
			}

			std::string out;
			out.reserve(a_max);
			std::size_t i = 0;
			for (; a_text[i] != '\0' && i < a_max; ++i) {
				const unsigned char c = static_cast<unsigned char>(a_text[i]);
				out.push_back((c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '.');
			}
			if (a_text[i] != '\0') {
				out += "...";
			}
			return out;
		}

		// Reads a book's RECORD body text (the inherited TESDescription, DESC field)
		// into an ASCII preview, and reports its raw length through a_rawLen. This
		// is the same call the engine makes to build the page text, so it is safe to
		// repeat here at the low frequency of "a book was opened".
		std::string ReadBookDescription(RE::TESObjectBOOK* a_book, std::size_t& a_rawLen)
		{
			a_rawLen = 0;
			if (!a_book) {
				return "<no book>";
			}

			RE::BSString desc;
			// static_cast selects the inherited TESDescription base subobject (the
			// book body, DESC) rather than the itemCardDescription member (CNAM).
			static_cast<RE::TESDescription*>(a_book)->GetDescription(desc, a_book);

			const char* raw = desc.c_str();
			a_rawLen = raw ? std::string_view(raw).size() : 0;
			return PreviewAscii(raw);
		}

		void LogBookOpen()
		{
			auto* book = RE::BookMenu::GetTargetForm();
			if (!book) {
				spdlog::info("BookTrace: Book Menu opened but GetTargetForm() is null - nothing to read.");
				return;
			}

			// Null reference == opened from inventory (per BookMenu.h). Non-null ==
			// a placed reference activated in the world.
			auto* ref = RE::BookMenu::GetTargetReference();

			const char* name = book->GetName();

			std::size_t rawLen = 0;
			const std::string preview = ReadBookDescription(book, rawLen);

			// A short/empty record body while the on-screen page is full means the
			// dynamic text is substituted at display time, not stored in the record.
			// Flag it so the log reads at a glance.
			const char* const recordHint = (rawLen == 0) ? " [record EMPTY - display-substituted?]" : "";

			spdlog::info("BookTrace: OPEN book '{}' (0x{:08X}) | source={} | note={} tome={} teachesSkill={} teachesSpell={} | descLen={}{} desc=\"{}\"",
				(name && *name) ? name : "<unnamed>",
				book->GetFormID(),
				ref ? "world" : "inventory",
				book->IsNoteScroll() ? "yes" : "no",
				book->IsBookTome() ? "yes" : "no",
				book->TeachesSkill() ? "yes" : "no",
				book->TeachesSpell() ? "yes" : "no",
				rawLen,
				recordHint,
				preview);

			if (ref) {
				const char* refName = ref->GetName();
				spdlog::info("BookTrace:   world ref 0x{:08X} '{}'",
					ref->GetFormID(),
					(refName && *refName) ? refName : "<unnamed>");
			}
		}

		// Passive observer of menu open/close. It does NOT intercept the text path:
		// the engine dispatches the event to us AFTER it has decided to open the
		// menu, so this cannot alter what is displayed. That is exactly the property
		// a trace needs. kContinue always, so every other sink still runs unchanged.
		class BookMenuTraceSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static BookMenuTraceSink* GetSingleton()
			{
				static BookMenuTraceSink singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->opening && a_event->menuName == RE::BookMenu::MENU_NAME) {
					try {
						LogBookOpen();
					} catch (...) {
						// A trace must never take the game down. Swallow and move on.
						spdlog::warn("BookTrace: exception while logging a book open - ignored.");
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			BookMenuTraceSink() = default;
			BookMenuTraceSink(const BookMenuTraceSink&) = delete;
			BookMenuTraceSink(BookMenuTraceSink&&) = delete;
			BookMenuTraceSink& operator=(const BookMenuTraceSink&) = delete;
			BookMenuTraceSink& operator=(BookMenuTraceSink&&) = delete;
		};
	}

	void Install()
	{
		try {
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				spdlog::error("BookTrace: no UI singleton - book trace not installed.");
				return;
			}

			ui->AddEventSink<RE::MenuOpenCloseEvent>(BookMenuTraceSink::GetSingleton());
			spdlog::info("BookTrace: Book Menu trace sink installed (TRACE ONLY - observes book opens, changes nothing).");
		} catch (const std::exception& e) {
			spdlog::error("BookTrace: failed to install: {} - book trace unavailable.", e.what());
		} catch (...) {
			spdlog::error("BookTrace: failed to install (unknown exception) - book trace unavailable.");
		}
	}
}
