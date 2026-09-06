# extern/prismaui-api

This folder holds one file that is **not in the repository**: `PrismaUI_API.h`, the C++ API header of [Prisma UI](https://www.nexusmods.com/skyrimspecialedition/mods/148718) by StarkMP. It is third-party work under the Prisma UI License, so it is not redistributed here. The build needs it; the shipped DLL contains nothing of it.

Copy it here before building. Either source is the author's own:

- the optional file **Prisma UI API Header File** on the Prisma UI Nexus page, or
- `src/PrismaUI_API.h` in [PrismaUI-SKSE/example-skse-plugin](https://github.com/PrismaUI-SKSE/example-skse-plugin).

Lodestone 1.18.0 was built against the copy in `example-skse-plugin` at commit `40771e2f` (2026-03-26), which matches the Nexus header file v1.4.0 by date. The header itself says at the top: `For modders: Copy this file into your own project if you wish to use this API.`

The file is listed in `.gitignore`, so a copy placed here stays out of commits.
