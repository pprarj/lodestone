// PluginInfo.h
// Intelligence Matters - SKSE plugin
//
// Module: PluginInfo
// Owns the plugin's self-describing native API (version / presence check).
// This is the first consumer of the module + dispatcher convention established
// in Stage B.1, and the module that satisfies the Stage B closing criterion.
//
// Papyrus-facing script: IMPlugin.psc
//
// Phase 16 - Stage B.1 / B.2

#pragma once

namespace Lodestone::PluginInfo
{
	// Registers this module's native functions with the Papyrus VM.
	// Called by Lodestone::Papyrus::Register - never called directly.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
