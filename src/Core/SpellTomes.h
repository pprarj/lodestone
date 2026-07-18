// SpellTomes.h
// Lodestone - Shared SKSE framework
//
// Module: SpellTomes (Core)
// Owns the "keep the spell tome" capability (Stage C.3): reading a spell tome
// still teaches the spell exactly as vanilla, but the book is NOT consumed. A
// consumer is notified of the read and decides, on its own terms, whether and when
// the book is finally consumed.
//
// SCOPE - deliberately narrow (settled with the design owner): Lodestone does NOT
// touch spell LEARNING. Whether the spell is learned instantly by the engine or by
// a consumer's own system is none of this module's business. Lodestone's only job
// is to stop the book from being eaten, and to expose a read event plus an explicit
// consume call so a consumer stays in control.
//
// DEFAULT BEHAVIOR: with this module installed, reading a spell tome learns the
// spell (vanilla, untouched) and keeps the book. This is an intentional change to
// vanilla on install, not a passthrough - it is the whole point of the module.
//
// PUBLIC API (declared in Lodestone.psc, "Lodestone" script):
//
//   Bool  Function RegisterForSpellTomeRead(Form akReceiver) global native
//   Bool  Function UnregisterForSpellTomeRead(Form akReceiver) global native
//   Bool  Function ConsumeSpellTome(Book akBook, ObjectReference akActor) global native
//
//   And the event a registered script implements:
//     Event OnSpellTomeRead(Book akBook, ObjectReference akReader)
//
//   A consumer registers a form whose script implements OnSpellTomeRead; every
//   spell-tome read then dispatches that event to it. When the consumer decides the
//   book should be eaten (e.g. after its own study/learning finishes, or never),
//   it calls ConsumeSpellTome. If nothing calls ConsumeSpellTome, the book stays.
//   Registration is session-scoped (not serialized) - a consumer re-registers
//   after each load, the same runtime model the other modules use.
//
// HOW IT WORKS (pending in-game validation):
//   The read is TESObjectBOOK::Read, which teaches the spell and returns whether
//   the book should be consumed. This module hooks it: it calls the original (so
//   the spell is still learned), and for a spell tome returns FALSE so the caller
//   does not remove the book, then dispatches OnSpellTomeRead. The Read address is
//   not in the installed headers; it is taken from the CommonLibSSE-NG source
//   (RELOCATION_ID(17439, 17842)), and the assumption that the caller honors the
//   return value to decide consumption must be confirmed in game. See SpellTomes.cpp.
//
// This module has TWO seams, like CastTime and BookFramework: RegisterFuncs(vm)
// plugs its natives into the dispatcher (Core/Papyrus.cpp), and Install() installs
// the engine hook from plugin.cpp on kDataLoaded.
//
// Phase L2 - Stage C.3

#pragma once

namespace Lodestone::Core::SpellTomes
{
	// Registers this module's native functions with the Papyrus VM.
	// Called by Lodestone::Core::Papyrus::Register - never called directly.
	//
	// Registers: RegisterForSpellTomeRead, UnregisterForSpellTomeRead,
	// ConsumeSpellTome on the "Lodestone" script.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);

	// Installs the engine hook on TESObjectBOOK::Read. Must be called exactly once -
	// NOT idempotent.
	//
	// Call site: plugin.cpp, on SKSE::MessagingInterface::kDataLoaded. Requires the
	// SKSE trampoline to be allocated first (SKSE::AllocTrampoline in
	// SKSEPluginLoad), because this is a branch hook.
	//
	// Never throws. Every failure path is logged and swallowed, leaving vanilla
	// spell-tome behavior intact.
	void Install();
}
