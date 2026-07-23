# Lodestone - Plugin SKSE (DLL)

Codigo C++ nativo do Lodestone, o framework SKSE compartilhado. A raiz deste
repositorio e a raiz do projeto CMake.

## Pre-requisitos (Etapa A - Parte 1)

- Visual Studio 2022 com a workload "Desenvolvimento para desktop com C++"
  (MSVC v143, Windows 11 SDK, Ferramentas do CMake para Windows).
- vcpkg clonado e com bootstrap executado.
- Variavel de ambiente `VCPKG_ROOT` apontando para a pasta do vcpkg.

## Integracao do CommonLibSSE-NG

Via git submodule em `extern/commonlibsse-ng` (alandtse/CommonLibVR, branch ng,
pinado em tag de release). O vcpkg cobre so as dependencias transitivas
(spdlog, directxtk, rapidcsv), declaradas em `vcpkg.json`. Nao ha fonte de
terceiros commitada na arvore - o que entra no historico e o ponteiro SHA do
submodule. Reprodutibilidade vem do tag pinado + builtin-baseline fixado.

No primeiro clone (ou se `extern/commonlibsse-ng` estiver vazio), inicializar
os submodules antes de configurar:

    git submodule update --init --recursive

Historico: ate a v1.8.0 o CommonLib vinha da porta vcpkg do registry
colorglass, pinada em 3.5.3 (linha CharmedBaryon, abandonada). A migracao para
a fork do alandtse esta registrada no resumo da fase de migracao da CommonLib.

## Primeiro build

Rodar no "Developer Command Prompt for VS 2022" (traz o cmake no PATH).
Todos os comandos a partir da raiz do repositorio.

O baseline do registry default (microsoft/vcpkg) esta fixado em
`builtin-baseline` no `vcpkg.json`. Nao e preciso rodar x-update-baseline.

Nota sobre o VCPKG_ROOT: o Developer Command Prompt sobrescreve o VCPKG_ROOT
para o vcpkg embutido do VS. Para usar o vcpkg standalone controlado, force-o
no inicio da sessao:

    set VCPKG_ROOT=D:\Projetos\vcpkg

Sequencia:

1. Configurar (o vcpkg resolve as dependencias transitivas na primeira vez -
   pode levar varios minutos; o terminal pode parecer travado, e normal; o
   CommonLibSSE-NG em si compila junto com o build, a partir do submodule):

       cmake --preset vs2022-windows

2. Compilar em Release:

       cmake --build build --config Release

3. A DLL sai em:

       build/Release/Lodestone.dll

Se trocar o VCPKG_ROOT entre builds, apagar a pasta build/ antes de
reconfigurar (o CMake cacheia o caminho do toolchain):

    rmdir /s /q build

## Via Visual Studio (alternativa a linha de comando)

Abrir a pasta raiz do repositorio com "Abrir uma pasta local". O VS detecta o CMakePresets,
roda o configure automaticamente e permite build com F7. Nao usar "Criar um
projeto" nem abrir .sln - o projeto e baseado em CMake, nao em solucao.

## Instalacao para teste em jogo

Copiar `Lodestone.dll` para a pasta `SKSE/Plugins/` de um mod
gerenciado pelo MO2 (ou habilitar o bloco de copia automatica comentado no
`CMakeLists.txt`).

## Criterio de fechamento da Etapa A

Com a DLL instalada e um save limpo carregado, deve existir:

    Documents/My Games/Skyrim Special Edition/SKSE/Lodestone.log

contendo as linhas de "carregado com sucesso" e "Ambiente de plugin validado".
