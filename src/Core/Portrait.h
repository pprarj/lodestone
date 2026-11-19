// Portrait.h
// Lodestone - Shared SKSE framework
//
// A front portrait of the player, as a PNG a web view can load. One native,
// Lodestone.CapturePlayerPortrait(String asFolder, Int aiSize), latent: the
// calling script waits a few frames and gets the file name back, "" when the
// capture was refused or failed (the reason is in Lodestone.log).
//
// The copy is made inside the frame, at the entry of the engine's frame swap
// (Main::Swap), from renderTargets[kMAIN] - the world without the interface.
// For the copy the camera is held for three camera updates in front of the
// player's head, the worldToCam matrix is rebuilt after each write, and the
// third person geometry of a helmet is culled; both go back after the copy.
// See Portrait.cpp.

#pragma once

namespace Lodestone::Core::Portrait
{
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);

	// kDataLoaded: the frame hook, the three camera state hooks and the
	// worldToCam rebuild function. VR: nothing is installed.
	void Install();

	// A save being loaded or a new game: a script waiting for a capture gets ""
	// at once (its stack goes away with the load).
	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg);
}
