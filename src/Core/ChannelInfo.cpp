// ChannelInfo.cpp
// Lodestone - Shared SKSE framework
//
// Native implementations of the channel diagnostics. See ChannelInfo.h for why
// this is a module of its own and what these functions are for.
//
// Phase L0 (multi-contributor)

#include "ChannelInfo.h"

#include "CastTime.h"
#include "Detection.h"
#include "MagicScaling.h"
#include "MultiChannel.h"

namespace Lodestone::Core::ChannelInfo
{
	namespace
	{
		// The channel table.
		//
		// Built from the channels themselves, and matched by MultiChannel::Name()
		// rather than against a list of string literals kept here. A second copy
		// of "MagicMagnitude" in this file is a copy that can drift from the one
		// the module logs under, and the two disagreeing would show up as a
		// diagnostic that quietly answers -1 for a channel that exists.
		//
		// Built on first use rather than at static init: these are references to
		// objects in four other translation units, and the order in which those
		// are constructed is not something to depend on.
		//
		// ADDING A CHANNEL: one line here, plus the name in the psc comment that
		// lists the valid values. Nothing else in this file changes.
		const std::vector<MultiChannel*>& Channels()
		{
			static const std::vector<MultiChannel*> channels{
				&CastTime::GetChannel(),
				&MagicScaling::GetMagnitudeChannel(),
				&MagicScaling::GetDurationChannel(),
				&MagicScaling::GetCostChannel(),
				&Detection::GetChannel(),
			};
			return channels;
		}

		// Null when the name is not one of the known channels. The caller logs -
		// what it says depends on which sentinel it has to return.
		MultiChannel* Find(const RE::BSFixedString& a_channel)
		{
			const std::string_view wanted{ a_channel.c_str() ? a_channel.c_str() : "" };
			if (wanted.empty()) {
				return nullptr;
			}

			for (auto* channel : Channels()) {
				if (wanted == channel->Name()) {
					return channel;
				}
			}

			return nullptr;
		}

		// Lodestone.GetChannelContributorCount(String) -> Int
		//
		// How many distinct plugins currently contribute to the named channel.
		// Zero is a real answer: the channel exists and nobody has registered.
		//
		// Returns -1 on an unknown channel name (the Int sentinel from the Stage
		// B.3 convention). The Papyrus side cannot tell that apart from the DLL
		// being absent, which yields the VM default 0 - and does not need to:
		// both mean "do not trust this number", and GetVersion() is the call that
		// answers presence.
		std::int32_t GetChannelContributorCount(RE::StaticFunctionTag*, RE::BSFixedString a_channel)
		{
			auto* channel = Find(a_channel);
			if (!channel) {
				spdlog::warn("ChannelInfo: GetChannelContributorCount asked about an unknown channel '{}' - "
							 "returning -1. Valid names are the channel names in Lodestone.psc.",
					a_channel.c_str() ? a_channel.c_str() : "");
				return -1;
			}

			return static_cast<std::int32_t>(channel->ContributorCount());
		}

		// Lodestone.GetChannelContributorPlugin(String, Int) -> String
		//
		// The source plugin filename of the contributor at a_index, 0-based.
		// Order is registration order, which is load-order dependent and is not
		// promised to be stable - a caller walks the whole range and reads the
		// set, it does not index into a position it saw earlier.
		//
		// Returns "" for an unknown channel name or an out-of-range index (the
		// String sentinel). Out of range is not logged: walking until the empty
		// string is a reasonable way to call this, and a warning per miss would
		// turn that into log noise.
		RE::BSFixedString GetChannelContributorPlugin(RE::StaticFunctionTag*, RE::BSFixedString a_channel, std::int32_t a_index)
		{
			auto* channel = Find(a_channel);
			if (!channel) {
				spdlog::warn("ChannelInfo: GetChannelContributorPlugin asked about an unknown channel '{}' - "
							 "returning \"\". Valid names are the channel names in Lodestone.psc.",
					a_channel.c_str() ? a_channel.c_str() : "");
				return RE::BSFixedString("");
			}

			if (a_index < 0) {
				return RE::BSFixedString("");
			}

			const std::string plugin = channel->ContributorPlugin(static_cast<std::size_t>(a_index));
			return RE::BSFixedString(plugin.c_str());
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("ChannelInfo: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("GetChannelContributorCount", "Lodestone", GetChannelContributorCount);
		a_vm->RegisterFunction("GetChannelContributorPlugin", "Lodestone", GetChannelContributorPlugin);

		spdlog::info("ChannelInfo: natives registered (GetChannelContributorCount, GetChannelContributorPlugin) "
					 "over {} channels.",
			Channels().size());
		return true;
	}
}
