// BookFramework.h
// Lodestone - Shared SKSE framework
//
// Module: BookFramework (Core)
// Owns the dynamic book-text capability (Stage C.2): a consumer can supply
// runtime-built text for a book (a journal that grows as the player plays, notes
// assembled at runtime, and so on), and the plugin shows that text when the book
// is opened.
//
// CORE, agnostic: the module knows no consumer by name. The consumer identifies a
// book by its own Book form and hands the plugin the text; the plugin keys the
// text by that book's FormID and serves it at display time.
//
// PUBLIC API (declared in Lodestone.psc, registered on the "Lodestone" script):
//
//   Bool   Function SetBookText(Book akBook, String asText) global native
//   Bool   Function AppendBookText(Book akBook, String asText) global native
//   Bool   Function ClearBookText(Book akBook) global native
//   String Function GetBookText(Book akBook) global native
//
//   Book maps to RE::TESObjectBOOK* (a form pointer parameter, verified the same
//   way CastTime's GlobalVariable was: RE/P/PackUnpack.h registers a form pointer
//   under its FORMTYPE and unpacks it back to the same pointer type). SetBookText
//   replaces the stored text, AppendBookText adds to it, ClearBookText drops it
//   (the book reverts to its record text), GetBookText reads it back.
//
// HOW IT WORKS (the design question the Part 1.5 trace was built to settle):
//   A book's page text reaches the engine's Book Menu as a RAW BSString argument
//   to the game's book-open function (RE/B/BookMenu.h). The record's own text is a
//   localized-string id that cannot hold arbitrary runtime text, so dynamic text
//   must be injected on the DISPLAY path, not written into the record. This module
//   therefore hooks the game's book-open function: when a book with stored text is
//   opened, it substitutes the stored string for the argument and calls the
//   original; when a book has no stored text, it passes through untouched (vanilla
//   text). That passthrough is the same silent-degradation property CastTime has:
//   "no text set" is indistinguishable from "plugin not installed".
//
//   NOTE (pending in-game validation): the hook target is the game function behind
//   BookMenu::OpenBookMenu. CommonLibSSE-NG 3.5.3 exposes no address-library ID for
//   it in the shipped headers, so the ID is taken from the CommonLibSSE-NG source
//   (RELOCATION_ID(50122, 51053)) and must be confirmed in game before this is
//   trusted. See BookFramework.cpp.
//
// PERSISTENCE (v1): the stored text lives in memory for the session only - it is
//   NOT serialized into the save. A consumer re-establishes a book's text after
//   each load (SetBookText from its own Papyrus state), the same runtime-registration
//   model CastTime uses. Co-save serialization is a documented FUTURE improvement,
//   deliberately not built here.
//
// ORDERING: SetBookText must land before the book is opened for the new text to
//   show. A consumer sets it at load/init or whenever the underlying data changes.
//
// This module has TWO seams, like CastTime: RegisterFuncs(vm) plugs its natives
// into the Papyrus dispatcher (Core/Papyrus.cpp), and Install() installs the engine
// hook from plugin.cpp on kDataLoaded.
//
// Phase L1 - Stage C.2

#pragma once

namespace Lodestone::Core::BookFramework
{
	// Registers this module's native functions with the Papyrus VM.
	// Called by Lodestone::Core::Papyrus::Register - never called directly.
	//
	// Registers: SetBookText, AppendBookText, ClearBookText, GetBookText on the
	// "Lodestone" script.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);

	// Installs the engine hook on the game's book-open function. Must be called
	// exactly once - NOT idempotent.
	//
	// Call site: plugin.cpp, on SKSE::MessagingInterface::kDataLoaded. Requires the
	// SKSE trampoline to be allocated first (SKSE::AllocTrampoline in
	// SKSEPluginLoad), because this is a branch hook, not a vtable swap.
	//
	// Never throws. Every failure path is logged and swallowed, and leaves books at
	// their vanilla text.
	void Install();
}
