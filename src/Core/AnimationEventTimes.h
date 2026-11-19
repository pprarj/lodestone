// AnimationEventTimes.h
// Lodestone - Shared SKSE framework
//
// Module: AnimationEventTimes (Core)
// When did a named event of the player's animation graph last happen, asked
// from Papyrus. The time is taken where the event happens - the player's own
// animation event sink, hooked by WebUIBridge - not where a script hears it.
//
// WHY THIS EXISTS. A script that times two animation events by the arrival of
// its OnAnimationEvent handlers measures the script queue, not the animation:
// on a large load order the handlers arrive hundreds of milliseconds late and
// out of order, so "released minus drawn" can come out negative or several
// times too long. The hook sees each event when the graph sends it; this module
// keeps that moment for the names a script asked for, and answers how long ago
// it was at the moment of the question.
//
// NO VIEW, NO WEB UI BACKEND. The hook belongs to WebUIBridge because the page
// delivery (1.50.0) came first; this module needs the hook and nothing else of
// the bridge, so it works with no backend installed.
//
// THE NATIVES DO NOT WAIT FOR A FRAME. They are registered callable from
// tasklets: a native that is not waits for the next frame on the main thread,
// and two questions in a row would be a frame or more apart. These read and
// write only this module's own state, under its own lock, so running them on
// the VM thread is safe.
//
// Papyrus-facing script: Lodestone.psc
//
// Phase L-W2

#pragma once

#include <chrono>

namespace Lodestone::Core::AnimationEventTimes
{
	// Whether any request is open. One atomic load; the hook reads it before it
	// knows it has work.
	bool Any();

	// Called by the player's animation event hook, on whatever thread the event
	// arrives on, with the two clocks read once at the hook: a_at for the ages
	// and intervals, a_wallMs (the "t" a page receives) for the log line.
	// a_logEach is LogWebUIEvents. Never throws.
	void Record(const char* a_tag, std::chrono::steady_clock::time_point a_at, double a_wallMs, bool a_logEach);

	// kPreLoadGame and kNewGame end every request. Call for every message.
	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg);

	// Registers the five natives on the "Lodestone" script:
	// RegisterPlayerAnimationEventTimes, UnregisterPlayerAnimationEventTimes,
	// GetPlayerAnimationEventAge, GetPlayerAnimationEventInterval,
	// GetPlayerAnimationEventCount. Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
