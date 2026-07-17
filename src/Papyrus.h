// Papyrus.h
// Intelligence Matters - SKSE plugin
//
// Central dispatcher for Papyrus native registration.
//
// This is the seam Stage C plugs into. Each module (CastTime, BookFramework,
// SpellTomes, ActorValues) owns its own natives and exposes a RegisterFuncs(vm);
// this dispatcher is the ONLY place that knows the full list. plugin.cpp knows
// about this file and nothing else.
//
// To add a module in Stage C:
//   1. Create Modules/<Name>.h/.cpp with a RegisterFuncs(vm)
//   2. Add its .cpp to SOURCES in CMakeLists.txt
//   3. Add one line to Register() in Papyrus.cpp
//
// Phase 16 - Stage B.1 / B.2

#pragma once

namespace Lodestone::Papyrus
{
	// Registration callback handed to SKSE's Papyrus interface. SKSE calls this
	// once, when the VM is ready. Fans out to every module's RegisterFuncs.
	//
	// Returns false if any module failed to register.
	bool Register(RE::BSScript::IVirtualMachine* a_vm);
}
