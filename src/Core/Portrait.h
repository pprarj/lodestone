// Portrait.h
// Lodestone - Shared SKSE framework
//
// A front portrait of the player, as a PNG a web view can load.
// Lodestone.CapturePlayerPortrait(String asFolder, Int aiSize) and
// Lodestone.CapturePlayerPortraitCutout(String asFolder, Int aiSize, String
// asKey, Bool abTrimBackground) are latent: the calling script waits a few
// frames and gets the file name back, "" when the capture was refused or
// failed (the reason is in Lodestone.log). The cutout is framed closer, lit
// by a light of its own, shows the hair under a helmet and has alpha 0 where
// the depth says it is not the player. Lodestone.GetPlayerPortrait(String
// asFolder, String asKey) returns the newest file of a key without a capture,
// and Lodestone.GetPlayerPortraitFailure() the reason code of the last capture
// that answered "".
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
	// at once (its stack goes away with the load). kPostLoad, kPostPostLoad and
	// kDataLoaded: SmoothCam's modder interface is asked for.
	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg);
}
