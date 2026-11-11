// MagicWrite.h
// Lodestone - Shared SKSE framework
//
// Module: MagicWrite
// Rewrites a Spell or a MagicEffect that ALREADY EXISTS - normally one a
// consumer's plugin declares empty - from another form of the same type, or
// entry by entry. Never creates a form and never assigns a FormID. The
// consumer rewrites on every load; in between, every save keeps the final
// state of what was written (cosave record 'MWST'), and at kPreLoadGame that
// state is written back before the engine reads the save, so an effect that
// was active when the game saved is rebuilt from the same content. See the
// header of MagicWrite.cpp.
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

	// Filters its own SKSE messages: kDataLoaded (load order probe, only with
	// LogMagicWrite = 1), kPreLoadGame (restore from the save's cosave file)
	// and kPostLoadGame (probe summary).
	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg);

	// Cosave seam, called by Core/Serialization. CosaveLoadBegin resets
	// nothing on purpose (the restore already ran at kPreLoadGame).
	void CosaveSave(SKSE::SerializationInterface* a_intfc);
	void CosaveLoadBegin();
	bool CosaveLoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, std::uint32_t a_version, std::uint32_t a_length);
}
