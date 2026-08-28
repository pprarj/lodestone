// Serialization.h
// Lodestone - Shared SKSE framework
//
// Module: Serialization (Core)
//
// The plugin's SINGLE owner of the SKSE cosave. It persists nothing of its
// own: it registers the callbacks once and hands each record to whichever
// module claims it.
//
// WHY IT EXISTS, AND IT IS A CONSTRAINT RATHER THAN A TIDY-UP. SKSE gives a
// plugin ONE set of cosave callbacks - SetSaveCallback, SetLoadCallback,
// SetRevertCallback - and the load callback walks the record stream with
// GetNextRecordInfo. That stream is consumed by whoever reads it first, so two
// modules each looping over it independently is not "both work": the first one
// drains it and the second sees an empty save. A second SetLoadCallback does
// not even get that far - it replaces the first.
//
// Core/Incapacitation owned the registration while it was the only module that
// persisted anything. The moment a second module needed the cosave, that
// arrangement had to become one owner dispatching by record type. This is that
// owner.
//
// HOW A MODULE PLUGS IN. Four functions, and the shape matters:
//
//   CosaveSave(intfc)                    - write your record
//   CosaveLoadBegin()                    - reset your state, EVERY load
//   CosaveLoadRecord(intfc, type) -> bool - claim the record or decline it
//   CosaveRevert()                       - new game, or back to main menu
//
// CosaveLoadBegin runs whether or not your record turns out to be in the save.
// A module that only resets inside CosaveLoadRecord keeps stale state from the
// previous session when a save has no record of its own - which is exactly the
// case of a save made before that module existed.
//
// CosaveLoadRecord returns false for a record type that is not yours. The
// dispatcher then offers it to the next module, and a record nobody claims is
// skipped rather than being an error: a future version of this plugin may
// write records this one has never heard of, and an older build must survive
// reading a newer save.
//
// Phase: equip veto - the second module to need the cosave.

#pragma once

namespace Lodestone::Core::Serialization
{
	// Registers the plugin's cosave callbacks with SKSE. Call exactly once,
	// from SKSEPluginLoad, before any save or load can happen.
	//
	// Cannot fail in a way the caller can act on: without the serialization
	// interface it logs and returns, and every module that persists state
	// simply stops persisting for the session.
	void Register();
}
