// PerkPoints.h
// Lodestone - Shared SKSE framework
//
// Module: PerkPoints
// The player's perk points and the loaded perk trees, for Papyrus: read a skill's
// tree in batch, ask whether the vanilla perk menu would let the player buy a
// perk, and buy it in ONE call (AddPerk plus the point debit). Stateless: no
// hook, no cosave, nothing cached. The point count lives in the engine's own
// game-state block, so it persists in the save exactly as the vanilla count does.
//
// Papyrus-facing script: Lodestone.psc
//
// Phase L-P1

#pragma once

namespace Lodestone::Core::PerkPoints
{
	// Registers this module's native functions with the Papyrus VM.
	// Called by Lodestone::Core::Papyrus::Register - never called directly.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
