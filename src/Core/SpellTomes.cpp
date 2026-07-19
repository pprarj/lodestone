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

	// -----------------------------------------------------------------------
	// THE ONE DELIBERATE EXCEPTION TO "PASSIVE UNTIL ASKED"
	//
	// Every other module in this plugin is inert until a consumer registers
	// something: install it with nothing else and the game behaves exactly as it
	// did before. This module does not follow that rule, and the difference is
	// intentional rather than an oversight.
	//
	// Installing it keeps spell tomes from being consumed, with no consumer
	// involved. The capability is inverted compared to the rest: the default IS
	// the behavior, and a consumer calls ConsumeSpellTome when it wants the book
	// eaten after all. Making it opt-in would mean shipping a module that does
	// nothing at all for anyone who has not written script against it, for a
	// behavior that needs no configuration to be useful.
	//
	// Recorded here because it is the sort of thing that reads like a bug to
	// whoever finds it next - including the author, months from now.
	// -----------------------------------------------------------------------
	void Install()
	{
		try {
			// TESObjectBOOK::Read. The ID is not in the shipped 3.5.3 headers; it is
			// taken from the CommonLibSSE-NG source and is pending in-game
			// confirmation (see SpellTomes.h).
			REL::Relocation<std::uintptr_t> target{ REL::RelocationID(17439, 17842) };

			// GUARD - same defect, same reasoning as BookFramework::Install; the
			// full explanation lives there.
			//
			// Short version: write_branch redirects an existing rel32 branch, it
			// does not detour a function body. Pointed at a prologue it returns a
			// garbage "original" that the thunk then calls. TESObjectBOOK::Read is
			// non-virtual, so there is no vtable slot to swap instead - the honest
			// fix needs a real call site, which needs disassembly evidence this
			// module does not have yet.
			//
			// Until then: refuse, log, and leave spell tomes behaving exactly as
			// vanilla. A tome that gets eaten is a design regression; a tome that
			// takes the game down is not something to ship on a guess.
			const auto opcode = *reinterpret_cast<const std::uint8_t*>(target.address());
			if (opcode != 0xE8 && opcode != 0xE9) {
				spdlog::error("SpellTomes: NOT installing the tome-read hook - the target is not a rel32 branch "
							  "(first byte 0x{:02X}, expected 0xE8 call or 0xE9 jmp). Spell tomes keep vanilla "
							  "behavior (learned and consumed) until a real call site is identified.",
					opcode);
				return;
			}

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
