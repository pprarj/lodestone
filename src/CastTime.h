// CastTime.h
// Intelligence Matters - SKSE plugin
//
// Module: CastTime
// Eventually replaces external plugin as the source of dynamic cast time (Stage C.1).
//
// ############################################################################
// #  PHASE 16 - STAGE C.1 - PART 1.5 - TRACE-ONLY BUILD                      #
// #                                                                          #
// #  THIS BUILD CHANGES NOTHING IN GAME. It installs a hook that calls the   #
// #  original function and then WRITES A LOG LINE. It does not touch the     #
// #  charge time, does not read the IM_CT_* globals, and registers no        #
// #  Papyrus natives. external plugin stays installed and keeps driving cast time.      #
// #                                                                          #
// #  Its only purpose is to turn the five inferences left open by the        #
// #  Part 1 investigation into facts, before Part 2 writes real code:        #
// #                                                                          #
// #    1. Does SetCastingTimerForCharge actually fire on a normal charge,    #
// #       and does castingTimer hold the vanilla value after the original?   #
// #    2. Does it fire for pure concentration spells (vanilla == 0)?         #
// #    3. Does it fire for concentration + charge time spells (an overhaul mod)?     #
// #    4. Does castingTimer already include dual cast and perks?             #
// #    5. Is StartChargeImpl (vfunc 0x04) a concurrent path to the timer?    #
// #                                                                          #
// #  Everything logged here is discarded in Part 2. Nothing in this file is  #
// #  a commitment to a design.                                               #
// ############################################################################
//
// This module registers NO Papyrus natives, so unlike PluginInfo it does NOT
// plug into the Papyrus.cpp dispatcher. It hooks the engine directly and is
// installed from plugin.cpp on kDataLoaded.
//
// Phase 16 - Stage C.1 / Part 1.5

#pragma once

namespace IMPlugin::CastTime
{
	// Installs the trace hooks by swapping two entries of the ActorMagicCaster
	// vtable. Idempotent-unsafe: must be called exactly once.
	//
	// Call site: plugin.cpp, on SKSE::MessagingInterface::kDataLoaded.
	//
	// A vtable swap needs no address library ID and no trampoline - the VTABLE
	// constant in CommonLibSSE-NG already carries the SE / AE / VR variants.
	//
	// Never throws. Failures are logged and swallowed; a trace build must not be
	// able to take the game down.
	void Install();
}
