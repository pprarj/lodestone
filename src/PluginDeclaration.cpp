// PluginDeclaration - the SKSE version data this DLL exports.
//
// WHY THIS IS HAND-WRITTEN. add_commonlibsse_plugin generates this declaration
// automatically, and it did until runtime 1.7.104. The generator hardcodes
// StructCompatibility::Independent (cmake/CommonLibSSE.cmake:106) and offers no
// way to change it, which on 1.7.x makes SKSE refuse to load the plugin with
// "must be recompiled for new address library". Declaring it here is the only
// place the required value can be expressed, so the generated translation unit
// is dropped from the target in CMakeLists.txt - two definitions of
// SKSEPlugin_Version would not link.
//
// WHAT SKSE ACTUALLY READS. SKSEPluginInfo exports a PluginDeclaration object
// (SKSE/Interfaces.h:703) and SKSE reads those same bytes as a
// PluginVersionData. The two are both 0x350 bytes and their fields line up by
// offset, so StructCompatibility sits exactly where PluginVersionData keeps
// versionIndependenceEx. That field is a BIT FIELD, not an enum:
//
//     bit 0  kVersionIndependentEx_NoStructUse      == StructCompatibility::Independent
//     bit 1  kVersionIndependentEx_AddressLibraryV5
//
// Runtime 1.7.x stores the Address Library database in format 5 and SKSE 2.3.1
// requires bit 1 from every plugin that resolves addresses through it. CommonLib
// v6.7.1's StructCompatibility enum has no name for that bit - it predates the
// format - so the value below is composed from the named constants of both
// sides rather than written as a literal 3.
//
// Measured, 2026-10-18. This exact binary reached the main menu on BOTH
// runtimes with the bit set: 1.7.104 under SKSE 2.3.1, and 1.6.1170 under SKSE
// 2.2.6, which predates format 5 entirely. The older loader IGNORES a bit it
// does not know rather than refusing the plugin, and every hook came up on both.
// That is what lets one DLL cover both runtimes instead of shipping two.
//
// The bit does not pin a plugin to a runtime - compatibleVersions does that,
// independently of these flags. A plugin that lists an explicit runtime there
// needs one DLL per runtime no matter what it sets here. This one lists nothing
// and resolves every address through the Address Library, so it has no pin.
//
// Whether the hardcoded vtable indices still land on the same functions is a
// separate question, and it was measured the same day: all 14 slots this
// plugin writes resolve to the same Address Library id on both runtimes. Three
// instruments agreed - a throwaway probe that read every slot before any hook
// installed, run once per runtime, and a reading of both SkyrimSE.exe straight
// off disk, where no other plugin's hook can sit in a slot and hide the
// engine's own function.

#include "REL/Relocation.h"
#include "SKSE/SKSE.h"

#include "Version.h"

namespace
{
	// ISOLATED, 2026-10-18. 1.33.17 changed three things at once - this bit, the
	// plugin version, and the move off the generated declaration - so the bit was
	// credited on a hypothesis rather than measured. 1.33.18 kept the hand-written
	// declaration and took ONLY the bit back out:
	//
	//     plugin Lodestone.dll (00000001 Lodestone 01210120)
	//         must be recompiled for new address library 0 (handle 0)
	//
	// The bit is the cause. The declaration move is only the vehicle, because the
	// generator cannot express this value.
	inline constexpr auto kStructCompatibility = static_cast<SKSE::StructCompatibility>(
		static_cast<std::uint32_t>(SKSE::StructCompatibility::Independent) |
		static_cast<std::uint32_t>(SKSE::PluginVersionData::kVersionIndependentEx_AddressLibraryV5));
}

// The version numbers come from Version.h so that project(... VERSION ...) in
// CMakeLists.txt stays the single source of truth, exactly as it was while the
// generator owned this declaration.
SKSEPluginInfo(
	.Version = REL::Version{
		Lodestone::Version::kMajor,
		Lodestone::Version::kMinor,
		Lodestone::Version::kPatch,
		0 },
	.Name = Lodestone::Version::kProjectName,
	.Author = "Awrawra"sv,
	.SupportEmail = ""sv,
	.StructCompatibility = kStructCompatibility,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary,
	.MinimumSKSEVersion = REL::Version{ 0, 0, 0, 0 })
