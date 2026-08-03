// Detection.h
// Lodestone - Shared SKSE framework
//
// Module: Detection (Core)
// Owns one scaling channel for a detection-related value.
//
// WHAT THIS IS. A channel of exactly the same shape as the magic scaling ones: a
// consumer owns a multiplier and an offset, drives them from its own Papyrus
// state, registers them, and the DLL composes every registrant into one pair -
// value = value * mult + offset. Multi-contributor from its first published
// version, so there is no single-owner behavior here to migrate away from later.
//
// WHAT THIS IS NOT, and the distinction is the whole reason this file is short:
// there is NO ENGINE HOOK. Nothing in this module reads light, noise or
// movement, and nothing applies the composed pair to the engine's detection
// calculation. A consumer that registers here and does nothing else changes
// nothing about the game.
//
// So what is it for today? Composition. A consumer computes its own detection
// value in Papyrus, and if a second mod ever wants a say in that same value, the
// two compose here instead of one of them silently losing - which is the failure
// this whole version exists to remove. The channel is also readable by whatever
// hook eventually applies it: when the context-reading work lands, it calls
// GetChannel().Read() and needs nothing new from this file.
//
// SHIPPING THE CHANNEL BEFORE THE HOOK IS THE CHEAP ORDER, not an oversight. The
// Papyrus API only grows and a published signature can never be changed, so a
// channel that is born single-owner and has to be migrated after two consumers
// exist costs a deprecation. Born multi-contributor, it costs nothing. The hook,
// by contrast, needs engine investigation that has not happened, and guessing
// its shape now is the expensive mistake in the other direction.
//
// ONE SEAM ONLY. Unlike CastTime and MagicScaling this module has no Install():
// there is no vtable to swap and no function to detour, so plugin.cpp does not
// mention it. RegisterFuncs() through the dispatcher is its entire wiring.
//
// Version gate for consumers: Lodestone.GetVersion() >= 1009000 (1.9.0).
//
// Phase L-A (detection scaling channel)

#pragma once

#include "MultiChannel.h"

namespace Lodestone::Core::Detection
{
	// The module's channel. Read by the diagnostic natives in ChannelInfo.cpp,
	// and by whatever eventually applies it.
	MultiChannel& GetChannel();

	// Registers this module's natives on the "Lodestone" script.
	// Registers: RegisterDetectionMultiplierChannel.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
