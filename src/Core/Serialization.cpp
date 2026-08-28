// Serialization.cpp
// Lodestone - Shared SKSE framework
//
// Implementation of the plugin's single cosave owner. Read Serialization.h
// first - it says why one owner is a requirement and not a preference.
//
// Phase: equip veto - the second module to need the cosave.

#include "Serialization.h"

#include "EquipVeto.h"
#include "Incapacitation.h"

namespace Lodestone::Core::Serialization
{
	namespace
	{
		// The plugin's cosave identity. It stays 'LDST' - the value
		// Core/Incapacitation registered when it owned this seam. Changing it
		// would orphan every existing save's Lodestone data, which is a
		// migration nobody asked for.
		constexpr std::uint32_t kSerializationID = 'LDST';

		void SaveCallback(SKSE::SerializationInterface* a_intfc)
		{
			if (!a_intfc) {
				return;
			}

			Incapacitation::CosaveSave(a_intfc);
			EquipVeto::CosaveSave(a_intfc);
		}

		void LoadCallback(SKSE::SerializationInterface* a_intfc)
		{
			if (!a_intfc) {
				return;
			}

			// EVERY module resets first, record present or not. A module that
			// reset only on finding its own record would carry state from the
			// previous session into a save that predates it.
			Incapacitation::CosaveLoadBegin();
			EquipVeto::CosaveLoadBegin();

			std::uint32_t type = 0;
			std::uint32_t version = 0;
			std::uint32_t length = 0;
			std::uint32_t unclaimed = 0;

			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (Incapacitation::CosaveLoadRecord(a_intfc, type)) {
					continue;
				}

				if (EquipVeto::CosaveLoadRecord(a_intfc, type)) {
					continue;
				}

				// Nobody claimed it. Not an error: a newer build of this plugin
				// may write records this one has never heard of, and reading an
				// unknown record has to be survivable rather than fatal.
				++unclaimed;
			}

			if (unclaimed > 0) {
				spdlog::info("Serialization: {} cosave record(s) claimed by no module - skipped. A save "
							 "written by a newer Lodestone is the ordinary reason.",
					unclaimed);
			}
		}

		void RevertCallback(SKSE::SerializationInterface*)
		{
			Incapacitation::CosaveRevert();
			EquipVeto::CosaveRevert();
		}
	}

	void Register()
	{
		auto* intfc = SKSE::GetSerializationInterface();
		if (!intfc) {
			spdlog::error("Serialization: no serialization interface - NOTHING this plugin persists will "
						  "survive a save/reload this session. Managed knockouts and equip blocks both "
						  "start empty after every load.");
			return;
		}

		intfc->SetUniqueID(kSerializationID);
		intfc->SetSaveCallback(SaveCallback);
		intfc->SetLoadCallback(LoadCallback);
		intfc->SetRevertCallback(RevertCallback);

		spdlog::info("Serialization: cosave registered (id 'LDST'), dispatching to Incapacitation and "
					 "EquipVeto.");
	}
}
