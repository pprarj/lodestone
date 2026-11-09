// SkillMessage.cpp
// Lodestone - Shared SKSE framework
//
// Native behind Lodestone.ShowSkillIncreaseMessage.
//
// MECHANISM. The text comes from the game setting sSkillIncreased (so it follows
// the game language), with its first string conversion replaced by the name and
// its first integer conversion replaced by the level. The message is a HUDData of
// type kSkillIncrease posted to the HUD menu through the UIMessageQueue, the same
// way CommonLib posts kSetMode.
//
// THREAD. A Papyrus native runs on a VM thread, and nothing read in CommonLib says
// the UIMessageQueue is safe from there, so the post is handed to the SKSE UI task
// queue (AddUITask). The native therefore returns true once the message is
// QUEUED: the post itself can still find the HUD gone and is then dropped (logged
// as a warning).
//
// WHAT THE HUD READS from a kSkillIncrease message (only text? other fields?) is
// NOT READ: no CommonLib source shows it. Only type and text are set.
//
// Phase L-N1

#include "SkillMessage.h"

#include "Config.h"

#include <string>

namespace Lodestone::Core::SkillMessage
{
	namespace
	{
		bool g_logMessage = false;  // Lodestone.ini LogSkillMessage = 1

		// Builds the message from the GMST format. The format is never handed to a
		// printf-style function: only the first %s and the first %d / %i are
		// substituted, by hand, so a hostile name or format cannot misbehave.
		std::string BuildText(std::string_view a_format, std::string_view a_name, int a_level)
		{
			std::string out;
			bool        doneName = false;
			bool        doneLevel = false;
			for (std::size_t i = 0; i < a_format.size(); ++i) {
				const char c = a_format[i];
				if (c == '%' && i + 1 < a_format.size()) {
					const char n = a_format[i + 1];
					if (n == '%') {
						out += '%';
						++i;
						continue;
					}
					if (!doneName && (n == 's' || n == 'S')) {
						out += a_name;
						doneName = true;
						++i;
						continue;
					}
					if (!doneLevel && (n == 'd' || n == 'i' || n == 'D' || n == 'I')) {
						out += std::to_string(a_level);
						doneLevel = true;
						++i;
						continue;
					}
				}
				out += c;
			}
			return out;
		}

		// Runs on the UI task queue.
		void Post(const std::string& a_text)
		{
			try {
				auto* queue = RE::UIMessageQueue::GetSingleton();
				auto* strings = RE::InterfaceStrings::GetSingleton();
				if (!queue || !strings) {
					spdlog::warn("SkillMessage: no UIMessageQueue or InterfaceStrings, message dropped.");
					return;
				}
				auto* data = static_cast<RE::HUDData*>(queue->CreateUIMessageData(strings->hudData));
				if (!data) {
					spdlog::warn("SkillMessage: CreateUIMessageData returned null, message dropped.");
					return;
				}
				auto type = RE::HUD_MESSAGE_TYPE::kSkillIncrease;
#ifdef ENABLE_SKYRIM_VR
				type = RE::GetHUDMessageType(type);
#endif
				data->type = type;
				data->text = a_text.c_str();
				queue->AddMessage(RE::HUDMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kUpdate, data);
				if (g_logMessage) {
					spdlog::info("SkillMessage: posted type {} text \"{}\".", static_cast<std::uint32_t>(type), a_text);
				}
			} catch (...) {
				spdlog::warn("SkillMessage: post threw, message dropped.");
			}
		}

		// Lodestone.ShowSkillIncreaseMessage(String, Int) -> Bool
		bool ShowSkillIncreaseMessage(RE::StaticFunctionTag*, RE::BSFixedString a_name, std::int32_t a_level)
		{
			try {
				auto* gmsts = RE::GameSettingCollection::GetSingleton();
				auto* setting = gmsts ? gmsts->GetSetting("sSkillIncreased") : nullptr;
				const char* format = setting ? setting->GetString() : nullptr;
				if (!format || !*format) {
					return false;
				}
				auto* task = SKSE::GetTaskInterface();
				if (!task) {
					return false;
				}
				std::string text = BuildText(format, a_name.c_str() ? a_name.c_str() : "", a_level);
				if (g_logMessage) {
					spdlog::info("SkillMessage: queued text \"{}\".", text);
				}
				task->AddUITask([text = std::move(text)]() { Post(text); });
				return true;
			} catch (...) {
				return false;
			}
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("SkillMessage: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("ShowSkillIncreaseMessage", "Lodestone", ShowSkillIncreaseMessage);

		Config::ForEachPair([](std::string_view a_key, std::string_view a_value) {
			if (a_key == "logskillmessage") {
				g_logMessage = (Config::Trim(a_value) == "1");
			}
		});

		spdlog::info("SkillMessage: native registered (ShowSkillIncreaseMessage). Log {}.", g_logMessage ? "ON" : "off");
		return true;
	}
}
