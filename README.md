# Lodestone

**A shared SKSE framework for Skyrim SE mods.**

Lodestone provides low-level native infrastructure to Skyrim mods that would otherwise each ship their own DLL: engine hooks a mod can drive from Papyrus at runtime, and a version gate. It is a dependency, not a gameplay mod.

Installed on its own it does nothing - every module is passthrough until a consumer registers something with it.

It is built on [CommonLibSSE-NG](https://github.com/alandtse/CommonLibVR/tree/ng), so a single DLL covers Skyrim SE, AE and VR.

---

## Status

Lodestone was extracted from the Intelligence Matters SKSE plugin, which is its first consumer. As of this release, Intelligence Matters has completed its migration and drives all four gameplay modules directly (Cast Time, Book Framework, Spell Tomes, Magic Scaling). Every channel has been validated end to end in game, including reload behavior for the session-scoped ones. The API is small and will keep growing as more consumers arrive; the versioning contract below exists for that reason.

**1.9.0 makes every channel multi-contributor.** Until 1.8.2 a channel had one owner: the first mod to register won it for the session, and a second mod registering a different pair of globals was warned in the log and refused. That is fine while one mod uses a channel and fails the moment two do - the loser dies quietly and its author gets a bug report with no visible cause. From 1.9.0 every registrant contributes and the DLL composes them (multipliers by product, offsets by sum). **Existing consumers need to change nothing**: no signature moved, and while a mod is the only registrant the numbers it gets are the same ones it got before. See [Channels and composition](#channels-and-composition).

Currently implemented. Every module is Core - it never knows a consumer by name. The Domain layer described in CONVENTIONS exists but is currently empty.

| Module | Since | Papyrus surface |
| ------ | ----- | --------------- |
| PluginInfo | 1.0.0 | `GetVersion()`, `GetVersionString()` |
| CastTime | 1.1.0 (multi-contributor since 1.9.0) | `RegisterCastTimeChannel()` |
| BookFramework | 1.2.0 | `SetBookText()`, `AppendBookText()`, `ClearBookText()`, `GetBookText()` |
| SpellTomes | 1.3.0 (behavior changed in 1.5.0; alias registration added in 1.6.0; suppression gated on registration in 1.8.0) | `RegisterForSpellTomeRead()`, `UnregisterForSpellTomeRead()`, `RegisterForSpellTomeReadAlias()`, `UnregisterForSpellTomeReadAlias()`, `ConsumeSpellTome()`, event `OnSpellTomeRead` |
| MagicScaling | 1.4.0 (magnitude moved to the perk entry seam in 1.7.0 - now shows in the spell menu, and no longer touches enchantments, food or potions; multi-contributor since 1.9.0) | `RegisterMagicMagnitudeChannel()`, `RegisterMagicDurationChannel()`, `RegisterMagicCostChannel()` |
| Detection | 1.9.0 | `RegisterDetectionMultiplierChannel()` |
| ChannelInfo | 1.9.0 | `GetChannelContributorCount()`, `GetChannelContributorPlugin()` |
| Incapacitation | 1.10.0 (usable from 1.11.0 - see below) | `KnockoutActor()`, `WakeActor()`, `KnockoutFall()`, `KnockoutRecover()`, `IsManagedUnconscious()`, `GetActorLifeState()`, `RegisterForActorWoke()`, `UnregisterForActorWoke()`, `RegisterForActorWokeAlias()`, `UnregisterForActorWokeAlias()`, event `OnActorWoke` |

**Detection is a channel with no hook behind it yet**, and that is deliberate rather than unfinished. It scales a detection value you compute yourself, with the same shape as the magic scaling channels. Nothing in the DLL reads light, noise or movement to feed it - that needs engine investigation that has not happened, and guessing the shape of a public signature now would be carried forever. Shipping the channel first costs nothing and means the capability is born multi-contributor instead of needing a deprecation later.

**Incapacitation is a managed non-lethal knockout, with no engine hook at all.** Applying and reverting the state is a handful of calls the engine already exposes publicly (`SetLifeState`, `EvaluatePackage`, and a few interrupts) - nothing here intercepts anything. It also has no native timer: how long a knockout lasts is a balance decision, so the consumer runs its own Papyrus timer and calls `WakeActor()` when it decides the knockout ends - the same call also serves a forced wake. `IsManagedUnconscious()` answers from Lodestone's own registry rather than the engine's `IsUnconscious()`, so it distinguishes a knockout this module caused from vanilla stun causes that never touch the same life state. The registry survives a save or reload.

**The physical fall arrived in 1.12.0, as two optional calls.** Through 1.11.0 the actor stopped acting hostile without falling over. `KnockoutFall()` now drops a managed actor on the ground and `KnockoutRecover()` puts it back on its feet; a consumer that calls neither keeps exactly the behavior it had. The fall goes through the engine's own knockdown path rather than writing the animation graph's knock state by hand, which is what the module refused to do before - the engine performs the transition it already performs for every explosion in the base game. The two halves are independent: the fall calls never touch the life state, the knockout calls never touch the physical state, so `WakeActor()` and `KnockoutRecover()` may be issued in either order. The fall is animation state and does not survive a save - reapply it on load, which is safe unconditionally because `KnockoutFall()` is idempotent. Also as of 1.12.0, a managed actor that dies is dropped automatically, so `IsManagedUnconscious()` returns false for a corpse; before it answered true forever and the cosave carried the dead actor between saves.

**Gate Incapacitation on 1.11.0, not 1.10.0.** The functions exist in 1.10.0, but `KnockoutActor()` refused every actor there: it read the life state through the C++ base-class hierarchy, which in a multi-runtime build does not point at the running game's layout, so it saw the same constant for every actor. 1.11.0 reads it through `AsActorState()` and adds `GetActorLifeState()`, which reports the number a consumer would otherwise have to infer from a refusal. The trap and how to avoid it are written up in `CONVENTIONS.md`.

All functions are global natives on the `Lodestone` script, so they are called as `Lodestone.GetVersion()`. Full signatures and per-function notes are in `Lodestone.psc`, which is the authoritative reference - a consumer copies that file into its own scripts.

**Spell tomes follow the same contract as everything else since 1.8.0.** With no consumer registered, the module is pure passthrough: tomes teach their spell and are eaten exactly as in vanilla. Once a consumer registers for `OnSpellTomeRead`, reading a spell tome does nothing on its own: the spell is not taught and the book is not consumed. The book is flagged as read, since the player did open it, and that is all. The consumer then decides everything: it teaches with plain Papyrus `AddSpell` when its own system says the spell is earned, and eats the book with `ConsumeSpellTome()` if and when it should be spent. Calling both immediately in the handler reproduces vanilla exactly.

Registration order matters, as with every channel: register before the first tome read you want intercepted, and re-register on every game load. A tome read before the registration lands is vanilla - spell learned, book consumed - and is not reverted afterwards.

(From 1.5.0 to 1.7.0 the suppression was unconditional - installing the DLL alone stopped every spell tome from working. That was retired in 1.8.0: it broke vanilla tomes for anyone whose load order carries Lodestone as a mere dependency, and collided with other mods on the same seam. A consumer that relied on it must register, gated on `GetVersion() >= 1008000`.)

> **Changed in 1.5.0.** Through 1.4.0 the module kept the book but still let the spell be learned on read. A consumer written against that behavior must now teach the spell itself; gate on `Lodestone.GetVersion() >= 1005000`.

---

## Using Lodestone from Papyrus

The Lodestone was not designed to be bundled in consumer mods, but you can do so. It ships as its own download, and consumers list it as a requirement. Please note, two mods bundling different versions of the same DLL means the Mod Organizer overwrite order silently decides which one wins, and the loser breaks with no legible symptom. Therefore, it is advisable to reference it, though it is not mandatory.

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

### Channels and composition

Most of what Lodestone does is exposed as a **channel**: you own two `GlobalVariable` records - a multiplier and an offset - drive them from your own Papyrus, and hand them to the DLL once at startup and after every load. The DLL then applies them where the engine has finished computing the quantity:

```
value = (value * multiplier) + offset
```

A multiplier of `1.0` with an offset of `0.0` is a no-op, so a channel can be neutralised without unregistering it. A channel nobody has registered is pure passthrough.

Since **1.9.0** a channel takes any number of contributors, identified by the plugin the multiplier global came from. They compose:

```
multiplier total = the product of every registered multiplier
offset total     = the sum of every registered offset
result           = (value * multiplier total) + offset total
```

Applied once, not once per contributor. Registering the same pair again from the same plugin is an idempotent refresh, so re-registering on every game load costs nothing and does not double anything.

Three consequences worth stating plainly:

- **Nothing is refused.** `Register...Channel` returns `True` when *your* pair is contributing, which now includes the case where other mods are contributing too. It returns `False` only on a `None` argument.
- **While you are alone, nothing changed.** One contributor makes the totals `1.0 * multiplier` and `0.0 + offset`, which are exact in floating point. A consumer written against the single-owner versions computes the same numbers on 1.9.0.
- **You cannot see the others from your own maths, only from the diagnostics.** `GetChannelContributorCount()` and `GetChannelContributorPlugin()` report how many plugins drive a channel and which ones, for an MCM or a debug command. They are read-only on purpose: no consumer can outrank or evict another. Which mod wins is not a question this framework answers - all of them do, by composition.

Balance stays where it always was. How much *you* ask for is your Papyrus's decision; how several requests combine is the DLL's, because the DLL is the only thing that can see more than one of them.

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

No Domain module ships yet. A Domain module may gate on its own consumer's plugin file - present, it does its work; absent, it stays passthrough - a bounded, deliberate exception that never leaks into Core. `CastTime` began that way and moved to Core once its consumer coupling was replaced by runtime registration (`RegisterCastTimeChannel`): a Core module never knows a consumer by name, and takes what it needs at runtime instead.

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

CommonLibSSE-NG comes from a git submodule pinned to a release tag; vcpkg covers the transitive dependencies. On a fresh clone, initialize the submodules first:

```
git submodule update --init --recursive
```

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

- **[CommonLibSSE-NG](https://github.com/alandtse/CommonLibVR/tree/ng)**, the `ng` branch maintained by alandtse - continuing the NG line by CharmedBaryon, itself a fork of CommonLibSSE by Ryan-rsm-McKenzie and powerof3. MIT licensed.
- **[SKSE](https://skse.silverlock.org/)** by Ian Patterson, Stephen Abel and the SKSE team.
- **[spdlog](https://github.com/gabime/spdlog)** by Gabi Melman. MIT licensed.

AI assistance was used during development to speed up the work. Every line here is reviewed and owned by the author, and the reasoning behind the non-obvious parts is documented in the source where you can check it.

---

## License

MIT. See `LICENSE`.

You are free to use, modify and redistribute this, including inside your own mod's requirements. If you build something on it, a link back is appreciated but not required.
