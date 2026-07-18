// SpellTomes.cpp
// Lodestone - Shared SKSE framework
//
// Production implementation of the "keep the spell tome" capability (Stage C.3).
// See SpellTomes.h for the contract, the deliberately narrow scope (learning is
// not this module's concern), and the validation note.
//
// This SUPERSEDES the Part 1.5 trace (a passive vtable hook on TESObjectBOOK
// Activate that only logged). Investigation identified the real seam as
// TESObjectBOOK::Read - the function that teaches the spell and returns whether the
// book is consumed - so production hooks that instead. The trace is in git history.
//
// EXCEPTION SAFETY: a C++ exception escaping a hook into the engine is undefined
// behavior. The thunk calls the original first (so learning is never affected) and
// wraps everything after it; on any failure it returns the original result, i.e.
// vanilla consumption behavior.
//
// Phase L2 - Stage C.3

#include "SpellTomes.h"

#include "SKSE/RegistrationSet.h"

namespace Lodestone::Core::SpellTomes
{
	namespace
	{
		// Registered consumers. SendEvent/QueueEvent dispatches OnSpellTomeRead to
		// each registered script. Session-scoped: not serialized, so a consumer
		// re-registers after load (the runtime model the other modules use). The
		// set has its own lock, so the natives and the hook can touch it safely.
		SKSE::RegistrationSet<RE::TESObjectBOOK*, RE::TESObjectREFR*> g_readReg{ "OnSpellTomeRead" };

		bool ShouldLog()
		{
			auto* logger = spdlog::default_logger_raw();
			return logger && logger->should_log(spdlog::level::debug);
		}

		// -------------------------------------------------------------------
		// Hook: TESObjectBOOK::Read - the game function at RELOCATION_ID(17439, 17842)
		//
		// The original runs FIRST and unconditionally: it teaches the spell (or
		// skill) exactly as vanilla, and returns whether the book should be
		// consumed. Learning is never touched. For a spell tome we then return
		// FALSE, so the caller keeps the book, and queue OnSpellTomeRead for any
		// registered consumer. Non-tome books are passed through with the original's
		// own return value.
		//
		// PENDING VALIDATION: this assumes the caller uses Read's return value to
		// decide whether to remove the book. If instead Read removes the book
		// itself, suppression would need a different seam - to be confirmed in game.
		// -------------------------------------------------------------------
		struct ReadHook
		{
			static bool thunk(RE::TESObjectBOOK* a_this, RE::TESObjectREFR* a_reader)
			{
				const bool consumed = func(a_this, a_reader);

				try {
					if (a_this && a_this->TeachesSpell()) {
						// QueueEvent defers the dispatch off this hook's call stack
						// onto a game task - safer than reaching into the VM from
						// deep in the read path.
						g_readReg.QueueEvent(a_this, a_reader);

						if (ShouldLog()) {
							const char* name = a_this->GetName();
							spdlog::debug("SpellTomes: kept tome '{}' (0x{:08X}) after read (vanilla wanted consume={}).",
								(name && *name) ? name : "<unnamed>",
								a_this->GetFormID(),
								consumed);
						}

						// Default: do not eat the tome. The consumer calls
						// ConsumeSpellTome later if it wants it gone.
						return false;
					}
				} catch (...) {
					// Never take the game down; fall back to vanilla behavior.
					return consumed;
				}

				return consumed;
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		// -------------------------------------------------------------------
		// Natives
		//
		// Error convention (Stage B.3): failure by return value, never by throwing.
		// -------------------------------------------------------------------

		// Lodestone.RegisterForSpellTomeRead(Form) -> Bool
		// Registers a form whose script implements OnSpellTomeRead(Book, ObjectReference).
		bool RegisterForSpellTomeRead(RE::StaticFunctionTag*, RE::TESForm* a_receiver)
		{
			if (!a_receiver) {
				spdlog::warn("SpellTomes: RegisterForSpellTomeRead got a None form - ignored.");
				return false;
			}
			return g_readReg.Register(a_receiver);
		}

		// Lodestone.UnregisterForSpellTomeRead(Form) -> Bool
		bool UnregisterForSpellTomeRead(RE::StaticFunctionTag*, RE::TESForm* a_receiver)
		{
			if (!a_receiver) {
				return false;
			}
			return g_readReg.Unregister(a_receiver);
		}

		// Lodestone.ConsumeSpellTome(Book, ObjectReference) -> Bool
		// Removes one copy of akBook from akActor - the explicit "eat the tome now"
		// call. Returns false on a None argument.
		bool ConsumeSpellTome(RE::StaticFunctionTag*, RE::TESObjectBOOK* a_book, RE::TESObjectREFR* a_actor)
		{
			if (!a_book || !a_actor) {
				spdlog::warn("SpellTomes: ConsumeSpellTome got a None argument (book={}, actor={}) - ignored.",
					a_book ? "ok" : "NONE",
					a_actor ? "ok" : "NONE");
				return false;
			}

			a_actor->RemoveItem(a_book, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
			return true;
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("SpellTomes: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("RegisterForSpellTomeRead", "Lodestone", RegisterForSpellTomeRead);
		a_vm->RegisterFunction("UnregisterForSpellTomeRead", "Lodestone", UnregisterForSpellTomeRead);
		a_vm->RegisterFunction("ConsumeSpellTome", "Lodestone", ConsumeSpellTome);

		spdlog::info("SpellTomes: natives registered (RegisterForSpellTomeRead, UnregisterForSpellTomeRead, ConsumeSpellTome).");
		return true;
	}

	void Install()
	{
		try {
			// TESObjectBOOK::Read. The ID is not in the shipped 3.5.3 headers; it is
			// taken from the CommonLibSSE-NG source and is pending in-game
			// confirmation (see SpellTomes.h).
			REL::Relocation<std::uintptr_t> target{ REL::RelocationID(17439, 17842) };

			auto& trampoline = SKSE::GetTrampoline();
			ReadHook::func = trampoline.write_branch<5>(target.address(), ReadHook::thunk);

			spdlog::info("SpellTomes: hook installed on TESObjectBOOK::Read. "
						 "Spell tomes are learned as vanilla but kept, not consumed.");
		} catch (const std::exception& e) {
			spdlog::error("SpellTomes: failed to install: {} - spell-tome behavior unchanged.", e.what());
		} catch (...) {
			spdlog::error("SpellTomes: failed to install (unknown exception) - spell-tome behavior unchanged.");
		}
	}
}
