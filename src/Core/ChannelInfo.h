// ChannelInfo.h
// Lodestone - Shared SKSE framework
//
// Module: ChannelInfo (Core)
// Owns the read-only Papyrus view over every registration channel in the plugin.
//
// WHY THIS IS ITS OWN MODULE AND NOT PART OF PluginInfo. It was the obvious
// place - PluginInfo is already the plugin's self-describing API, and these two
// functions describe the plugin. The cost is what decided it: answering them
// means naming every channel-owning module, so PluginInfo would have to include
// CastTime.h, MagicScaling.h and Detection.h, and would gain an include for
// every channel added afterwards. PluginInfo reads two compile-time constants
// and depends on nothing; that is worth keeping. A module whose job is exactly
// "know where all the channels are" pays that cost once, in the one file that
// cannot avoid it.
//
// WHAT THESE ARE FOR. Multi-contributor composition is invisible from Papyrus:
// a consumer registers, gets True, and cannot tell whether it is alone or one of
// three. When a user reports numbers that do not match a mod's own maths, the
// first question is how many mods are driving that channel and which ones, and
// without these two natives the only answer is a DLL log the user has to be
// talked through finding. These make it a Papyrus call any consumer can put
// behind its own MCM or debug command.
//
// NOT A CONTROL SURFACE. Read-only, by design. A consumer can see that another
// plugin contributes; it cannot remove it, outrank it or refuse to compose with
// it. Which mod wins is not a question this framework answers - all of them do,
// by composition - and handing one consumer a way to evict another would put
// exactly the arbitration this version removed back in, with worse manners.
//
// Version gate for consumers: Lodestone.GetVersion() >= 1009000 (1.9.0).
//
// Phase L0 (multi-contributor)

#pragma once

namespace Lodestone::Core::ChannelInfo
{
	// Registers this module's natives on the "Lodestone" script.
	// Registers: GetChannelContributorCount, GetChannelContributorPlugin.
	//
	// Returns false if any registration failed.
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);
}
