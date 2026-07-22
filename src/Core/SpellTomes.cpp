// SpellTomes.cpp
// Lodestone - Shared SKSE framework
//
// Production implementation of the spell-tome capability (Stage C.3, extended in
// 1.5.0 to full interception). See SpellTomes.h for the contract and the
// validation note.
//
// This SUPERSEDES the Part 1.5 trace (a passive vtable hook on TESObjectBOOK
// Activate that only logged). Investigation identified the real seam as
// TESObjectBOOK::Read - the function that teaches the spell and returns whether the
// book is consumed - so production hooks that instead. The trace is in git history.
//
// WHAT CHANGED IN 1.5.0. Until 1.4.0 this module only stopped the tome from being
// eaten: it called the original, which teaches the spell, and overrode the consume
// flag. From 1.5.0 it suppresses the vanilla learn as well, so a spell tome read
// grants nothing on its own. The consumer teaches with its own AddSpell, when its
// own system says so, and eats the book with ConsumeSpellTome if it wants to.
//
// WHAT CHANGED IN 1.8.0. Suppression now waits for a consumer. 1.5.0 suppressed
// unconditionally: install the DLL and every spell tome stopped teaching and
// stopped being eaten, with no consumer involved. That held while this code only
// ever loaded next to the one mod that wanted the suppression; it stopped holding
// when the first consumer's core began requiring Lodestone for every user, so
// every install - whether or not anything ever registers for tome reads - had
// vanilla tomes broken, and collided with any other mod on this seam. The module
// now follows the same contract as the other three (CastTime, BookFramework,
// MagicScaling): hook installed always, real passthrough until someone registers.
//
// So the FIRST question the thunk asks is whether anyone has registered:
//
//   nobody registered -> call the original and return its answer, for EVERY
//                        book, tome or not. No flag set, no event queued.
//                        Identical to the DLL not being installed.
//   registered, tome  -> never call the original. No learn, no vanilla message,
//                        return false so the caller keeps the book. Mark the book
//                        read (see the thunk). Dispatch OnSpellTomeRead.
//   registered, other -> call the original, return what it returned. Skill books
//                        and ordinary books are not this module's business.
//
// EXCEPTION SAFETY: a C++ exception escaping a hook into the engine is undefined
// behavior. The tome path is wrapped and its failure path still returns false -
// suppression is the contract, and a consumer seeing no event is a missed feature,
// while a consumer seeing a spell it never granted is a broken save.
//
// Phase L2 - Stage C.3

#include "SpellTomes.h"

#include <atomic>

#include <safetyhook.hpp>

#include "SKSE/RegistrationSet.h"

namespace Lodestone::Core::SpellTomes
{
	namespace
	{
		// Registered consumers. SendEvent/QueueEvent dispatches OnSpellTomeRead to
		// each registered script. Session-scoped: not serialized, so a consumer
		// re-registers after load (the runtime model the other modules use). The
		// set has its own lock, so the natives and the hook can touch it safely.
		//
		// Since 1.8.0 the hook DOES ask whether anyone is registered: an empty set
		// means passthrough, so emptiness became a decision input, not just a no-op
		// dispatch. RegistrationSetBase does not expose it (verified against the
		// installed 3.5.3 source, the same pinned tree the linked lib was built
		// from: no empty()/size(), _handles and _lock are protected). Rather than
		// reach into the internal set, this subclass keeps its own count next to
		// it: +1 only when Register reports a genuine insertion, -1 only when
		// Unregister reports a genuine removal - the 3.5.3 source returns exactly
		// that (insert().second / found-and-erased), so duplicates and failed
		// handles never move the count. Both updates happen under the set's own
		// _lock (a recursive_mutex, so holding it around the base call is safe),
		// keeping count and set in step. The count itself is atomic so the hook
		// reads it without touching the lock.
		class CountedRegistrationSet : public SKSE::RegistrationSet<RE::TESObjectBOOK*, RE::TESObjectREFR*>
		{
		public:
			using super = SKSE::RegistrationSet<RE::TESObjectBOOK*, RE::TESObjectREFR*>;

			explicit CountedRegistrationSet(const std::string_view& a_eventName) :
				super(a_eventName)
			{}

			// T is RE::TESForm or RE::BGSBaseAlias - the template forwards to the
			// matching base overload, so the natives below keep calling
			// Register/Unregister exactly as before.
			template <class T>
			bool Register(const T* a_object)
			{
				Locker locker(_lock);
				const bool added = super::Register(a_object);
				if (added) {
					_count.fetch_add(1, std::memory_order_release);
				}
				return added;
			}

			template <class T>
			bool Unregister(const T* a_object)
			{
				Locker locker(_lock);
				const bool removed = super::Unregister(a_object);
				if (removed) {
					_count.fetch_sub(1, std::memory_order_release);
				}
				return removed;
			}

			// One atomic load - what the hook calls on every book read. A
			// registration landing mid-read costs at most one extra passthrough,
			// the same benign race the CastTime hook accepts for its channel.
			bool HasRegistrants() const
			{
				return _count.load(std::memory_order_acquire) > 0;
			}

		private:
			std::atomic<std::size_t> _count{ 0 };
		};

		CountedRegistrationSet g_readReg{ "OnSpellTomeRead" };

		// debug: per-event lines are absent from a Release build, since Log.cpp
		// caps the level at info when NDEBUG is defined. The guard is not
		// redundant with that - spdlog skips formatting below the active level,
		// but the arguments are still evaluated at the call site.
		bool ShouldLog()
		{
			auto* logger = spdlog::default_logger_raw();
			return logger && logger->should_log(spdlog::level::debug);
		}

		// The inline hook object owns the original; call it through this.
		SafetyHookInline g_readHook{};

		// -------------------------------------------------------------------
		// Hook: TESObjectBOOK::Read - the game function at RELOCATION_ID(17439, 17842)
		//
		// The original teaches the spell (or skill) and returns whether the book
		// should be consumed. Non-tome books are always passed through with the
		// original's own return value; this module has no opinion about them.
		//
		// Since 1.8.0 the tome behavior below only engages once a consumer has
		// registered. Before that, the thunk's first test fails and EVERY book -
		// tome included - takes the passthrough, identical to vanilla. The rest
		// of this block describes the registered path, which is unchanged.
		//
		// ORDERING (the same requirement the CastTime channel documents): a
		// consumer must register BEFORE the first tome read it cares about. A
		// tome read before the first registration passes through as vanilla -
		// the spell is learned, the book is consumed - and neither is reverted
		// when a registration lands later. This is the same silent degradation
		// the other modules already accept: "not registered yet" reads
		// identically to "not installed".
		//
		// WHY THIS THUNK DOES NOT CALL THE ORIGINAL ON THE TOME PATH. The house rule
		// (CONVENTIONS, "The original runs first") is the right rule for a hook that
		// adjusts a value the engine computed. This hook is the other kind: its job
		// on a spell tome is to SUPPRESS what the original does. Read teaches the
		// spell itself, so calling it and undoing the teach afterwards would mean
		// RemoveSpell on a spell the actor may have legitimately known already -
		// unrecoverable, since the engine does not say which case it was - and the
		// vanilla "spell learned" message would have been shown regardless.
		// Suppression is only possible by not making the call. CONVENTIONS carries
		// that exception explicitly as of 1.5.0.
		//
		// WHAT IS RESTORED BY HAND, AND WHY ONLY THIS. Skipping the original skips
		// every side effect it had, so each one is a deliberate decision:
		//
		//   kHasBeenRead  -> RESTORED below. The player did open the book; leaving
		//                    it flagged unread makes it reappear as new in the
		//                    inventory and in every "unread books" UI. Reading is
		//                    what happened, learning is what was suppressed, and
		//                    the flag records the first.
		//   teach spell   -> suppressed. The entire point.
		//   consume book  -> suppressed via the return value, as since 1.3.0.
		//
		// SKIPPING THE ORIGINAL IS VALIDATED IN GAME (1.5.0, Release build). Two
		// reads, both on a tome whose spell the player did not know:
		//
		//   read from the inventory   -> spell NOT learned, book NOT consumed,
		//                                book shown as read afterwards, read closed
		//                                cleanly - no stuck menu, no lost input.
		//   dropped, then picked up   -> arrived in the inventory unread, unlearned
		//                                and unconsumed.
		//
		// The second one is the more informative of the two: acquisition does not
		// reach this function at all, only an active read does. That is what keeps
		// a tome taken from a container or a merchant unread, with this module
		// doing nothing, exactly as in vanilla.
		//
		// Hook chaining cannot explain these results. Other plugins on this seam
		// suppress CONSUMPTION only; none of them stops the spell from being
		// learned, so "not learned" has no other possible source. That is the trap
		// described below, and it is the reason this particular observation is
		// worth trusting.
		//
		// STILL UNOBSERVED, so do not read this block as covering it: the
		// no-registrant passthrough added in 1.8.0 (a tome read with nothing
		// registered teaching and consuming as vanilla) - it is the new manual
		// test for this change and must be proven in game, together with a
		// re-run of the registered path above to show it did not move - and the
		// registered-path passthrough for skill books and ordinary books, which
		// has never been watched under a registrant.
		//
		// Nothing per-read appears in a Release log: those lines are spdlog::debug
		// and Log.cpp caps the level at info under NDEBUG. Their absence during a
		// test is expected and is not evidence that the hook did not run.
		//
		// VALIDATED IN GAME. Both the address and the meaning of the return value
		// were unproven for a long time - the module could not even install, so
		// nothing about it had ever been observed. A log-only pass answered both:
		//
		//   READ book='Spell Tome: Firebolt' teachesSpell=true  | original returned true
		//   READ book='2920, First Seed, v3' teachesSpell=false | original returned false
		//
		// The tome was consumed and the ordinary book was not, so the return value
		// means "consume this book" and returning false is what keeps a tome. That
		// is the assumption this module was built on, now measured rather than
		// hoped for.
		//
		// ONE TRAP WORTH KEEPING. The same trace run with another spell-tome plugin
		// still active reported the tome returning FALSE. That plugin hooks this
		// same function, so the "original" being called was its thunk rather than
		// the engine's - hook chaining is invisible in a log, and the value looked
		// perfectly plausible. Anything measured here has to be measured with other
		// plugins touching this function disabled, or it is measuring them.
		// -------------------------------------------------------------------
		struct ReadHook
		{
			static bool thunk(RE::TESObjectBOOK* a_this, RE::TESObjectREFR* a_reader)
			{
				// 0. Anyone registered? One atomic load, the cheapest test first.
				//    With no consumer this module has no business here at all:
				//    every book - tome or not - takes the original untouched, no
				//    flag set, no event queued. Identical to the DLL not being
				//    installed.
				if (!g_readReg.HasRegistrants()) {
					return g_readHook.call<bool, RE::TESObjectBOOK*, RE::TESObjectREFR*>(a_this, a_reader);
				}

				// TeachesSpell reads one flag on the book, so this is cheap enough
				// to run before deciding anything.
				const bool isTome = [a_this]() {
					try {
						return a_this && a_this->TeachesSpell();
					} catch (...) {
						return false;
					}
				}();

				if (!isTome) {
					// Skill books, notes, ordinary books: untouched, including the
					// original's own consume decision.
					return g_readHook.call<bool, RE::TESObjectBOOK*, RE::TESObjectREFR*>(a_this, a_reader);
				}

				// From here the original is never called, so the spell is not
				// taught and the vanilla message is not shown.
				try {
					a_this->data.flags.set(RE::OBJ_BOOK::Flag::kHasBeenRead);

					// QueueEvent defers the dispatch off this hook's call stack
					// onto a game task - safer than reaching into the VM from
					// deep in the read path. An empty set makes it a no-op.
					g_readReg.QueueEvent(a_this, a_reader);

					if (ShouldLog()) {
						const char* name = a_this->GetName();
						spdlog::debug("SpellTomes: intercepted tome '{}' (0x{:08X}) - not learned, not consumed, marked read.",
							(name && *name) ? name : "<unnamed>",
							a_this->GetFormID());
					}
				} catch (...) {
					// Never take the game down. Suppression already happened by not
					// calling the original, and it stays in force: returning false
					// here costs at most the read flag and the event, whereas any
					// other answer would hand back behavior this module exists to
					// remove.
				}

				return false;
			}
		};

		// -------------------------------------------------------------------
		// Natives
		//
		// Error convention (Stage B.3): failure by return value, never by throwing.
		// -------------------------------------------------------------------

		// Lodestone.RegisterForSpellTomeRead(Form) -> Bool
		// Registers a form whose script implements OnSpellTomeRead(Book, ObjectReference).
		// The first registration is also what arms the suppression (1.8.0) - see
		// the ordering note at the thunk: register before the first relevant read.
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

		// Lodestone.RegisterForSpellTomeReadAlias(Alias) -> Bool
		// Registers an alias whose script implements
		// OnSpellTomeRead(Book, ObjectReference). Needed for ReferenceAlias
		// consumers: an alias script is bound to the alias handle, which a
		// Form-keyed registration cannot reach - a Papyrus Alias has no cast to
		// Form, so the Form native above cannot serve one.
		//
		// The parameter type is not a guess. SKSE/RegistrationSet.h declares
		// RegistrationSetBase::Register(const RE::BGSBaseAlias*) alongside the
		// TESForm and ActiveEffect overloads, and RE/B/BGSBaseAlias.h carries
		// VMTYPEID = 139, which is what the VM binds a Papyrus `Alias` argument
		// to. Native param, RegistrationSet overload and .psc type therefore
		// agree. The handle lands in the same _handles set QueueEvent already
		// fans out to, so dispatch needs no change.
		bool RegisterForSpellTomeReadAlias(RE::StaticFunctionTag*, RE::BGSBaseAlias* a_alias)
		{
			if (!a_alias) {
				spdlog::warn("SpellTomes: RegisterForSpellTomeReadAlias got a None alias - ignored.");
				return false;
			}
			return g_readReg.Register(a_alias);
		}

		// Lodestone.UnregisterForSpellTomeReadAlias(Alias) -> Bool
		bool UnregisterForSpellTomeReadAlias(RE::StaticFunctionTag*, RE::BGSBaseAlias* a_alias)
		{
			if (!a_alias) {
				return false;
			}
			return g_readReg.Unregister(a_alias);
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
		a_vm->RegisterFunction("RegisterForSpellTomeReadAlias", "Lodestone", RegisterForSpellTomeReadAlias);
		a_vm->RegisterFunction("UnregisterForSpellTomeReadAlias", "Lodestone", UnregisterForSpellTomeReadAlias);
		a_vm->RegisterFunction("ConsumeSpellTome", "Lodestone", ConsumeSpellTome);

		spdlog::info("SpellTomes: natives registered (RegisterForSpellTomeRead, UnregisterForSpellTomeRead, "
					 "RegisterForSpellTomeReadAlias, UnregisterForSpellTomeReadAlias, ConsumeSpellTome).");
		return true;
	}

	// -----------------------------------------------------------------------
	// THE EXCEPTION THAT WAS HERE, AND WHY IT IS GONE (1.8.0)
	//
	// From 1.5.0 to 1.7.0 this block declared "the one deliberate exception to
	// passive until asked": suppression was unconditional, with no consumer
	// involved, and a registration only added the event on top. The argument was
	// that Lodestone is a modder resource - nobody installs it to play with, so a
	// mod hooking tome reading would want the vanilla instant-learn gone before
	// its own system could exist, and there was nobody a conservative default
	// would have protected.
	//
	// That argument had a hidden premise: that this DLL only ever loads next to a
	// consumer that wants the suppression. The premise died when the first
	// consumer's core started requiring Lodestone for every user. From then on,
	// everyone with that mod had this DLL in the load order - including users who
	// never enable anything that registers for tome reads - and for them the
	// exception meant vanilla spell tomes silently stopped teaching and stopped
	// being eaten, and any other mod driving this same seam was broken for free.
	//
	// So the exception is gone and the module follows the general rule, the same
	// contract as the other three: the hook is installed always, but until a
	// consumer registers via RegisterForSpellTomeRead(Alias), the thunk calls
	// the original and returns its answer, for every book. The suppress path and
	// everything validated about it in 1.5.0/1.6.0 is intact - it just waits to
	// be asked, like everything else in this plugin.
	//
	// The corollary a consumer must know: registration has to land BEFORE the
	// first tome read it wants intercepted. A read before that is vanilla and is
	// not reverted. See the ordering note at the thunk.
	// -----------------------------------------------------------------------
	void Install()
	{
		try {
			// TESObjectBOOK::Read. The ID is not in the shipped 3.5.3 headers; it
			// came from the CommonLibSSE-NG source, and the trace quoted at the
			// thunk is what proved it.
			REL::Relocation<std::uintptr_t> target{ REL::RelocationID(17439, 17842) };

			// The guard that used to stand here is gone, and so is the branch hook
			// it protected. It refused to install because the first byte at this
			// address is 0x48 - a REX prefix, i.e. an ordinary function prologue,
			// which confirmed in game that this is a function body and not a call
			// site. Trampoline::write_branch cannot detour a body; that is what it
			// was protecting against, and it did its job.
			//
			// TESObjectBOOK::Read is non-virtual, so there is no vtable slot to
			// swap. An inline hook is the correct instrument: SafetyHook relocates
			// the displaced prologue and suspends other threads while patching.
			//
			// The address was a hypothesis for a long time, on the grounds that an
			// inline hook on a wrong address installs and runs quietly on the wrong
			// function. It was settled by the log-only pass quoted at the thunk;
			// the wording that still called it unproven outlived that trace and was
			// corrected in 1.5.0.
			g_readHook = safetyhook::create_inline(
				reinterpret_cast<void*>(target.address()),
				reinterpret_cast<void*>(&ReadHook::thunk));

			if (!g_readHook) {
				spdlog::error("SpellTomes: SafetyHook refused to hook the tome-read target - "
							  "spell tomes keep vanilla behavior.");
				return;
			}

			spdlog::info("SpellTomes: hook installed on TESObjectBOOK::Read. "
						 "No consumer registered yet - passthrough (vanilla tome behavior) until "
						 "a consumer calls Lodestone.RegisterForSpellTomeRead.");
		} catch (const std::exception& e) {
			spdlog::error("SpellTomes: failed to install: {} - spell-tome behavior unchanged.", e.what());
		} catch (...) {
			spdlog::error("SpellTomes: failed to install (unknown exception) - spell-tome behavior unchanged.");
		}
	}
}
