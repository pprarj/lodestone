// ---------------------------------------------------------------------------
// Intelligence Matters - SKSE plugin proprio
// Fase 16 - Etapa A - plugin de validacao.
//
// Objetivo unico desta etapa: provar que o ambiente compila uma DLL e que o
// SKSE a carrega em jogo. O plugin so configura o log e escreve uma linha.
// Nenhuma funcionalidade do IM entra aqui - isso e Etapa C.
// ---------------------------------------------------------------------------

// RE/Skyrim.h e SKSE/SKSE.h vem do PCH.h (force-incluido pelo CMake).
#include <spdlog/sinks/basic_file_sink.h>

#include <format>

namespace {
    // Configura o logger para escrever em:
    //   Documents/My Games/Skyrim Special Edition/SKSE/IntelligenceMatters.log
    // (o SKSE resolve a pasta de log correta para o runtime em uso).
    void SetupLog() {
        auto logsFolder = SKSE::log::log_directory();
        if (!logsFolder) {
            SKSE::stl::report_and_fail("SKSE log directory not provided.");
        }

        auto pluginName = SKSE::PluginDeclaration::GetSingleton()->GetName();
        auto logFilePath = *logsFolder / std::format("{}.log", pluginName);

        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            logFilePath.string(), true);
        auto logger = std::make_shared<spdlog::logger>("global", std::move(fileSink));

        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);

        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    }
}

// SKSEPluginLoad e o entry point moderno do CommonLibSSE-NG. O boilerplate de
// versao (SKSEPlugin_Version) e gerado automaticamente por add_commonlibsse_plugin.
SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SetupLog();
    SKSE::Init(a_skse);

    auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    auto version = plugin->GetVersion();

    spdlog::info("{} v{} carregado com sucesso.", plugin->GetName(), version.string());
    spdlog::info("Ambiente de plugin validado - Fase 16 Etapa A.");

    return true;
}
