// SkillMessage.h
// Lodestone - Shared SKSE framework
//
// Module: SkillMessage
// Shows the vanilla "skill increased" HUD message for a name and a level the
// caller supplies. Display only: no actor value, skill, XP or PlayerSkills is
// touched. Stateless: no hook, no cosave.
//
// Papyrus-facing script: Lodestone.psc
//
// Phase L-N1

#pragma once

namespace Lodestone::Core::SkillMessage
{
	// Registers this module's native functions with the Papyrus VM.
	// Called by Lodestone::Core::Papyrus::Register - never called directly.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
