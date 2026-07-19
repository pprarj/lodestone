// MagicScaling.h
// Lodestone - Shared SKSE framework
//
// Phase L3, Part 1.5 - TRACE ONLY. This build observes and logs; it changes
// nothing in game and adds no Papyrus API.
//
// GOAL OF THE MODULE (Part 2, not this build): scale a spell's magnitude,
// duration and magicka cost from values a consumer drives at runtime, in the
// same shape as the cast time module - the consumer registers its own globals,
// the plugin applies them at the engine's calculation point. No custom actor
// values are involved: the capability is the RESULT (scaled magic), not the
// mechanism a consumer used to reach it before.
//
// WHAT THIS TRACE HAS TO ANSWER (Part 1 left these open):
//
//   Q1. Does ActiveEffect::AdjustForPerks carry the post-perk magnitude and
//       duration? The header shows it takes the caster and the target, and the
//       ActiveEffect carries duration@74 and magnitude@78. If magnitude changes
//       across the original call, that is the engine applying its perk entry
//       points, and it is the right seam to scale afterwards - exactly how the
//       cast time module scales castingTimer after the original wrote it.
//
//   Q2. Does ONE branch hook cover every effect type? AdjustForPerks is vfunc
//       0x00 of ActiveEffect, and there are around fifty derived vtables - far
//       too many to swap one by one. But ValueModifierEffect (checked in the
//       3.5.3 headers) does NOT override it, so the derived vtables should all
//       point their slot 0x00 at the SAME base implementation. If that holds, a
//       single branch hook on that one body covers all of them. The census in
//       Install() reads those slots and prints the answer instead of assuming
//       it.
//
//   Q3. Is SpellItem::AdjustCost the cost seam? The header gives an exact
//       signature - void(float& cost, Actor*) at vfunc 0x63, overridden only by
//       SpellItem in the whole tree. Cost by reference plus the actor is the
//       same shape the cast time hook has. The trace confirms it fires, for the
//       player, with a plausible value, and whether the original changes the
//       cost itself.
//
// ADDRESSES: nothing here is a hardcoded RELOCATION_ID taken from an outside
// source. The cost hook is a vtable swap addressable from the shipped headers.
// The AdjustForPerks body address is READ OUT of VTABLE_ActiveEffect at runtime,
// so it is derived from a header constant rather than guessed - this is
// deliberately unlike the L1 and L2 addresses, which are still pending in-game
// confirmation.
//
// RISK: none intended. Every thunk calls the original FIRST and then only reads
// and logs. Nothing is written back. A trace that changed a value would be
// measuring itself.
//
// Phase L3 (Part 1.5)

#pragma once

namespace Lodestone::Core::MagicScaling
{
	// Runs the vtable census and installs the log-only hooks. Called from
	// plugin.cpp on kDataLoaded, like the other engine hooks. Never throws.
	void Install();
}
