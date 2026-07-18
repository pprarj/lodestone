# Lodestone

**A shared SKSE framework for the Matters mods.**

Lodestone provides low-level native infrastructure to Skyrim mods that would otherwise each ship their own DLL: custom actor values, per-instance ExtraData, engine hooks, localized strings, and a version gate. It is a dependency, not a gameplay mod. Installed on its own it does nothing.

It is built on [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG), so a single DLL covers Skyrim SE, AE and VR.

---

## Status

**Pre-release.** Not yet published on Nexus.

Lodestone was extracted from the Intelligence Matters SKSE plugin, which is its first consumer. The API is small and will grow. Until the first Nexus release, treat everything here as unstable.

Currently implemented:

| Module | Layer | Papyrus surface |
| ------ | ----- | --------------- |
| PluginInfo | Core | `Lodestone.GetVersion()`, `Lodestone.GetVersionString()` |
| CastTime | Domain (Intelligence Matters) | none - engine hook, driven by globals |

---

## Using Lodestone from Papyrus

Lodestone is not bundled inside consuming mods, ever. It ships as its own download, and consumers list it as a requirement. Two mods bundling different versions of the same DLL means the Mod Organizer overwrite order silently decides which one wins, and the loser breaks with no legible symptom.

### The version gate

Every consumer declares a minimum Lodestone version and checks it at startup.

```papyrus
Int Function MinimumLodestoneVersion() global
    Return 1000000  ; 1.0.0
EndFunction

Function CheckLodestone()
    Int found = Lodestone.GetVersion()
    If found < MinimumLodestoneVersion()
        ; Fail loudly. Do not continue, do not log quietly to the Papyrus log.
        Debug.MessageBox("MyMod requires Lodestone " + ... + " or newer.")
        Return
    EndIf
EndFunction
```

`GetVersion()` returns the version packed as `major * 1000000 + minor * 1000 + patch`, so `1.0.0` is `1000000`.

If the DLL is missing or failed to load, the native call fails at the VM level and Papyrus yields `0`, which is below any real version. A single `>=` check therefore covers both "absent" and "too old". Do not parse `GetVersionString()` for gating - it exists for display and logs.

### Versioning contract

Once two mods depend on the same DLL, a user running one of them updated and the other outdated is guaranteed, not hypothetical. So:

1. **The Papyrus API only grows.** Signatures never change. A function that turns out to be wrong gets deprecated, not corrected in place.
2. **Behavior never changes silently.** No balance tuning lives in this DLL.
3. **The DLL version is independent of any mod version on Nexus.** It is bumped when the native API or native behavior changes.

---

## For mod authors: what Lodestone is not

> **The framework provides capability. Papyrus decides policy.**

No balance value lives here. Lodestone exposes hooks, actor values and channels; what to do with them is the consumer's Papyrus. If a feature only makes sense for one mod's design, it does not belong in Core.

### Core and Domain

Modules live in one of two layers, and the distinction is enforced, not aspirational:

| Layer | Rule |
| ----- | ---- |
| **Core** (`Lodestone::Core`) | **Never** knows a consumer by name. PluginInfo, Log, the Papyrus dispatcher, actor values, localization, ExtraData. |
| **Domain** (`Lodestone::Modules`) | **May** hardcode its own consumer's plugin file. Gated on that file's presence, passthrough when absent. |

`CastTime` is a Domain module: it resolves two globals out of `IntelligenceMatters_CastTime.esp` and does nothing at all if that file is not installed. This is a bounded, deliberate exception, documented in the module itself. It is not a pattern to copy into Core.

### Native API conventions

- A native never lets a C++ exception cross into the Papyrus VM. Escaping into the VM is undefined behavior and can take the game down. Fallible natives wrap their body and return a sentinel.
- Errors are reported by return value, never by throwing: `Int -> -1`, `String -> ""`, `Bool -> false`. The sentinel is documented per function in the `.psc`, which is the contract Papyrus reads.
- The same applies to hooks. Nothing this plugin installs may throw across an engine boundary.

---

## Building

Requirements:

- Visual Studio 2022 with the C++ toolset (C++23)
- CMake 3.21+
- [vcpkg](https://github.com/microsoft/vcpkg), with `VCPKG_ROOT` set

Dependencies resolve through the Color-Glass Studios vcpkg registry, configured in `vcpkg-configuration.json`. There is no submodule to initialize.

```
cmake --preset <preset>
cmake --build build --config Release
```

To have the build copy the DLL straight into a mod folder, set `LODESTONE_OUTPUT_FOLDER` - do not edit the default in `CMakeLists.txt`:

```
cmake --preset <preset> -DLODESTONE_OUTPUT_FOLDER="C:/path/to/mods/Lodestone/SKSE/Plugins"
```

The plugin writes to `Documents/My Games/Skyrim Special Edition/SKSE/Lodestone.log`. Release builds log at `info`; debug builds log everything.

---

## Contributing

Read `CONVENTIONS.md` first. It is short.

The one thing worth knowing up front: this codebase documents *why*, with evidence. Hook targets, rejected alternatives and non-obvious engine behavior are explained inline, citing the trace that established them. A change that alters behavior is expected to say what it observed, not what it assumed.

---

## Credits

- **[CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG)** by CharmedBaryon, a fork of CommonLibSSE by Ryan-rsm-McKenzie and powerof3. MIT licensed.
- **[SKSE](https://skse.silverlock.org/)** by Ian Patterson, Stephen Abel and the SKSE team.
- **[spdlog](https://github.com/gabime/spdlog)** by Gabi Melman. MIT licensed.
- The Color-Glass Studios vcpkg registry, which makes the above consumable without submodules.

AI assistance was used during development to speed up the work. Every line here is reviewed and owned by the author, and the reasoning behind the non-obvious parts is documented in the source where you can check it.

---

## License

MIT. See `LICENSE`.

You are free to use, modify and redistribute this, including inside your own mod's requirements. If you build something on it, a link back is appreciated but not required.
