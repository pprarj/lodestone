// Translation.h
// Lodestone - Shared SKSE framework
//
// Module: Translation
// Resolves a '$' translation key to its translated text for Papyrus, with the
// caller's runtime values substituted into the result. Read-only over the game's
// own translation table: no hook, no cosave, nothing cached.
//
// The table is ONE for the whole load order, so a consumer reads keys belonging
// to any active plugin, not only its own. Measured in game, phase L-T1.
//
// Papyrus-facing script: Lodestone.psc
//
// Phase L-T2

#pragma once

namespace Lodestone::Core::Translation
{
	// Loads this plugin's own Interface\Translations\Lodestone_<LANGUAGE>.txt into
	// the game's translation table.
	//
	// THIS IS NOT OPTIONAL AND IT IS NOT A CONVENIENCE. The game does not scan
	// the Translations folder: SKSE walks plugins.txt and opens one file per
	// ACTIVE PLUGIN NAME. Lodestone has no plugin, so nothing would ever open its
	// file. Measured twice in game on 2026-10-01 - once by watching the table grow
	// by exactly this file's line count after the call, and once from the other
	// side, with skse64.log listing every file it read and this one absent while a
	// probe mod that had been given an .esp was read.
	//
	// Called from the kDataLoaded seam, which is also the first seam where the
	// Scaleform translator exists at all (measured: null at kPostLoad,
	// kPostPostLoad and kInputLoaded).
	void Install();

	// Registers this module's native functions with the Papyrus VM.
	// Called by Lodestone::Core::Papyrus::Register - never called directly.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
