// ChordClaim.h
// Lodestone - Shared SKSE framework
//
// Module: ChordClaim (Core)
//
// A mod claims a CHORD - one keyboard key plus a non-empty set of modifiers - and
// from then on that chord reaches the claimant alone. The key press is taken out
// of the input event list before anyone else is handed it, so another mod's
// RegisterForKey on the same key, and the game's own action for it, never see it.
//
// Papyrus-facing script: Lodestone.psc
//
// Phase L-H1
//
// ---------------------------------------------------------------------------
// WHY THIS EXISTS
// ---------------------------------------------------------------------------
//
// Papyrus has no way to stop a second RegisterForKey from receiving a key. A mod
// that binds Shift+U can check the modifier itself (and should), but a neighbour
// bound to bare U that checks nothing fires on the same press, and the first mod
// cannot do anything about it from inside the VM. The only place the key can be
// stopped is before the event reaches the sink that feeds the VM, and that is
// native.
//
// ---------------------------------------------------------------------------
// WHAT IS TAKEN OUT, AND WHAT IS NOT
// ---------------------------------------------------------------------------
//
// ONE EVENT, NOT THE BATCH. Returning kStop from the sink would skip every sink
// behind it for the whole batch, and any other key or the mouse that arrived in
// the same frame would vanish with the claimed one - for the claimant too. The
// module unlinks only the button event of the claimed key from the list. The
// modifier keys, other keys and the mouse stay exactly where they were.
//
// DOWN, REPEAT AND UP of the claimed key are taken, as one press: the down is
// matched against the modifiers held at that moment, and the repeat and release
// that belong to it follow it, whatever the modifiers do in between. A release
// whose down was not taken (the key was pressed before the chord was complete)
// passes untouched.
//
// THE MODIFIERS PASS, ALWAYS. Shift is still Shift to the game, so a claimed
// Shift+U still makes the player walk while it is held. Which modifiers are held
// is read from the same event stream, not from the device: a modifier whose
// release the game never delivered (focus lost with the key down) stays held
// until its next event. That case was not measured.
//
// A BARE KEY IS NOT CLAIMABLE. Whoever owns a key that nobody else shares has no
// conflict to solve; whoever shares one has a conflict that is the player's to
// settle by remapping. A claim needs at least one modifier.
//
// ONLY OUTSIDE MENUS. While the game is paused by a menu, a modal menu is open or
// something is taking text (the console), nothing is taken: the press goes to
// whoever has the screen, as it would without this module.
//
// ---------------------------------------------------------------------------
// HOW THE CLAIMANT HEARS IT
// ---------------------------------------------------------------------------
//
// By mod event. ClaimChord takes the event name; the claimant registers for it
// with RegisterForModEvent. The event name is also the claimant's identity for
// ReleaseChord, so two mods cannot release each other's chords.
//
// ---------------------------------------------------------------------------
// LIFETIME
// ---------------------------------------------------------------------------
//
// Claims live in memory only and are dropped when a save is loaded or a new game
// starts, before any script of the new session runs. The claimant claims again
// in OnPlayerLoadGame, exactly where it calls RegisterForKey. Nothing is written
// to the save, so a mod that is uninstalled leaves no claim behind.

#pragma once

#include <cstdint>
#include <string>

namespace Lodestone::Core::ChordClaim
{
	// Result codes of Claim. Negative is a refusal and nothing changed.
	enum : std::int32_t
	{
		kClaimed       = 0,   // the chord is now the caller's
		kAlreadyYours  = 1,   // same chord, same event name: nothing to do
		kBadKey        = -1,  // not a keyboard scan code in 1..255, or a modifier key
		kBadModifiers  = -2,  // zero, or a bit outside 1|2|4
		kNoEvent       = -3,  // empty event name
		kHeldByOther   = -4,  // the chord belongs to a different event name
		kTableFull     = -5,
		kNotInstalled  = -6,  // the input sink is not in; the engine's input manager did not exist
	};

	std::int32_t Claim(std::int32_t a_key, std::int32_t a_modifiers, const std::string& a_event);
	bool         Release(std::int32_t a_key, std::int32_t a_modifiers, const std::string& a_event);
	std::string  Owner(std::int32_t a_key, std::int32_t a_modifiers);

	// Puts the input sink in front of the others. Called at kInputLoaded - the
	// input device manager is null before it - and again at kDataLoaded in case
	// the first call found nothing. Idempotent.
	void TryInstall();

	// Drops every claim when a save is loaded or a new game starts.
	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg);

	// Registers this module's native functions with the Papyrus VM.
	// Called by Lodestone::Core::Papyrus::Register - never called directly.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
