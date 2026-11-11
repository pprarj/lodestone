// MagicWrite.h
// Lodestone - Shared SKSE framework
//
// Module: MagicWrite
// Rewrites a Spell or a MagicEffect that ALREADY EXISTS - normally one a
// consumer's plugin declares empty - from another form of the same type, or
// entry by entry. Never creates a form, never assigns a FormID, keeps nothing
// in the cosave: what it writes lives in memory for the rest of the game
// session, and the consumer rewrites it on every load.
//
// Papyrus-facing script: Lodestone.psc
//
// Phase L-F5

#pragma once

namespace Lodestone::Core::MagicWrite
{
	// Registers this module's native functions with the Papyrus VM.
	// Called by Lodestone::Core::Papyrus::Register - never called directly.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
