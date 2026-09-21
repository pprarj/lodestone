// WebUIBridge.h
// Lodestone - Shared SKSE framework
//
// Module: WebUIBridge (Core)
// Papyrus access to a web UI backend, which has no Papyrus surface of its own.
//
// THE MODULE IS NAMED FOR WHAT IT DOES, NOT FOR WHO SUPPLIES IT, and that is
// what this file was renamed from PrismaBridge to say. The rename happened in
// 1.18.0, while Prisma UI was still the only backend, on the argument that a
// contract naming its supplier cannot outlive it.
//
// THAT ARGUMENT WAS A PROMISE UNTIL 1.21.0 PAID IT. Meridian UI arrived as a
// second backend and no consumer had to change a line - a .pex built against
// 1.17.x, calling the Prisma* names, reached a backend that is not Prisma
// without being recompiled. The split the promise depended on is BUILT:
// WebUIBackend.h is the seam, PrismaUIBackend.cpp and MeridianUIBackend.cpp are
// the two sides of it, and this file keeps only what is true of any backend.
//
// ONE BACKEND IS CHOSEN PER SESSION, at kInputLoaded, and never revisited.
//
// WHY THIS EXISTS. Prisma UI ships a vtable and nothing else: no .psc, no .pex,
// no .esp, and no native registered on its DLL - measured by listing the
// installed mod (Prisma UI 1.4.1.0) and by inspecting the binary. A Papyrus-only
// consumer therefore cannot reach it at all, cannot even ask whether it is
// installed, without a DLL in between. Four consumers of this framework want a
// panel; without this module the answer is four DLLs.
//
// WHY THIS IS Core AND NOT Domain. The instruction that asked for this module
// called for "the Domain pattern", meaning gated on presence and passthrough on
// absence, and this module is exactly that. But Domain in THIS project is not
// the gating axis - it is "may hardcode its own consumer's plugin file"
// (CONVENTIONS.md, Project appendix). This module names no consumer: any mod
// passes its own view id and its own html path, and nothing here knows who is
// calling. That is the definition of Core. The gating discipline Domain
// requires is adopted anyway, because it is right here for the same reason it
// is right there - see the next paragraph.
//
// INACTIVE IS NOT BROKEN, AND THE LOG SAYS WHICH. With no backend present every
// native returns its sentinel and nothing is written at error level:
// WebUIAvailable() is a probe, and a probe that logs a failure teaches users to
// report a non-problem. That is the COMMON case, not the exceptional one - most
// load orders have neither backend installed. The one line written at load says
// which state this module is in and, when there was a contest, which backend won
// and which it passed over; from the outside those states are indistinguishable
// and unanswerable in a support thread.
//
// NOTHING IN LODESTONE CONSUMES THIS. The dependency points one way on purpose,
// and NEITHER backend is required: a framework that might stop moving may be
// exposed, never depended on. Prisma UI is the case that set the rule - its last
// public activity was 2026-03-27, and a defect report from a sibling project of
// this tree has gone unanswered since 2026-08-11. If every backend disappears,
// consumers of this module degrade to "no panel" and no other part of Lodestone
// notices.
//
// THERE IS A FOCUS SURFACE SINCE 1.22.0. WebUIFocusView, WebUIClearFocus and
// WebUIIsViewFocused let one view receive the game's mouse and keyboard. A
// consumer asks whether the installed backend can do it at all with
// WebUIHasCapability("view-focus") - and asks FIRST, because the answer is
// False with no backend, and was False on Prisma UI from 1.22.0 to 1.26.x.
//
// The single-holder rule is this module's: at most one view created through
// this bridge holds focus at a time, and the second asker is refused rather
// than queued. Focus is released automatically when a save loads, when a new
// game starts, and when a menu that pauses the game opens; the player's own
// escape is the panic chord, which no consumer can disable. On Meridian UI
// the chord is the backend's; on Prisma UI it is Lodestone's, since 1.27.0 -
// see PrismaUIBackend.cpp for why the two do not behave alike.
//
// "focus-stack" STILL ANSWERS False, AND THAT IS NOT A LEFTOVER. It asks
// whether TWO views can hold focus INDEPENDENTLY, which no backend here can do
// and which the paragraph above deliberately does not promise either. The two
// names are one keystroke apart in meaning and worlds apart in answer - see the
// trap written out in both backends and in Lodestone.psc.
//
// Version gates for consumers:
//   >= 1018000 (1.18.0)  the WebUI* surface. The 1.17.x names still answer.
//   >= 1022000 (1.22.0)  the three focus natives and "view-focus".
//   1.27.0 needs no gate of its own: "view-focus" started answering True on
//   Prisma UI, and a consumer that already asks sees the change.
//   >= 1030000 (1.30.0)  WebUIGetViewIds and WebUIGetVisibleViewIds.
//
// THE TWO ENUMERATION NATIVES GET NO CAPABILITY OF THEIR OWN, ON PURPOSE. They
// read this module's own view table and call no backend, so they answer the
// same with zero backends installed as with either one. A capability that can
// only answer True teaches a consumer to ask a question with one answer, and
// the version gate above already says whether they are there.

#pragma once

namespace Lodestone::Core::WebUIBridge
{
	// Gives every backend its chance to find its framework. Call once, on
	// kPostLoad.
	//
	// PROBING IS NOT CHOOSING, and separating them is what the second backend
	// forced. Prisma UI is settled when its Probe() returns; Meridian UI's only
	// arms a two-step SKSE handshake that finishes at kInputLoaded. Deciding here
	// would always pick Prisma, whatever the order said - so the choice is made
	// in HandleSKSEMessage below, not here.
	//
	// kPostLoad is what the Prisma header itself recommends for the request, and
	// it is also the latest point that is still early enough for a backend that
	// answers immediately: the pointer has to exist before the first Papyrus call
	// arrives.
	//
	// NO VIEW IS CREATED HERE, and that is the trap this module was written
	// around. At kPostLoad the D3D device and the renderer do not exist yet, and
	// CreateView at that moment queues forever or blocks the load chain -
	// established from the Add Item Menu's own source and paid for again by a
	// sibling project of this tree. Views are created only when a consumer asks,
	// which is necessarily later.
	//
	// Cannot fail in a way a caller can act on: no backend installed is a normal,
	// expected outcome and leaves the module inactive.
	void Acquire();

	// Forwards one SKSE message to every backend, and picks the session's
	// backend when the last of them has had its chance.
	//
	// CALL THIS FOR EVERY MESSAGE, not for a chosen few. One backend is
	// acquired by a two-step SKSE handshake rather than by a direct request:
	// Meridian UI asks for a version at kPostPostLoad and for its API at
	// kInputLoaded, and it only ever becomes available if it sees both. Which
	// seams a backend needs is the backend's business, so this hands over
	// everything and lets each one filter.
	//
	// The choice of backend is made at kInputLoaded and never revisited - see
	// Resolve() in the .cpp for why one per session is deliberate. Before that
	// message, WebUIAvailable() answers False; no consumer can observe it,
	// because Papyrus has not started by then.
	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg);

	// Registers this module's natives on the "Lodestone" script.
	//
	// 27 in total. The 18 of the current surface: WebUIAvailable,
	// WebUICreateView, WebUIIsViewReady, WebUICall, WebUIShow, WebUIHide,
	// WebUIIsViewVisible, WebUIDestroyView, WebUIRegisterListener,
	// WebUIGetBackend, WebUIHasCapability, WebUIGetViewState,
	// WebUIGetListenerSlotsFree, WebUIFocusView, WebUIClearFocus,
	// WebUIIsViewFocused, WebUIGetViewIds, WebUIGetVisibleViewIds.
	//
	// And the 9 deprecated 1.17.x names, which forward: PrismaAvailable,
	// PrismaCreateView, PrismaIsViewReady, PrismaCall, PrismaShow, PrismaHide,
	// PrismaIsHidden, PrismaDestroy, PrismaRegisterListener. They are removed in
	// the next internal major (2.0.0), not before.
	//
	// Registration happens whether or not a backend is installed. The natives
	// must exist for a consumer to be able to ask WebUIAvailable() at all, and
	// a script that fails to find a function it calls is a Papyrus error the
	// consumer cannot suppress.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
