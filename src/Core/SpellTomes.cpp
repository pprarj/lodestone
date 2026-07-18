// SpellTomes.cpp
// Lodestone - Shared SKSE framework
//
// TRACE-ONLY implementation for Stage C.3 / L2, Part 1.5. Observes book
// activations and logs the spell-tome ones; changes nothing. See SpellTomes.h for
// the rationale, the layer note, and the question this trace answers.
//
// EXCEPTION SAFETY: a C++ exception escaping a hook into the engine is undefined
// behavior. The thunk calls the original first (so vanilla behavior is intact no
// matter what) and wraps its own logging in try/catch.
//
// Phase L2 - Stage C.3 / Part 1.5 (TRACE)

#include "SpellTomes.h"

namespace Lodestone::Core::SpellTomes
{
	namespace
	{
		bool ShouldLog()
		{
			auto* logger = spdlog::default_logger_raw();
			return logger && logger->should_log(spdlog::level::info);
		}

		// -------------------------------------------------------------------
		// Hook: TESObjectBOOK::Activate - vfunc 0x37
		//
		// Shared across every book (the vtable belongs to the form type). The
		// original runs FIRST and unconditionally - the trace never alters what a
		// read does. After it, we log only spell-tome activations (TeachesSpell) to
		// keep the noise down, capturing enough to tell whether this fires for
		// inventory reads as well as world reads.
		// -------------------------------------------------------------------
		struct ActivateHook
		{
			static bool thunk(RE::TESObjectBOOK* a_this, RE::TESObjectREFR* a_targetRef,
				RE::TESObjectREFR* a_activatorRef, std::uint8_t a_arg3, RE::TESBoundObject* a_object,
				std::int32_t a_targetCount)
			{
				const bool result = func(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount);

				try {
					if (a_this && a_this->TeachesSpell() && ShouldLog()) {
						const char* bookName = a_this->GetName();

						auto* spell = a_this->GetSpell();
						const char* spellName = spell ? spell->GetName() : nullptr;

						// a_targetRef is usually the book reference being activated;
						// a_activatorRef is who activated it (the player, normally).
						// From inventory there may be no world reference at all - part
						// of what this trace is here to reveal.
						spdlog::info("SpellTomeTrace: ACTIVATE tome '{}' (0x{:08X}) | spell='{}' (0x{:08X}) | "
									 "target={} activator={} | arg3={} count={} | original returned {}",
							(bookName && *bookName) ? bookName : "<unnamed>",
							a_this->GetFormID(),
							(spellName && *spellName) ? spellName : "<none>",
							spell ? spell->GetFormID() : 0,
							a_targetRef ? "ref" : "none",
							a_activatorRef ? "ref" : "none",
							a_arg3,
							a_targetCount,
							result);

						if (a_activatorRef) {
							const char* actName = a_activatorRef->GetName();
							spdlog::info("SpellTomeTrace:   activator 0x{:08X} '{}' isPlayer={}",
								a_activatorRef->GetFormID(),
								(actName && *actName) ? actName : "<unnamed>",
								a_activatorRef->IsPlayerRef());
						}
					}
				} catch (...) {
					// A trace must never take the game down.
				}

				return result;
			}

			static inline REL::Relocation<decltype(thunk)> func;
			static inline constexpr std::size_t            idx{ 0x37 };
		};
	}

	void Install()
	{
		try {
			// [0] = the primary (TESBoundObject) branch of TESObjectBOOK's multiple
			// inheritance; Activate (0x37) is a TESBoundObject override and lives
			// there. The VTABLE constant carries the SE / AE / VR variants.
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_TESObjectBOOK[0] };

			ActivateHook::func = vtbl.write_vfunc(ActivateHook::idx, ActivateHook::thunk);

			spdlog::info("SpellTomeTrace: hook installed on TESObjectBOOK vtable (Activate @0x37). "
						 "TRACE ONLY - logs spell-tome reads, changes nothing.");
		} catch (const std::exception& e) {
			spdlog::error("SpellTomeTrace: failed to install: {} - trace unavailable.", e.what());
		} catch (...) {
			spdlog::error("SpellTomeTrace: failed to install (unknown exception) - trace unavailable.");
		}
	}
}
