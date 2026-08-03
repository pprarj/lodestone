// MultiChannel.cpp
// Lodestone - Shared SKSE framework
//
// Implementation of the shared multi-contributor channel. See MultiChannel.h
// for the contract and for why the single-owner policy was retired.
//
// A .cpp WITH A HEADER, and not the header-only template the two existing
// channel modules might suggest. CastTime and MagicScaling each keep their state
// in an anonymous namespace inside one .cpp because nothing outside those files
// ever needed to name it. This type is the opposite case: four modules hold one,
// and the diagnostics reach all of them through the getters those modules
// expose, so the type has to be nameable from five translation units. Making it
// a template to keep it header-only would buy nothing - there is exactly one
// instantiation, and the code would be recompiled into every one of those units.
//
// Phase L0 (multi-contributor)

#include "MultiChannel.h"

#include <format>
#include <memory>

namespace Lodestone::Core
{
	namespace
	{
		// Not a limit. Registration is never refused - see D3 in the design
		// notes and the header. This is the count at which a channel starts
		// saying that something looks wrong, because a healthy modlist does not
		// have seventeen mods driving one quantity.
		//
		// The failure this actually catches is a bad KEY, not a crowd. One
		// contributor per source plugin is enforced below, so honest registrants
		// cannot pile up: sixteen entries means sixteen distinct plugin names,
		// or - far more likely - a global whose GetFile(0) keeps coming back
		// null, sending every registration down the FormID fallback and minting
		// a fresh key each time.
		constexpr std::size_t kContributorWarnThreshold = 16;
	}

	MultiChannel::MultiChannel(const char* a_name) noexcept :
		_name(a_name)
	{}

	std::string MultiChannel::DeriveKey(RE::TESGlobal* a_global)
	{
		// GetFile(0) is the FIRST file in the form's source chain - the plugin
		// that created the record, not whichever one edited it last. That is the
		// identity wanted here: a consumer's global stays keyed to the consumer
		// even when a patch in the load order overrides its value.
		if (a_global) {
			if (const auto* file = a_global->GetFile(0)) {
				const auto filename = file->GetFilename();
				if (!filename.empty()) {
					return std::string(filename);
				}
			}
		}

		// ACHADO - none, and that is the point: this path has never been
		// observed. A GlobalVariable handed in from Papyrus came out of a loaded
		// plugin, so it has a source file. The fallback exists because "should
		// not happen" is not a thing to dereference on, and because the two
		// alternatives are both worse than a synthetic key: rejecting would
		// resurrect exactly the silent failure this whole change removes, and
		// returning an empty key would make every such global collide with every
		// other one, so the second registrant would overwrite the first.
		//
		// Keying by FormID instead keeps each registrant separate and composing.
		// The cost is that identity is no longer per-plugin, so the same
		// consumer re-registering a DIFFERENT global would add a contributor
		// rather than replace one - which is what the threshold warning above is
		// there to make visible.
		const auto formID = a_global ? a_global->GetFormID() : 0;
		spdlog::warn("MultiChannel: a registered global (0x{:08X}) reports no source file, so its contributor "
					 "key falls back to its FormID. This is not expected for a global that came from a loaded "
					 "plugin - see DeriveKey.",
			formID);

		return std::format("<form:0x{:08X}>", formID);
	}

	void MultiChannel::Publish(Snapshot* a_next)
	{
		const Snapshot* previous = _live.load(std::memory_order_relaxed);

		// Release, so a reader that observes this pointer also observes every
		// write that built the vector behind it.
		_live.store(a_next, std::memory_order_release);

		// THE SUPERSEDED SNAPSHOT IS NEVER FREED. A reader takes no lock, so at
		// the instant of the store another thread may be part way through
		// walking the old vector, and there is no cheap way to learn when it is
		// done. Freeing it correctly would mean epochs or hazard pointers - real
		// machinery, in a hot path, to reclaim a few dozen bytes.
		//
		// So it is kept instead, and kept REACHABLE rather than leaked outright,
		// so a leak checker reports the truth about this and does not train
		// anyone to ignore it. The bound is the number of registrations in a
		// session, which is a handful: consumers register once at startup and
		// once per load.
		if (previous) {
			_retired.push_back(previous);
		}
	}

	bool MultiChannel::Register(RE::TESGlobal* a_multiplier, RE::TESGlobal* a_offset)
	{
		if (!a_multiplier || !a_offset) {
			spdlog::warn("Channel '{}': registration got a null global (multiplier={}, offset={}) - ignored.",
				_name,
				a_multiplier ? "ok" : "NULL",
				a_offset ? "ok" : "NULL");
			return false;
		}

		const std::string key = DeriveKey(a_multiplier);

		std::lock_guard<std::mutex> lock(_lock);

		const Snapshot* live = _live.load(std::memory_order_acquire);
		auto            next = live ? std::make_unique<Snapshot>(*live) : std::make_unique<Snapshot>();

		for (auto& contributor : *next) {
			if (contributor.key != key) {
				continue;
			}

			if (contributor.multiplier == a_multiplier && contributor.offset == a_offset) {
				// The refresh-on-load case every consumer already does. Same
				// plugin, same pair, no change - and no new snapshot, so a
				// consumer re-registering on every load allocates nothing.
				spdlog::debug("Channel '{}': '{}' re-registered the same pair - no-op.", _name, key);
				return true;
			}

			// Same plugin, a DIFFERENT pair. One contributor per source plugin,
			// so this replaces rather than adds: a plugin driving a channel from
			// two pairs at once would compose with itself, which is a
			// configuration nothing asks for and one that a consumer swapping
			// its globals mid-session would fall into silently.
			spdlog::info("Channel '{}': '{}' replaced its pair (multiplier=0x{:08X}, offset=0x{:08X}; was "
						 "multiplier=0x{:08X}, offset=0x{:08X}). One contributor per source plugin.",
				_name, key,
				a_multiplier->GetFormID(), a_offset->GetFormID(),
				contributor.multiplier->GetFormID(), contributor.offset->GetFormID());

			contributor.multiplier = a_multiplier;
			contributor.offset     = a_offset;

			Publish(next.release());
			return true;
		}

		next->push_back(Contributor{ key, a_multiplier, a_offset });

		const auto count = next->size();
		Publish(next.release());

		spdlog::info("Channel '{}': contributor '{}' registered (multiplier=0x{:08X}, offset=0x{:08X}) - "
					 "{} contributor(s) now composing.",
			_name, key,
			a_multiplier->GetFormID(), a_offset->GetFormID(),
			count);

		if (count > kContributorWarnThreshold) {
			spdlog::warn("Channel '{}': {} contributors is more than any modlist should produce ({} is the point "
						 "at which this line appears). Nothing has been rejected and the channel still composes "
						 "all of them - but check Lodestone.GetChannelContributorPlugin for repeated or "
						 "FormID-shaped keys, which is what a registration loop or a bad contributor key looks "
						 "like from here.",
				_name, count, kContributorWarnThreshold);
		}

		return true;
	}

	bool MultiChannel::Unregister(std::string_view a_key)
	{
		std::lock_guard<std::mutex> lock(_lock);

		const Snapshot* live = _live.load(std::memory_order_acquire);
		if (!live) {
			return false;
		}

		auto next = std::make_unique<Snapshot>();
		next->reserve(live->size());

		bool removed = false;
		for (const auto& contributor : *live) {
			if (!removed && contributor.key == a_key) {
				removed = true;
				continue;
			}
			next->push_back(contributor);
		}

		if (!removed) {
			return false;
		}

		const auto count = next->size();
		Publish(next.release());

		spdlog::info("Channel '{}': contributor '{}' removed - {} contributor(s) left composing.",
			_name, a_key, count);
		return true;
	}

	bool MultiChannel::Read(float& a_multiplier, float& a_offset) const
	{
		const Snapshot* live = _live.load(std::memory_order_acquire);
		if (!live || live->empty()) {
			return false;
		}

		// Read fresh on every call, exactly as the single-channel version did:
		// the globals are the truth at the moment they are asked for, and a
		// consumer that moves one mid-session expects the next cast to see it.
		//
		// IDENTICAL RESULT FOR ONE CONTRIBUTOR, and not approximately so. The
		// seeds make the single-contributor case `1.0f * m` and `0.0f + o`, and
		// both are exact in IEEE 754 for every finite value - so a consumer
		// alone in the load order (which is every consumer shipping today)
		// computes the same float it computed before this change, bit for bit.
		// The one exception is an offset of -0.0, which comes back as +0.0 and
		// compares equal to it.
		float mult   = 1.0f;
		float offset = 0.0f;

		for (const auto& contributor : *live) {
			mult *= contributor.multiplier->value;
			offset += contributor.offset->value;
		}

		a_multiplier = mult;
		a_offset     = offset;
		return true;
	}

	bool MultiChannel::HasContributors() const
	{
		const Snapshot* live = _live.load(std::memory_order_acquire);
		return live && !live->empty();
	}

	std::size_t MultiChannel::ContributorCount() const
	{
		const Snapshot* live = _live.load(std::memory_order_acquire);
		return live ? live->size() : 0;
	}

	std::string MultiChannel::ContributorPlugin(std::size_t a_index) const
	{
		const Snapshot* live = _live.load(std::memory_order_acquire);
		if (!live || a_index >= live->size()) {
			return {};
		}

		// A copy, deliberately. The snapshot outlives every reader, but the
		// caller is about to hand this to the Papyrus VM, and a native returning
		// a view into plugin-owned storage is a lifetime question nobody should
		// have to answer twice.
		return (*live)[a_index].key;
	}
}
