// PerkPoints.cpp
// Lodestone - Shared SKSE framework
//
// Natives behind Lodestone.GetPerkPoints, HasPerk, IsPerkEligible, TryBuyPerk and
// the GetPerkTree* batch readers.
//
// POINTS. The count is PlayerCharacter::GetGameStatsData().perkCount, the same
// byte Game.AddPerkPoints moves and the perk menu reads. Papyrus has no reader
// for it. A count of 0 is a valid answer; -1 means the player was unavailable.
//
// ELIGIBLE. True only when the vanilla menu would let the player buy the perk:
// not None, not already owned, at least one point, and the perk's own conditions
// (perkConditions, where the CK stores the prerequisites) pass with the player as
// both subject and target. That the menu evaluates exactly this is INFERRED and
// is measured by the consumer's probe.
//
// BUY. One native call re-checks everything, then AddPerk and the debit happen
// back to back on the main thread. The debit only follows an AddPerk that is
// confirmed by HasPerk, so a refused buy never spends a point. There is no
// RemovePerk and no refund anywhere in this module.
//
// TREE. Papyrus has no Perk[][], so a tree comes back as parallel arrays over one
// fixed order (breadth first from the tree root, visiting each node once, nodes
// without a perk skipped) plus a flat edge list of index pairs. The order is the
// same for every GetPerkTree* call on the same skill, so index i means the same
// perk in all of them.
//
// Phase L-P1

#include "PerkPoints.h"

#include <unordered_map>
#include <unordered_set>

namespace Lodestone::Core::PerkPoints
{
	namespace
	{
		constexpr std::int32_t kNoPoints = -1;

		RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		std::int32_t ReadPoints()
		{
			auto* player = Player();
			if (!player) {
				return kNoPoints;
			}
			return static_cast<std::int32_t>(player->GetGameStatsData().perkCount);
		}

		bool ConditionsPass(RE::PlayerCharacter* a_player, RE::BGSPerk* a_perk)
		{
			// An empty condition list has nothing to fail.
			if (!a_perk->perkConditions.head) {
				return true;
			}
			return a_perk->perkConditions.IsTrue(a_player, a_player);
		}

		bool Eligible(RE::PlayerCharacter* a_player, RE::BGSPerk* a_perk)
		{
			if (!a_player || !a_perk) {
				return false;
			}
			if (a_player->HasPerk(a_perk)) {
				return false;
			}
			if (a_player->GetGameStatsData().perkCount == 0) {
				return false;
			}
			return ConditionsPass(a_player, a_perk);
		}

		// The nodes of a skill's tree, in the fixed order every reader shares.
		// Empty when the skill name is unknown or the skill has no tree.
		std::vector<RE::BGSSkillPerkTreeNode*> TreeNodes(const std::string& a_skill)
		{
			std::vector<RE::BGSSkillPerkTreeNode*> out;
			if (a_skill.empty()) {
				return out;
			}

			const RE::ActorValue av = RE::ActorValueList::LookupActorValueByName(a_skill.c_str());
			if (av == RE::ActorValue::kNone) {
				return out;
			}
			const RE::ActorValueInfo* info = RE::ActorValueList::GetActorValueInfo(av);
			if (!info || !info->perkTree) {
				return out;
			}

			std::vector<RE::BGSSkillPerkTreeNode*> queue{ info->perkTree };
			std::unordered_set<RE::BGSSkillPerkTreeNode*> seen{ info->perkTree };
			for (std::size_t head = 0; head < queue.size(); ++head) {
				RE::BGSSkillPerkTreeNode* node = queue[head];
				if (node->perk) {
					out.push_back(node);
				}
				for (auto* child : node->children) {
					if (child && seen.insert(child).second) {
						queue.push_back(child);
					}
				}
			}
			return out;
		}

		// Lodestone.GetPerkPoints() -> Int
		std::int32_t GetPerkPoints(RE::StaticFunctionTag*)
		{
			try {
				return ReadPoints();
			} catch (...) {
				return kNoPoints;
			}
		}

		// Lodestone.HasPerk(Perk) -> Bool
		bool HasPerk(RE::StaticFunctionTag*, RE::BGSPerk* a_perk)
		{
			try {
				auto* player = Player();
				return player && a_perk && player->HasPerk(a_perk);
			} catch (...) {
				return false;
			}
		}

		// Lodestone.IsPerkEligible(Perk) -> Bool
		bool IsPerkEligible(RE::StaticFunctionTag*, RE::BGSPerk* a_perk)
		{
			try {
				return Eligible(Player(), a_perk);
			} catch (...) {
				return false;
			}
		}

		// Lodestone.TryBuyPerk(Perk) -> Bool
		bool TryBuyPerk(RE::StaticFunctionTag*, RE::BGSPerk* a_perk)
		{
			try {
				auto* player = Player();
				if (!Eligible(player, a_perk)) {
					return false;
				}

				player->AddPerk(a_perk);
				// Debit only a purchase the engine confirms.
				if (!player->HasPerk(a_perk)) {
					spdlog::warn("PerkPoints: AddPerk did not take for {:08X}; no point spent.", a_perk->GetFormID());
					return false;
				}

				auto& stats = player->GetGameStatsData();
				if (stats.perkCount > 0) {
					--stats.perkCount;
				}
				return true;
			} catch (...) {
				return false;
			}
		}

		// Lodestone.GetPerkTreePerks(String) -> Perk[]
		std::vector<RE::BGSPerk*> GetPerkTreePerks(RE::StaticFunctionTag*, RE::BSFixedString a_skill)
		{
			try {
				std::vector<RE::BGSPerk*> out;
				for (auto* node : TreeNodes(a_skill.c_str() ? a_skill.c_str() : "")) {
					out.push_back(node->perk);
				}
				return out;
			} catch (...) {
				return {};
			}
		}

		std::vector<std::int32_t> GridAxis(const char* a_skill, bool a_x)
		{
			try {
				std::vector<std::int32_t> out;
				for (auto* node : TreeNodes(a_skill ? a_skill : "")) {
					out.push_back(static_cast<std::int32_t>(a_x ? node->perkGridX : node->perkGridY));
				}
				return out;
			} catch (...) {
				return {};
			}
		}

		// Lodestone.GetPerkTreeGridX(String) -> Int[]
		std::vector<std::int32_t> GetPerkTreeGridX(RE::StaticFunctionTag*, RE::BSFixedString a_skill)
		{
			return GridAxis(a_skill.c_str(), true);
		}

		// Lodestone.GetPerkTreeGridY(String) -> Int[]
		std::vector<std::int32_t> GetPerkTreeGridY(RE::StaticFunctionTag*, RE::BSFixedString a_skill)
		{
			return GridAxis(a_skill.c_str(), false);
		}

		// Lodestone.GetPerkTreeNames(String) -> String[]
		std::vector<std::string> GetPerkTreeNames(RE::StaticFunctionTag*, RE::BSFixedString a_skill)
		{
			try {
				std::vector<std::string> out;
				for (auto* node : TreeNodes(a_skill.c_str() ? a_skill.c_str() : "")) {
					const char* name = node->perk->GetName();
					out.emplace_back(name ? name : "");
				}
				return out;
			} catch (...) {
				return {};
			}
		}

		// Lodestone.GetPerkTreeEdges(String) -> Int[]
		// Flat pairs [parent0, child0, parent1, child1, ...], indices into the
		// GetPerkTreePerks order. Edges to or from the perkless root are dropped.
		std::vector<std::int32_t> GetPerkTreeEdges(RE::StaticFunctionTag*, RE::BSFixedString a_skill)
		{
			try {
				const auto nodes = TreeNodes(a_skill.c_str() ? a_skill.c_str() : "");
				std::unordered_map<RE::BGSSkillPerkTreeNode*, std::int32_t> index;
				for (std::size_t i = 0; i < nodes.size(); ++i) {
					index[nodes[i]] = static_cast<std::int32_t>(i);
				}

				std::vector<std::int32_t> out;
				for (std::size_t i = 0; i < nodes.size(); ++i) {
					for (auto* child : nodes[i]->children) {
						const auto it = index.find(child);
						if (it != index.end()) {
							out.push_back(static_cast<std::int32_t>(i));
							out.push_back(it->second);
						}
					}
				}
				return out;
			} catch (...) {
				return {};
			}
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("PerkPoints: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("GetPerkPoints", "Lodestone", GetPerkPoints);
		a_vm->RegisterFunction("HasPerk", "Lodestone", HasPerk);
		a_vm->RegisterFunction("IsPerkEligible", "Lodestone", IsPerkEligible);
		a_vm->RegisterFunction("TryBuyPerk", "Lodestone", TryBuyPerk);
		a_vm->RegisterFunction("GetPerkTreePerks", "Lodestone", GetPerkTreePerks);
		a_vm->RegisterFunction("GetPerkTreeGridX", "Lodestone", GetPerkTreeGridX);
		a_vm->RegisterFunction("GetPerkTreeGridY", "Lodestone", GetPerkTreeGridY);
		a_vm->RegisterFunction("GetPerkTreeNames", "Lodestone", GetPerkTreeNames);
		a_vm->RegisterFunction("GetPerkTreeEdges", "Lodestone", GetPerkTreeEdges);

		spdlog::info("PerkPoints: natives registered (GetPerkPoints, HasPerk, IsPerkEligible, TryBuyPerk, GetPerkTree*).");
		return true;
	}
}
