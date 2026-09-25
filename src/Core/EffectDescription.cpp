// EffectDescription.cpp
// Lodestone - Shared SKSE framework
//
// Native implementation behind Lodestone.GetEffectDescription,
// Lodestone.SetEffectDescription and Lodestone.ClearEffectDescription.
//
// Hands back EffectSetting::magicItemDescription as the engine keeps it, with
// markers such as <mag> and <dur> still in the text. Nothing is substituted
// here: the caller holds the numbers, and the magic menu's own display
// formatting is not something this plugin reimplements.
//
// MEASURED BEFORE THIS WAS WRITTEN (phase L-F2, a throwaway probe on DLL
// 1.23.2, development load order): the field holds RESOLVED text for localized
// plugins too. Skyrim.esm is localized, and its Firebolt effect (0x012F03) came
// back as the whole sentence, not as a string-table id; the per-master counts
// of described effects matched a read of the plugin files. That is the only
// reason this can be a plain read instead of a string-table lookup.
//
// ONE DEPARTURE FROM "AS STORED": a description made only of whitespace comes
// back as "". Vanilla ships two ("\r\n" on 0x10192D, " " on 0x082A36) and the
// measured load order nine. On a card they mean "no description", and a caller
// testing for "" would otherwise draw an empty line. Any other text is returned
// untouched, leading and trailing whitespace included.
//
// WRITING (phase L-F4). magicItemDescription is public, non-const, and a plain
// BSFixedString assignment - measured in the necessity that opened this phase
// (Necessidade-escrever-descricao-de-magiceffect.md). A MagicEffect read from a
// plugin is shared by the whole load order for the rest of the session: writing
// one that was not cloned first contaminates every spell that uses it. Cloning
// is the caller's job (same rule already written for casting type and delivery
// - see Necessidade-escrever-casting-type-e-delivery-em-magia-de-runtime.md);
// this module writes wherever it is told.
//
// THE WRITE IS QUEUED, NOT IMMEDIATE. A native runs on the Papyrus VM thread;
// the field can be read by the magic menu on the game thread at any time, so
// the assignment is queued through SKSE::GetTaskInterface(), the same pattern
// DetectionRead uses (DetectionRead.cpp). Queuing does not cost a frame - the
// task queue is drained in a loop, and a task added during that drain runs in
// the same pass - measured, and recorded in the private workspace
// - but it does mean the native returns before the write has landed. The Bool
// these two natives return means "accepted", not "written": True is only a
// promise that the task was queued, not a confirmation that the field changed.
//
// PERSISTENCE IS SESSION-ONLY, same doctrine as BookFramework v1
// (BookFramework.h, PERSISTENCE (v1)): nothing is serialized to the save, and a
// consumer re-applies its description after every load, the same way the
// Intelligence Matters IM_FUS_Carrier effects already re-apply condition data.
//
// THE ORIGINAL TEXT MAP exists only so Clear can undo a Set: the text a written
// effect had is captured once, on the FIRST write, and never overwritten after.
// Presence in the map doubles as "this plugin has touched this effect" - same
// shape as BookFramework's g_bookText (BookFramework.cpp), keyed the same way,
// guarded the same way.
//
// Phase L-F2 (read), L-F4 (write)

#include "EffectDescription.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace Lodestone::Core::EffectDescription
{
	namespace
	{
		// Lodestone.GetEffectDescription(MagicEffect) -> String
		//
		// The effect's description text, raw, or "" when the effect is None, has
		// no description, or has one made only of whitespace. "" is an ordinary
		// answer here, not a failure - close to half of all vanilla effects have
		// no text.
		//
		// A plain read of form data this plugin never writes: no hook, no state,
		// nothing cached. Wrapped anyway, because no native in this plugin lets
		// an exception reach the VM (PluginInfo.cpp, API CONVENTION).
		RE::BSFixedString GetEffectDescription(RE::StaticFunctionTag*, RE::EffectSetting* a_effect)
		{
			try {
				if (!a_effect) {
					return RE::BSFixedString("");
				}

				const std::string_view text = a_effect->magicItemDescription.c_str();
				if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
					return RE::BSFixedString("");
				}

				return a_effect->magicItemDescription;
			} catch (...) {
				return RE::BSFixedString("");
			}
		}

		// -------------------------------------------------------------------
		// The original-text store - MagicEffect FormID -> the description the
		// engine had before this plugin's first write to that effect.
		//
		// Written by SetEffectDescription (on a Papyrus VM thread) and read by
		// ClearEffectDescription (same thread family), so a plain mutex is
		// enough - unlike the field itself, this map is never touched by the
		// game thread. Session-scoped like the field write it undoes: it is
		// never serialized, and it goes empty on every load.
		// -------------------------------------------------------------------
		std::unordered_map<RE::FormID, std::string> g_originalText;
		std::mutex                                  g_originalTextLock;

		// Lodestone.SetEffectDescription(MagicEffect, String) -> Bool
		//
		// Queues a write of asText to akEffect's description. See the file
		// header for what the effect being shared, the queuing, and the
		// return value's meaning ("accepted", not "written") actually mean.
		//
		// The first call for a given effect captures its current text before
		// touching anything, so ClearEffectDescription can undo this call (and
		// every one after it) later in the session. A later call on the same
		// effect does not recapture: the original would already be this
		// plugin's own text, not the record's.
		bool SetEffectDescription(RE::StaticFunctionTag*, RE::EffectSetting* a_effect, RE::BSFixedString a_text)
		{
			try {
				if (!a_effect) {
					spdlog::warn("EffectDescription: SetEffectDescription got a None effect - ignored.");
					return false;
				}

				const RE::FormID  formID = a_effect->GetFormID();
				const std::string newText = a_text.c_str() ? a_text.c_str() : "";

				{
					std::lock_guard<std::mutex> lock(g_originalTextLock);
					g_originalText.try_emplace(formID, a_effect->magicItemDescription.c_str() ? a_effect->magicItemDescription.c_str() : "");
				}

				auto* task = SKSE::GetTaskInterface();
				if (!task) {
					spdlog::error("EffectDescription: no task interface available - cannot queue a write for effect (0x{:08X}).", formID);
					return false;
				}

				task->AddTask([a_effect, newText]() {
					a_effect->magicItemDescription = newText.c_str();
				});

				return true;
			} catch (...) {
				return false;
			}
		}

		// Lodestone.ClearEffectDescription(MagicEffect) -> Bool
		//
		// Queues a restore of akEffect's description to what it held before
		// this plugin's first write. False, with nothing queued, when the
		// effect was never written by SetEffectDescription in this session -
		// that is a normal answer, not a failure.
		bool ClearEffectDescription(RE::StaticFunctionTag*, RE::EffectSetting* a_effect)
		{
			try {
				if (!a_effect) {
					spdlog::warn("EffectDescription: ClearEffectDescription got a None effect - ignored.");
					return false;
				}

				const RE::FormID formID = a_effect->GetFormID();
				std::string      originalText;
				{
					std::lock_guard<std::mutex> lock(g_originalTextLock);
					auto                        it = g_originalText.find(formID);
					if (it == g_originalText.end()) {
						return false;
					}
					originalText = it->second;
				}

				auto* task = SKSE::GetTaskInterface();
				if (!task) {
					spdlog::error("EffectDescription: no task interface available - cannot queue the restore for effect (0x{:08X}).", formID);
					return false;
				}

				task->AddTask([a_effect, originalText]() {
					a_effect->magicItemDescription = originalText.c_str();
				});

				{
					std::lock_guard<std::mutex> lock(g_originalTextLock);
					g_originalText.erase(formID);
				}

				return true;
			} catch (...) {
				return false;
			}
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("EffectDescription: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("GetEffectDescription", "Lodestone", GetEffectDescription);
		a_vm->RegisterFunction("SetEffectDescription", "Lodestone", SetEffectDescription);
		a_vm->RegisterFunction("ClearEffectDescription", "Lodestone", ClearEffectDescription);

		spdlog::info("EffectDescription: natives registered (GetEffectDescription, SetEffectDescription, ClearEffectDescription).");
		return true;
	}
}
