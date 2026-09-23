// ---------------------------------------------------------------------------
// Lodestone - Shared SKSE framework
//
// Phase 16 - Stage A: validation plugin (load log).
// Phase 16 - Stage B: lean entry point. plugin.cpp implements nothing itself -
//   it only orchestrates the load:
//     1. Bring up the log        -> Log.cpp
//     2. Initialize SKSE         -> required before requesting any interface
//     3. Register Papyrus natives -> Papyrus.cpp (dispatcher)
//
//   No IM functionality goes here - that's Stage C. Every new module plugs
//   into the dispatcher (Papyrus.cpp), NOT into this file.
//
// Phase 16 - Stage C.1: a fourth step appears -
//     4. Register a message listener -> installs engine hooks on kDataLoaded
//
//   This is the one exception to "everything plugs into the dispatcher". The
//   dispatcher is for PAPYRUS NATIVES. Engine hooks are not natives: they have
//   no Papyrus surface and a different lifecycle (the vtable must exist, but
//   the VM need not be up). So hook installation gets its own seam here.
//
//   Phase L0 update: CastTime now uses BOTH seams. It is an engine hook (installed
//   here on kDataLoaded) AND a native provider (RegisterCastTimeChannel, plugged
//   into the dispatcher like any other module). The two are independent: the hook
//   is passthrough until a consumer calls the native to register its channel.
//   C.2 (book text) and C.3 (spell tomes) also expose natives and plug into the
//   dispatcher as Stage B predicted.
//
//   kDataLoaded is the install point for the hook: a vtable swap alone would work
//   earlier, but the vtable must exist, and kDataLoaded is the established, safe
//   seam. The module no longer caches any globals here - they arrive at runtime
//   through the native.
//
// Phase L3 update: four modules now install hooks here, by two mechanisms:
//     CastTime, MagicScaling  -> vtable swap (virtual targets)
//     BookFramework, SpellTomes -> SafetyHook inline hook (non-virtual targets)
//
//   The split is not a preference, it is what the target allows. CommonLibSSE's
//   Trampoline::write_branch is used by none of them: it redirects an existing
//   rel32 call site and cannot detour a function body, which is a distinction
//   that cost a crash and two silently dead modules to learn. See the rule
//   recorded in CMakeLists.
// ---------------------------------------------------------------------------

// RE/Skyrim.h and SKSE/SKSE.h come from PCH.h (force-included by CMake).
#include "Core/BookFramework.h"
#include "Core/CastTime.h"
#include "Core/EquipVeto.h"
#include "Core/Incapacitation.h"
#include "Core/Log.h"
#include "Core/MagicScaling.h"
#include "Core/MenuPrompt.h"
#include "Core/Papyrus.h"
#include "Core/Serialization.h"
#include "Core/SpellTomes.h"
#include "Core/WebUIBridge.h"
#include "Version.h"

namespace
{
	// SKSE message dispatch. Fans out to whichever modules need a lifecycle
	// hook. CastTime swaps a vtable entry; BookFramework installs a branch hook on
	// the book-open function (which is why the trampoline is allocated below).
	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}

		// kPostLoad, and it has to be this seam rather than kDataLoaded.
		//
		// Prisma UI's own header recommends requesting the API at or after
		// kPostLoad, which is when its DLL is guaranteed loaded. Nothing is
		// created here: at kPostLoad the D3D device and the Ultralight renderer
		// do not exist yet, and CreateView at that moment queues forever or
		// blocks the load chain. Only the pointer is taken - views are built
		// when a consumer asks, which is necessarily later. See
		// Core/WebUIBridge.h.
		if (a_msg->type == SKSE::MessagingInterface::kPostLoad) {
			Lodestone::Core::WebUIBridge::Acquire();
		}

		// EVERY message, and not a chosen few, because the bridge has more than
		// one backend and they are acquired differently. Meridian UI answers a
		// two-step SKSE handshake - a version request at kPostPostLoad, an API
		// request at kInputLoaded - and only becomes available if it sees both.
		// The bridge picks the session's backend at kInputLoaded, once.
		//
		// This is the only module in this plugin that needs those two seams, and
		// filtering them here instead of inside the bridge would put knowledge
		// of one vendor's handshake in the wrong file.
		Lodestone::Core::WebUIBridge::HandleSKSEMessage(a_msg);

		// MenuPrompt wants two of these seams and filters them itself, for the
		// same reason: a save being loaded or a new game starting has to close
		// an open prompt and hand the waiting script a cancellation. Which
		// messages those are is the module's business, not this file's.
		Lodestone::Core::MenuPrompt::HandleSKSEMessage(a_msg);

		// Incapacitation stands up, at kPostLoadGame, the actors a save says
		// were knocked down - a load ends every knockdown. Same shape as
		// MenuPrompt: the module picks its message.
		Lodestone::Core::Incapacitation::HandleSKSEMessage(a_msg);

		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Lodestone::Core::CastTime::Install();
			Lodestone::Core::BookFramework::Install();
			Lodestone::Core::SpellTomes::Install();
			Lodestone::Core::MagicScaling::Install();

			// Incapacitation needs this seam for two things at once: a vtable
			// swap like the four above (suppressing the engine's get-up for
			// actors it is holding down) and a TESDeathEvent sink, which is
			// not a hook and is here only because the script event source has
			// to exist by then. Two different requirements, same timing.
			Lodestone::Core::Incapacitation::Install();

			// EquipVeto installs an inline hook on a non-virtual target, so it
			// does not need the vtable to exist - but it reads its configuration
			// through TESDataHandler, which does need data loaded. Same seam,
			// different reason from the four above.
			Lodestone::Core::EquipVeto::Install();

			// MenuPrompt registers its two windows with the menu framework
			// here. Not a hook at all - the seam is shared because the
			// framework's DLL is reliably loaded by this point, which is what
			// its own consumers rely on.
			Lodestone::Core::MenuPrompt::Install();
		}
	}
}

// SKSEPluginLoad is the modern CommonLibSSE-NG entry point. The version
// boilerplate (SKSEPlugin_Version) is auto-generated by add_commonlibsse_plugin.
SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    // Without a log there's no way to diagnose anything - abort the load
    // explicitly instead of continuing blind.
    if (!Lodestone::Core::Log::Init()) {
        SKSE::stl::report_and_fail("SKSE log directory not provided.");
    }

    // a_log = false: with the default (true), CommonLibSSE-NG builds its own
    // logger on top of the one Log::Init() just configured - it truncates the
    // file, replaces the pattern, and drops trace to info in release builds.
    // Confirmed in src/SKSE/API.cpp:97-100 and src/SKSE/Logger.cpp:126-151 of
    // v6.7.1. Do not drop the second argument. Campaign
    // 2026-09-04-skse-init-preserva-o-logger-do-plugin.
    SKSE::Init(a_skse, false);

    spdlog::info("{} v{} loaded successfully.",
        Lodestone::Version::kProjectName, Lodestone::Version::kString);

    // Cosave registration. This is a THIRD seam, distinct from the native
    // dispatcher and from engine-hook installation on kDataLoaded: a save or
    // load can happen well before kDataLoaded fires, so the callbacks have to
    // be wired as early as the serialization interface is available. No
    // other module in this plugin has needed this seam before Incapacitation
    // - see Incapacitation.h for what it persists and why.
    Lodestone::Core::Serialization::Register();

    // NO TRAMPOLINE IS RESERVED HERE ANY MORE.
    //
    // There used to be an AllocTrampoline call, for branch hooks that no module
    // installs today. Every hook in this plugin now uses one of two mechanisms,
    // and neither touches CommonLibSSE's trampoline:
    //
    //   virtual function      -> vtable swap  (CastTime, MagicScaling)
    //   non-virtual function  -> SafetyHook   (BookFramework, SpellTomes)
    //
    // SafetyHook allocates and manages its own trampoline per hook. The reserved
    // block was left over from the branch hooks that were removed once it became
    // clear write_branch only redirects an existing call site and cannot detour a
    // function body. Reserving it changed nothing except to describe an
    // architecture that no longer exists - which is worse than useless in a file
    // whose whole job is to say how the plugin is wired.
    //
    // Anything added later that does need it must call AllocTrampoline before the
    // hook is written; running out is a silent failure to detour.

    // Native function registration. SKSE calls the dispatcher once the
    // Papyrus VM is ready - that happens later during load, not right now.
    if (!SKSE::GetPapyrusInterface()->Register(Lodestone::Core::Papyrus::Register)) {
        spdlog::error("Failed to register the Papyrus callback - natives unavailable.");
        return false;
    }

    spdlog::info("Papyrus callback registered.");

    // Engine hook installation is deferred to kDataLoaded (see header comment).
    if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
        spdlog::error("Failed to register the SKSE message listener - hooks unavailable.");
        return false;
    }

    spdlog::info("Message listener registered.");

    return true;
}
