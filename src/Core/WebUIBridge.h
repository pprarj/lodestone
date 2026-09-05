// WebUIBridge.h
// Lodestone - Shared SKSE framework
//
// Module: WebUIBridge (Core)
// Papyrus access to a web UI backend, which has no Papyrus surface of its own.
//
// THE MODULE IS NAMED FOR WHAT IT DOES, NOT FOR WHO SUPPLIES IT, and that is
// what this file was renamed from PrismaBridge to say. The backend supported
// today is Prisma UI, and the vendor name below is factually correct about it -
// it is not left over. When a second backend arrives, the body of the .cpp
// becomes PrismaUIBackend.cpp, where the name is still correct, and this file
// keeps only the parts that are true of any backend. That split is designed and
// is not built - see the phase document.
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
// INACTIVE IS NOT BROKEN, AND THE LOG SAYS WHICH. With Prisma UI absent every
// native returns its sentinel and nothing is written at error level:
// PrismaAvailable() is a probe, and a probe that logs a failure teaches users to
// report a non-problem. The one line written at load says which of the two
// states this module is in, because from the outside they are indistinguishable
// and unanswerable in a support thread.
//
// NOTHING IN LODESTONE CONSUMES THIS. The dependency points one way on purpose.
// Prisma UI's last public activity was 2026-03-27 and a defect report from a
// sibling project of this tree has gone unanswered since 2026-08-11; a framework
// that might stop moving may be exposed, never depended on. If Prisma UI
// disappears, every consumer of this module degrades to "no panel" and no other
// part of Lodestone notices.
//
// NO FOCUS SURFACE IN THIS VERSION, AND IT IS NOT AN OVERSIGHT. Focus, Unfocus,
// HasFocus and HasAnyActiveFocus are deliberately not exposed. Prisma's
// FocusMenu is a single kModal IMenu with no focus stack: Unfocus(view) closes
// it unconditionally, so with two Prisma views on screen the other one's cursor
// is stranded. A sibling project of this tree exhausted the four-way
// configuration matrix in game and found no mitigation, and the two public flags
// do not touch the broken path. A widget that never calls Focus never meets any
// of that. Exposing focus that no consumer has asked for would import the defect
// into this framework for free. It arrives when a consumer needs input, and the
// defect becomes that phase's problem, openly.
//
// A consumer asks about that with WebUIHasCapability("focus-stack"), which
// answers False.
//
// Version gate for consumers of the current surface:
// Lodestone.GetVersion() >= 1018000 (1.18.0). The 1.17.x names still answer.

#pragma once

namespace Lodestone::Core::WebUIBridge
{
	// Acquires the Prisma UI API pointer. Call once, on kPostLoad.
	//
	// kPostLoad is what the Prisma header itself recommends for the request, and
	// it is also the latest point that is still early enough: the pointer has to
	// exist before the first Papyrus call arrives.
	//
	// NO VIEW IS CREATED HERE, and that is the trap this module was written
	// around. At kPostLoad the D3D device and the Ultralight renderer do not
	// exist yet, and CreateView at that moment queues forever or blocks the load
	// chain - established from the Add Item Menu's own source and paid for again
	// by a sibling project of this tree. Views are created only when a consumer
	// asks, which is necessarily later.
	//
	// Cannot fail in a way a caller can act on: Prisma UI absent is a normal,
	// expected outcome and leaves the module inactive.
	void Acquire();

	// Registers this module's natives on the "Lodestone" script.
	//
	// 22 in total. The 13 of the current surface: WebUIAvailable,
	// WebUICreateView, WebUIIsViewReady, WebUICall, WebUIShow, WebUIHide,
	// WebUIIsViewVisible, WebUIDestroyView, WebUIRegisterListener,
	// WebUIGetBackend, WebUIHasCapability, WebUIGetViewState,
	// WebUIGetListenerSlotsFree.
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
