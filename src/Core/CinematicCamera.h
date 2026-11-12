// CinematicCamera.h
// Lodestone - Shared SKSE framework
//
// L-C1 - a camera that starts far and high and closes in on an actor until it
// reaches the pose the normal third person camera had when it began.
//
// Natives (Lodestone.psc, DLL 1.42.0):
//   CinematicApproach(akTarget, afBack, afUp, afSeconds) -> Bool
//   CinematicIsActive() -> Bool
//   CinematicStop()
// ModEvent "Lodestone_CinematicDone", strArg = done | stopped | load | timeout.

#pragma once

namespace Lodestone::Core::CinematicCamera
{
	// Installs the two engine seams (the free camera state's vtable and the
	// player's per frame Update). Called at kDataLoaded. Idempotent.
	void Install();

	// A load or a new game ends an approach: the camera state is rebuilt by the
	// engine, and the consumer is told with strArg "load".
	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg);

	// Registers this module's native functions with the Papyrus VM.
	// Called by Lodestone::Core::Papyrus::Register - never called directly.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
