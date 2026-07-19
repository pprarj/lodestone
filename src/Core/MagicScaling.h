// MagicScaling.h
// Lodestone - Shared SKSE framework
//
// Runtime scaling of a spell's magnitude, duration and magicka cost.
//
// THE CAPABILITY. A consumer owns three pairs of GlobalVariable records and
// drives them from its own Papyrus state - typically from some attribute it
// tracks. It registers each pair through the natives below, and from then on
// this module applies that pair to the matching quantity at the point where the
// engine has finished computing it. Nothing here decides balance: the DLL
// applies numbers the consumer supplies, which is the same division of labour
// the cast time module follows.
//
// PASSIVE UNTIL ASKED. Every channel is independent and every one is
// passthrough until someone registers it. Install this plugin with no consumer
// present and magnitude, duration and cost are exactly what they would have
// been - the hooks are installed, but they do nothing beyond a pointer compare.
// A consumer that registers only the cost channel gets only cost scaled.
//
// WHERE IT APPLIES (all three located by measurement - see the Part 1.5 trace in
// git history, commit 3f04519, and the notes in the .cpp):
//
//   magnitude  ActiveEffect::AdjustForPerks   vfunc 0x00, vtable swap
//   duration   ActiveEffect::AdjustForPerks   same hook
//   cost       MagicItem::CalculateCost       inline hook (non-virtual)
//
// The engine has already applied its own perks by the time each hook's original
// returns, so this scales the finished value rather than competing with the
// engine over it.
//
// SCOPE - ORDINARY SPELLS ONLY (v1 decision, worth a review). AdjustForPerks
// fires for every active effect on the actor, which during the trace included
// armour enchantments at magnitude 25 and a quest ability at 300. A consumer
// asking to scale "spell magnitude" does not mean those. So the magnitude and
// duration channels apply only when the effect's source is SpellType::kSpell -
// not abilities, not enchantments, not powers. Narrow is the recoverable
// direction: widening later is additive, whereas shipping wide would silently
// rescale every enchantment in the load order and there would be no way to take
// that back from saves already played.
//
// FORMULA, per channel: value = (value * multiplier) + offset. A channel whose
// multiplier is 1 and offset 0 is a no-op, so a consumer can neutralise one
// channel without unregistering it. Negative results are clamped to zero.
//
// ONLY WHAT ALREADY EXISTS IS SCALED. An effect with no magnitude keeps none -
// the offset is not applied to a zero, because that would invent magnitude on
// effects that never had any (every script effect and controller ability in the
// load order). Same for duration. This mirrors the cast time module, which
// refuses to invent charge time on a spell whose record has none.
//
// PLAYER ONLY, like the cast time module - by decision, not by limitation. Both
// hooks receive the casting actor, the formula is evaluated per call rather than
// cached, and Channel is a plain struct a second instance can be made of. So the
// player test is one branch in each thunk, and nothing here has to be rebuilt to
// serve NPCs.
//
// RESERVED SEAM - NPC SCALING, and the trap in it. Relaxing the player test is
// NOT how NPC support gets built. A channel is a pair of GlobalVariable records,
// and a global holds ONE value: that works for the player because there is one
// player. Dropping the test would apply the player's own multiplier to every NPC
// in the cell, which is worse than not supporting them at all.
//
// NPC scaling needs a per-actor source instead, and there are at least three
// shapes it could take:
//   - one global shared by all NPCs (trivial, but every mage scales alike,
//     regardless of skill or level)
//   - an actor value the consumer writes per NPC and this module reads in the
//     hook (per actor, and a cheap read on a hot path)
//   - a Papyrus callback per cast (most flexible, and the one to avoid - calling
//     into the VM from a hook this frequent is expensive and fragile)
//
// Which one is right depends on a design decision that has not been made: what
// an NPC's scaling should derive from. That is why no NPC API is published here
// yet. The Papyrus API only ever grows - a published signature can never be
// changed - so guessing the shape now would be carried forever, while the
// internal cost of adding it later does not grow with time. Deferring is the
// cheap direction on both axes.
//
// The cast time module carries a similar note; its wording assumes the
// derivation is the actor's school skill, which is one of the options above
// rather than a settled decision.
//
// SINGLE CHANNEL PER QUANTITY, first-registrant-wins, matching the cast time
// module's v1 policy. Re-registering the same pair is an idempotent refresh (a
// consumer may re-register on every load); a different second registrant is
// warned and rejected. Arbitration between several scaling mods is a future
// improvement and is not a blocker.
//
// PERSISTENCE. Registration is session-scoped and not serialised. A consumer
// re-registers after each load, exactly as it does for the other modules.
//
// Version gate for consumers: Lodestone.GetVersion() >= 1004000 (1.4.0).
//
// Phase L3 (Part 2)

#pragma once

namespace Lodestone::Core::MagicScaling
{
	// Installs the engine hooks. Called from plugin.cpp on kDataLoaded. Never
	// throws; on failure the module stays passthrough and says so in the log.
	void Install();

	// Registers this module's natives on the "Lodestone" script. Plugged into
	// Core/Papyrus.cpp like every other module with a Papyrus surface.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
