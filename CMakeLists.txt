cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------------------------------
# Intelligence Matters - SKSE plugin proprio (Fase 16)
# Etapa A: plugin de validacao. So escreve uma linha no log do SKSE.
# ---------------------------------------------------------------------------

project(
    IntelligenceMatters
    VERSION 1.0.0
    LANGUAGES CXX
)

# CommonLibSSE-NG exige C++23.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Resolvido pela porta vcpkg (registry colorglass, ver vcpkg-configuration.json).
find_package(CommonLibSSE CONFIG REQUIRED)

# add_commonlibsse_plugin cria o alvo de DLL, configura o link do CommonLibSSE
# e gera automaticamente o boilerplate de versao do SKSE (SKSEPlugin_Version).
# O nome do arquivo de saida sera IntelligenceMatters.dll.
add_commonlibsse_plugin(
    ${PROJECT_NAME}
    AUTHOR "Awrawra"
    SOURCES
        src/plugin.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

# PCH.h e obrigatorio no CommonLibSSE-NG. Force-incluido em todos os arquivos
# do alvo, ele traz os headers do CommonLib antes de Relocation.h ser lido.
# O add_commonlibsse_plugin ja aplica /Zc:preprocessor e demais flags.
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

# ---------------------------------------------------------------------------
# Copia opcional da DLL para uma pasta de mod do MO2 apos o build.
# Descomente e ajuste o caminho quando quiser automatizar a instalacao.
# Por enquanto (Etapa A), copiar na mao para validar ja basta.
# ---------------------------------------------------------------------------
# set(IM_OUTPUT_FOLDER "G:/Modlists/Skyrim Dev/mods/Intelligence Matters DLL/SKSE/Plugins")
# if(DEFINED IM_OUTPUT_FOLDER)
#     add_custom_command(
#         TARGET ${PROJECT_NAME}
#         POST_BUILD
#         COMMAND ${CMAKE_COMMAND} -E make_directory "${IM_OUTPUT_FOLDER}"
#         COMMAND ${CMAKE_COMMAND} -E copy_if_different
#             "$<TARGET_FILE:${PROJECT_NAME}>" "${IM_OUTPUT_FOLDER}"
#     )
# endif()
