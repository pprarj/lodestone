// SpellTomes.h
// Lodestone - Shared SKSE framework
//
// Module: SpellTomes - TRACE ONLY (Stage C.3 / L2, Part 1.5)
//
// This is the LOG-ONLY trace build that precedes Lodestone's spell-tome module -
// the capability to intercept reading a spell tome BEFORE the vanilla "learn the
// spell and eat the book" happens, so a consumer can run its own spell-learning
// (gated learning, keep the tome, custom effects) through an event. It is NOT the
// production module. It changes NOTHING: it calls the original first and only
// logs, so the game behaves exactly as vanilla while the trace runs.
//
// LAYER NOTE (open for review): the reference this replaces was tied to one
// consumer, which is why the roadmap filed this module under Domain. But the
// CAPABILITY is agnostic - any spell-learning mod can register for the read event,
// and nothing here needs a consumer's name. So it is written as Core (agnostic
// surface), consistent with the rest of the framework. If a genuine per-consumer
// coupling appears later, it can move to Domain then. Placement is a one-line
// change; flagged so it can be vetoed.
//
// THE ENGINE FACTS (Part 1, from the installed CommonLibSSE-NG 3.5.3 headers):
//   - A book is activated through TESObjectBOOK::Activate (vfunc 0x37,
//     RE/T/TESObjectBOOK.h), which is in VTABLE_TESObjectBOOK[0] - addressable
//     from the headers, no address-library ID needed (the CastTime pattern).
//   - A spell tome is a book with TeachesSpell(); GetSpell() gives the SpellItem;
//     the vanilla path learns it (Actor::AddSpell) and consumes the book.
//   - TESObjectBOOK::Read(reader) also exists but has no ID in the headers.
//
// THE QUESTION THIS ANSWERS:
//   Does TESObjectBOOK::Activate fire for a spell tome read from the INVENTORY, or
//   only from a world reference? That decides the production hook: if Activate
//   covers both paths, the production module hooks it (addressable, safe); if
//   inventory reads bypass it, production needs Read (whose ID would be sourced
//   like the book-open one in L1). The trace logs every spell-tome activation -
//   book, spell, who activated it, from where - so opening a tome from inventory
//   vs from the world, in TWO load orders, settles it.
//
// Phase L2 - Stage C.3 / Part 1.5 (TRACE)

#pragma once

namespace Lodestone::Core::SpellTomes
{
	// Installs the trace hook on the shared TESObjectBOOK vtable (Activate, 0x37).
	// Must be called exactly once - NOT idempotent. A vtable swap needs no
	// trampoline and no address-library ID.
	//
	// Call site: plugin.cpp, on SKSE::MessagingInterface::kDataLoaded. Never throws;
	// every failure path is logged and swallowed, leaving vanilla behavior intact.
	void Install();
}
