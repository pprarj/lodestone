// MultiChannel.h
// Lodestone - Shared SKSE framework
//
// One registration channel shared by N contributors.
//
// WHAT A CHANNEL IS. A consumer owns two GlobalVariable records - a multiplier
// and an offset - drives them from its own Papyrus state, and hands them to
// Lodestone through a native. A module then applies them where the engine has
// finished computing the quantity in question: value = value * mult + offset.
// That is the shape CastTime and the three MagicScaling channels have always
// had, and it is unchanged.
//
// WHAT CHANGED IN 1.9.0. Until 1.8.2 a channel had exactly one owner: the first
// valid registration won it for the session, and a second registrant with a
// DIFFERENT pair was warned in the log and rejected. That policy is fine while
// exactly one mod uses a channel, and it fails the moment two do - the loser
// dies quietly (a log line most users never read) and its author gets a bug
// report with no visible cause. Lodestone is a public framework, so two
// registrants on one channel is a normal case, not an edge one.
//
// From 1.9.0 a channel accepts every registrant and COMPOSES them:
//
//   mult_total   = product of every registered multiplier
//   offset_total = sum of every registered offset
//   result       = value * mult_total + offset_total
//
// applied in a single pass, not chained. With one contributor - every consumer
// shipping today - the arithmetic is identical to what the single-channel code
// did, exactly (see Read).
//
// COMPOSING IS CORE WORK, NOT POLICY LEAKING IN. "The framework provides
// capability, Papyrus decides policy" still holds: HOW MUCH each consumer asks
// for is its own globals' business, and no consumer can see the others. Only
// Core can see all of them, so combining them is precisely the job nothing else
// is in a position to do.
//
// CONTRIBUTOR IDENTITY IS DERIVED, NOT PASSED. The key is the filename of the
// plugin the multiplier global comes from (TESForm::GetFile(0)). Consumers do
// not pass it and did not have to be recompiled: a mod written against 1.1.0
// keeps calling the same native with the same two arguments and is now a
// contributor rather than an owner. One contributor per source plugin - see
// Register for what a second pair from the same plugin does.
//
// Phase L0 (multi-contributor)

#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Lodestone::Core
{
	class MultiChannel
	{
	public:
		// a_name is the channel's canonical name - the same string the
		// diagnostic natives take ("CastTime", "MagicMagnitude", ...). It is
		// stored by pointer and must be a literal with static lifetime. Kept in
		// one place, on the channel itself, so the diagnostics can match against
		// Name() instead of repeating the literals in a second table that would
		// drift.
		explicit MultiChannel(const char* a_name) noexcept;

		MultiChannel(const MultiChannel&)            = delete;
		MultiChannel& operator=(const MultiChannel&) = delete;

		[[nodiscard]] const char* Name() const noexcept { return _name; }

		// Adds (or refreshes) the caller's pair. Returns true when the CALLER's
		// pair is contributing to the channel after this call - which now
		// includes the case where other plugins are contributing too. Returns
		// false only on a null argument.
		//
		// Never rejects a registrant. Never throws.
		bool Register(RE::TESGlobal* a_multiplier, RE::TESGlobal* a_offset);

		// Drops the contributor registered under a_key. Returns true when one
		// was found and removed.
		//
		// NO CALLER TODAY. Nothing in the Papyrus API exposes this yet, and this
		// is a deliberate exception to the "do not leave code lying around in
		// advance of a need" rule in CONVENTIONS: removal is the other half of a
		// registry, it is four lines against the same snapshot discipline
		// Register already needs, and inventing it under pressure the day a
		// consumer wants to hand a channel back is how the discipline gets got
		// wrong. It is a working function, not a no-op placeholder.
		bool Unregister(std::string_view a_key);

		// Composes every live contributor and hands back the totals. Returns
		// false when nobody has registered, which is what keeps every module
		// passthrough until a consumer arrives - the same contract, and the same
		// call shape, the single-channel Read had.
		//
		// Lock-free: one acquire load and a walk over an immutable snapshot. Safe
		// to call from a hook on the game thread.
		bool Read(float& a_multiplier, float& a_offset) const;

		// True when at least one contributor is registered - one acquire load and
		// a compare, with no walk and no float reads.
		//
		// This exists for hooks that fire far more often than they act. The two
		// loose atomics this class replaced let a thunk ask "is anyone driving
		// this?" for the price of a pointer compare, and put the value reads down
		// at the formula, past every filter. Composing is cheap but it is not
		// free, and a hook measured at 316 events a minute to reach the formula 4
		// times should not pay for it 312 times.
		[[nodiscard]] bool HasContributors() const;

		// Diagnostics, behind Lodestone.GetChannelContributorCount and
		// Lodestone.GetChannelContributorPlugin.
		[[nodiscard]] std::size_t ContributorCount() const;

		// Source plugin filename of the contributor at a_index, or "" when the
		// index is out of range. Order is registration order and is not promised
		// to be stable.
		[[nodiscard]] std::string ContributorPlugin(std::size_t a_index) const;

	private:
		struct Contributor
		{
			std::string    key;
			RE::TESGlobal* multiplier{ nullptr };
			RE::TESGlobal* offset{ nullptr };
		};

		// The live set is an IMMUTABLE vector published by pointer. A registrant
		// copies it, edits the copy under _lock, and stores the new one with a
		// release. A reader takes one acquire load and walks what it got, so the
		// hot path never takes a lock and never sees a half-built set.
		using Snapshot = std::vector<Contributor>;

		// Publishes a_next as the live set and keeps the superseded one alive
		// forever. Call under _lock. See the definition for why nothing is
		// freed.
		void Publish(Snapshot* a_next);

		// Source plugin filename of the plugin a_global comes from, or a
		// FormID-based fallback. Never returns empty.
		[[nodiscard]] static std::string DeriveKey(RE::TESGlobal* a_global);

		const char*                  _name;
		std::atomic<const Snapshot*> _live{ nullptr };
		mutable std::mutex           _lock;

		// Superseded snapshots, kept reachable rather than deleted - see
		// Publish. Only ever touched under _lock.
		std::vector<const Snapshot*> _retired;
	};
}
