// BookFramework.h
// Lodestone - Shared SKSE framework
//
// Module: BookFramework (Core) - TRACE ONLY (Stage C.2 / L1, Part 1.5)
//
// This is the LOG-ONLY trace build that precedes Lodestone's own book-text
// module - the capability to let a consumer supply dynamic, runtime-built text
// for a book (a journal that grows as the player plays, notes assembled at
// runtime, and so on). It is NOT the production module. It changes NOTHING in
// game: it does not hook the text path, registers no natives, and only OBSERVES
// book-menu opens and logs, so that the production design (Part 2) rests on
// measured fact rather than inference - the same discipline the CastTime module
// was built with.
//
// THE ENGINE FACTS (Part 1, read from the installed CommonLibSSE-NG 3.5.3 headers):
//   - A book's static text lives in its record: TESObjectBOOK inherits
//     TESDescription (RE/T/TESObjectBOOK.h), read via TESDescription::GetDescription.
//   - The Book Menu is handed the page text as a RAW BSString argument:
//     BookMenu::OpenBookMenu(const BSString& a_description, ..., TESObjectBOOK*, ...)
//     (RE/B/BookMenu.h). A raw string can carry arbitrary runtime text that the
//     record's localized-string field cannot hold.
//
// THE DESIGN QUESTION THIS ANSWERS:
//   For a book whose displayed text is built at runtime, is that text stored back
//   into the RECORD before it opens, or is it SUBSTITUTED at display time (record
//   left as a placeholder)? The answer decides the production shape:
//     - record carries the text  => a native that writes the record is enough,
//       no engine hook on the display path.
//     - record is a placeholder, text substituted at open => the production module
//       must hook the display path (OpenBookMenu) to inject the runtime string.
//   Given OpenBookMenu takes a raw BSString - exactly what arbitrary runtime text
//   needs, and more than the localized-string record can express - the display
//   path is the likely answer, but this trace confirms it on THIS build instead of
//   assuming.
//
// WHY THIS TRACE IS A PASSIVE SINK, NOT THAT HOOK:
//   OpenBookMenu is a static engine function, and CommonLibSSE-NG 3.5.3 exposes NO
//   address-library ID for it in the shipped headers (only RTTI/VTABLE for
//   BookMenu). Hooking it needs an ID resolved from the address library (a Part 2
//   task) or a fragile prologue detour - neither is zero-risk, and a trace must be.
//   So this trace registers a passive MenuOpenCloseEvent sink on RE::UI (fully
//   addressable, no trampoline) and, when the "Book Menu" opens, reads the target
//   book through BookMenu::GetTargetForm / GetTargetReference and logs its RECORD
//   description via TESDescription.
//
// HOW TO READ THE RESULT:
//   Open a book whose text is supplied dynamically in the load order and compare
//   the log against the screen. If this logs a SHORT or empty record description
//   while the screen shows the full dynamic page, the text is substituted at
//   display and the production module needs the display hook, not a record write.
//   The trace also captures the exact open flow the Part 2 hook depends on: that
//   the menu event fires, that GetTargetForm resolves the book, and whether the
//   book was opened from inventory or from a world reference. Run in TWO
//   independent load orders (one heavy, one clean vanilla for the baseline),
//   exactly as the CastTime trace was.
//
// Phase L1 - Stage C.2 / Part 1.5 (TRACE)

#pragma once

namespace Lodestone::Core::BookFramework
{
	// Registers the passive Book Menu trace sink on RE::UI. Must be called once.
	//
	// Call site: plugin.cpp, on SKSE::MessagingInterface::kDataLoaded (the UI
	// singleton exists by then). Never throws - failures are logged and swallowed,
	// and simply leave no trace sink installed.
	void Install();
}
