// SpellTomes.h
// Lodestone - Shared SKSE framework
//
// Module: SpellTomes (Core)
// Owns spell-tome interception. Once a consumer has registered, reading a spell
// tome grants NOTHING: the spell is not taught and the book is not consumed. The
// read is reported to the registered consumers, which decide on their own terms
// whether the spell is ever learned and whether the book is ever eaten. With no
// consumer registered the module is pure passthrough - tomes teach and are eaten
// exactly as in vanilla.
//
// SCOPE, WIDENED IN 1.5.0. Through 1.4.0 this module only stopped the book from
// being eaten and deliberately did not touch LEARNING. That turned out to be half
// a capability: a consumer building a study session or an attribute gate cannot
// build it if the engine has already granted the spell by the time it is told the
// tome was read. Its handler sees a spell the player already knows and has nothing
// left to gate. Suppressing the learn is therefore part of the job, not a policy
// choice - what to do INSTEAD of learning stays entirely with the consumer.
//
// BEHAVIOR ON INSTALL, CHANGED IN 1.8.0: passthrough. The hook is installed
// always, but until a consumer registers, every book - spell tome included -
// takes the original path untouched, identical to the DLL not being installed.
// The first registration is what switches the suppression on. (From 1.5.0 to
// 1.7.0 suppression was unconditional, an intentional exception to "passive
// until asked"; the exception and why it was retired are recorded at Install()
// in SpellTomes.cpp.)
//
// ORDERING REQUIREMENT (the consumer must honor this): registration must land
// BEFORE the first tome read the consumer wants intercepted. The consumer
// registers early - a load/init event or an init quest, and again on every game
// load, since registration is session-scoped. A tome read before the first
// registration passes as vanilla (the spell is learned, the book is consumed)
// and is NOT reverted when a registration lands later. This is the same silent
// degradation the other modules already accept: "not registered yet" is
// indistinguishable from "not installed".
//
// With a registrant, the read still counts as a read: the book is flagged
// kHasBeenRead, because the player did open it. Only learning and consumption
// are suppressed. See the thunk in SpellTomes.cpp for why that one side effect
// is restored by hand and the others are not.
//
// Skill books, notes and ordinary books are passed through untouched either way,
// including the engine's own decision about consuming them.
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
//   spell-tome read then dispatches that event to it. Teaching is the consumer's
//   call, made with Papyrus AddSpell whenever its own system says the spell is
//   earned - there is no native for it and none is needed. Consuming is
//   ConsumeSpellTome, called if and when the book should be spent. A consumer that
//   wants plain vanilla behavior back reproduces it by calling both in its handler.
//   Registration is session-scoped (not serialized) - a consumer re-registers
//   after each load, the same runtime model the other modules use.
//
//   The event carries no return value and cannot: Papyrus events do not have one,
//   and dispatch is asynchronous. The DLL therefore never waits to be told what to
//   do - it suppresses, reports, and the consumer acts afterwards.
//
// HOW IT WORKS:
//   The read is TESObjectBOOK::Read, which teaches the spell and returns whether
//   the book should be consumed. This module hooks it. With no registrant the
//   thunk calls the original and returns its answer, for every book. With at
//   least one registrant, a spell tome does not call the original at all - that
//   is the only way to suppress a teach the original performs itself. It flags
//   the book read, returns FALSE so the caller keeps it, and dispatches
//   OnSpellTomeRead. Every other book type calls the original and returns its
//   answer unchanged. "Is anyone registered" is a count kept by the module
//   itself, updated inside Register/Unregister under the set's own lock -
//   RegistrationSet does not expose emptiness.
//
//   The address (RELOCATION_ID(17439, 17842)) is not in the installed headers; it
//   came from the CommonLibSSE-NG source and was proven in game, along with the
//   meaning of the return value. Skipping the original was validated in game too,
//   in 1.5.0. Both observations are recorded at the thunk in SpellTomes.cpp,
//   together with what is still unobserved.
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
	// Call site: plugin.cpp, on SKSE::MessagingInterface::kDataLoaded. This is a
	// SafetyHook inline hook, not a branch hook - it does not use the SKSE
	// trampoline.
	//
	// Never throws. Every failure path is logged and swallowed, leaving vanilla
	// spell-tome behavior intact.
	void Install();
}
