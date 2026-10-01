// Translation.cpp
// Lodestone - Shared SKSE framework
//
// Natives behind Lodestone.Translate and Lodestone.GetTranslationStatus.
//
// WHY THIS DOES THE LOOKUP ITSELF INSTEAD OF CALLING SKSE::Translation::Translate.
// That helper takes the arguments INSIDE the key - "$KEY{Firebolt}" - and finds
// them by counting braces (Translation.cpp:136-163 of the submodule). Two
// consequences made it unusable here:
//
//   1. A runtime value containing '{' or '}' corrupts the nesting, and there is
//      no escape syntax to protect it. The values this module exists to insert
//      are spell and item names, which come from third-party records - content we
//      do not control. Raised by the Intelligence Matters consumer while reading
//      the same file, and it is right.
//   2. The helper's own brace pass would eat the "{}" placeholders before we
//      could put anything in them.
//
// So the key travels to the engine EXACTLY as the caller wrote it, and the
// arguments never pass through any parser. That does not dodge the problem by
// convention - it removes the code path where the problem exists.
//
// This is not a reimplementation for its own sake: the lookup proper is six
// lines (engine translator, TranslateInfo, UTF-16 out), and the substitution
// follows the submodule's own right-to-left "{}" rule so a translation file
// written for one works with the other.
//
// WHY GetStateAddRef AND NOT THE GetState<T> HELPER. GFxStateBag::GetState<T>
// dereferences the returned pointer to call Release on it, guarded only by an
// assert (GPtr.h:173-177) - and assert is compiled out by NDEBUG, which is the
// release build the player runs. GetStateAddRef returns null for a state that
// does not exist, which is exactly the early-boot case this module has to survive.
// Release is called by hand below.
//
// Phase L-T2

#include "Translation.h"

namespace Lodestone::Core::Translation
{
	namespace
	{
		// Status codes returned by GetTranslationStatus, and the reason this
		// module has a second native at all.
		//
		// The submodule's helper collapses THREE different failures into one
		// false, and only a log line tells two of them apart. A consumer that
		// reads "no text" as "not translated" will show a raw key on a screen
		// where the translator asked for silence, or report a missing string for
		// a table that simply does not exist yet.
		enum : std::int32_t
		{
			kOk = 0,           // key found, text non-empty
			kNotAKey = 1,      // does not start with '$'
			kNoTranslator = 2, // the Scaleform translator does not exist yet
			kAbsent = 3,       // not in the table
			kEmptyValue = 4    // in the table, value deliberately empty
		};

		// Result of one lookup: the status and, when the status is kOk or
		// kEmptyValue, the text as stored (placeholders not yet substituted).
		struct Lookup
		{
			std::int32_t status{ kAbsent };
			std::string  text;
		};

		// Asks the engine's translator for one key, verbatim.
		//
		// ABSENT AND EMPTY ARE TOLD APART BY THE BUFFER, NOT BY THE TEXT. An
		// absent key leaves the buffer empty; a key whose value is empty comes
		// back with a buffer that is NOT empty but whose content terminates at
		// once. That is why this reads buffer.empty() before converting, and why
		// the two statuses exist.
		//
		// MEASURED, phase L-T1, same run, same millisecond:
		//   key='$LT1_VAZIA'            returned=true  result=''
		//   key='$LT1_NAO_EXISTE_12345' returned=false result=''
		// The DISCRIMINATION is measured. The mechanism above - a buffer whose
		// length is non-zero over NUL-terminated empty content - is INFERRED from
		// GFxWStringBuffer::empty() being _length == 0; the length itself was
		// never read.
		Lookup Resolve(const std::string& a_key)
		{
			Lookup out;

			if (!a_key.starts_with('$')) {
				out.status = kNotAKey;
				return out;
			}

			const auto scaleformManager = RE::BSScaleformManager::GetSingleton();
			const auto loader = scaleformManager ? scaleformManager->loader : nullptr;
			if (!loader) {
				out.status = kNoTranslator;
				return out;
			}

			auto* translator = loader->GetStateAddRef<RE::GFxTranslator>(RE::GFxState::StateType::kTranslator);
			if (!translator) {
				out.status = kNoTranslator;
				return out;
			}

			const std::wstring keyUtf16 = SKSE::stl::utf8_to_utf16(a_key).value_or(L""s);

			RE::GFxWStringBuffer buffer;

			RE::GFxTranslator::TranslateInfo info;
			info.key = keyUtf16.c_str();
			info.result = std::addressof(buffer);

			translator->Translate(std::addressof(info));

			const bool absent = buffer.empty();
			if (!absent) {
				out.text = SKSE::stl::utf16_to_utf8(buffer.c_str()).value_or(""s);
			}

			translator->Release();

			if (absent) {
				out.status = kAbsent;
			} else if (out.text.empty()) {
				out.status = kEmptyValue;
			} else {
				out.status = kOk;
			}

			return out;
		}

		// BSFixedString has no conversion to bool, and c_str() can be null on a
		// default-constructed one. Same helper, same reason, as WebUIBridge.cpp.
		std::string ToStd(const RE::BSFixedString& a_str)
		{
			const char* raw = a_str.c_str();
			return raw ? std::string(raw) : std::string();
		}

		// Replaces the "{}" placeholders in a_text with a_args, right to left -
		// the same order and the same two-character token the submodule uses, so
		// a translation file written for either side behaves the same.
		//
		// Extra placeholders are left standing and extra arguments are dropped.
		// Neither is silently "corrected": a mismatch between a file and its
		// caller is the translator's business, and a visible "{}" says so on the
		// screen where someone can see it.
		std::string Substitute(std::string a_text, const std::vector<std::string>& a_args)
		{
			std::size_t remaining = a_args.size();
			auto        pos = a_text.rfind("{}");
			while (pos != std::string::npos && remaining > 0) {
				--remaining;
				a_text = a_text.replace(pos, 2, a_args[remaining]);
				if (pos == 0) {
					break;
				}
				pos = a_text.rfind("{}", pos - 1);
			}
			return a_text;
		}

		// The lookup plus substitution, shared by both Translate natives so the
		// two cannot drift apart.
		//
		// ON FAILURE THIS RETURNS THE KEY AS GIVEN, never an empty string. A key on
		// screen is wrong in a way someone reports; a blank line is wrong in a way
		// nobody notices. GetTranslationStatus says which failure it was.
		//
		// A key whose value is deliberately empty returns that empty string - it is
		// not a failure, and substituting the key there would print "$KEY" where the
		// translator asked for nothing.
		std::string TranslateImpl(const std::string& a_key, const std::vector<std::string>& a_args)
		{
			const Lookup lookup = Resolve(a_key);
			if (lookup.status == kOk) {
				return Substitute(lookup.text, a_args);
			}
			if (lookup.status == kEmptyValue) {
				return {};
			}
			return a_key;
		}

		// Lodestone.Translate(String, String[]) -> String
		//
		// The key goes to the table verbatim, so it has to be written the way the
		// translation file holds it, placeholders included: "$IM_Learned{}{}".
		//
		// ON FAILURE THIS RETURNS THE KEY AS GIVEN, never an empty string. A key
		// on screen is wrong in a way someone reports; a blank line is wrong in a
		// way nobody notices. GetTranslationStatus says which failure it was.
		//
		// A key whose value is deliberately empty returns that empty string - it
		// is not a failure, and substituting the key there would print "$KEY"
		// where the translator asked for nothing.
		std::string Translate(RE::StaticFunctionTag*, RE::BSFixedString a_key, std::vector<std::string> a_args)
		{
			const std::string key = ToStd(a_key);

			try {
				return TranslateImpl(key, a_args);
			} catch (...) {
				spdlog::error("Translation: Translate threw.");
				return key;
			}
		}

		// Lodestone.TranslatePlain(String) -> String
		//
		// Translate for a key with no placeholders. IT EXISTS TO REMOVE A MEASURED
		// TRAP, not to save typing, and the trap was in this file's own advice.
		//
		// Papyrus cannot build an empty array - "new String[0]" does not compile -
		// so the only way to reach Translate with no arguments was to pass None.
		// That call works, but it POISONS THE CALLER. The Papyrus compiler routes
		// every ARRAYCREATE of a function through ONE temporary, and passing None
		// for a String[] argument emits a CAST on that same temporary. From there
		// on, every "new String[n]" in that function fails with "Cannot create an
		// array into a non-array variable", the variable stays None, and the array
		// reaches the native EMPTY - which is indistinguishable from "no arguments",
		// so nothing reports it.
		//
		// MEASURED in phase L-T2, 2026-10-01: seven failed creations in one
		// function, identical across two consecutive game runs, isolated in the
		// compiler's own assembly against a control differing by a single line.
		// Passing None also logs two Papyrus warnings per call.
		//
		// Otherwise identical to Translate with no arguments: a "{}" left standing
		// in the value stays visible, because a file and its caller disagreeing is
		// the translator's business and has to show.
		std::string TranslatePlain(RE::StaticFunctionTag*, RE::BSFixedString a_key)
		{
			const std::string key = ToStd(a_key);

			try {
				return TranslateImpl(key, {});
			} catch (...) {
				spdlog::error("Translation: TranslatePlain threw.");
				return key;
			}
		}

		// Lodestone.GetTranslationStatus(String) -> Int
		//
		// 0 found with text, 1 not a key (no '$'), 2 translator not up yet,
		// 3 absent from the table, 4 present with an empty value.
		//
		// Stateless on purpose: it repeats the lookup rather than remembering the
		// last one. A remembered status races with every other caller in the load
		// order, and this table is shared by all of them.
		std::int32_t GetTranslationStatus(RE::StaticFunctionTag*, RE::BSFixedString a_key)
		{
			try {
				return Resolve(ToStd(a_key)).status;
			} catch (...) {
				spdlog::error("Translation: GetTranslationStatus threw.");
				return kAbsent;
			}
		}
	}

	void Install()
	{
		// Nothing to do if the file is absent - ParseTranslation reports that
		// itself, and a Lodestone with no keys of its own is a valid state. The
		// natives above do not depend on this call: they read the shared table,
		// which other plugins have already filled.
		SKSE::Translation::ParseTranslation("Lodestone");

		spdlog::info("Translation: own translation file parsed (the game loads by active plugin name, "
					 "and this plugin has none).");
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("Translation: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("Translate", "Lodestone", Translate);
		a_vm->RegisterFunction("TranslatePlain", "Lodestone", TranslatePlain);
		a_vm->RegisterFunction("GetTranslationStatus", "Lodestone", GetTranslationStatus);

		spdlog::info("Translation: natives registered (Translate, TranslatePlain, GetTranslationStatus). "
					 "The table is one "
					 "for the whole load order - a consumer reads any active plugin's keys.");
		return true;
	}
}
