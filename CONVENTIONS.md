# Lodestone - Conventions

Rules that apply to all code and documents in this project.

---

## Architecture

### The framework provides capability. Papyrus decides policy.

No balance value, no design decision, no tuning number lives in this DLL. The native side exposes hooks, actor values and channels; the consumer's Papyrus decides what to do with them.

Test: if changing a number here would change how a mod plays, that number is in the wrong place.

### Core never knows a consumer by name. Domain may.

| Layer | Namespace | Rule |
| ----- | --------- | ---- |
| Core | `Lodestone::Core` | Never references a consumer, its plugin file, its forms or its concepts. If it cannot be described without naming a mod, it is not Core. |
| Domain | `Lodestone::Modules::<Name>` | May hardcode its own consumer's plugin file. Must be gated on that file's presence and must pass through cleanly when it is absent. |

A Domain module that fails to resolve its data is **inactive**, not broken. It logs the difference explicitly, because from the outside those two states are indistinguishable and unanswerable in a support thread.

Directory layout mirrors the namespace. No exceptions.

### The Papyrus API only grows.

Signatures never change once released. A function that turns out to be wrong is deprecated and removed years later, not corrected in place. Two consumers on different versions of the same DLL is the normal case, not an edge case.

### Behavior never changes silently.

A version bump that changes what an existing call does is a breaking change even if the signature is identical.

### The DLL version is independent of any mod version.

Bump it when the native API or native behavior changes. Not when a consumer ships a Papyrus fix.

---

## Native functions

- **A native never lets a C++ exception cross into the Papyrus VM.** It is undefined behavior and can take the game down. Any fallible native wraps its body and returns a sentinel.
- **The same applies to hooks.** A C++ exception escaping a hook into the engine is undefined behavior. Thunks wrap their body.
- **Errors are reported by return value, never by throwing.** Sentinels: `Int -> -1`, `String -> "" (empty)`, `Bool -> false`.
- **The sentinel is documented in the `.psc`**, which is the contract Papyrus reads, and in a comment on the native itself stating what it returns and when it fails.
- **Every public native gets a comment** stating what it returns and when it fails, including "cannot fail" when that is the case.

---

## Engine hooks

### The target decides the mechanism. It is not a preference.

| Target | Mechanism |
| ------ | --------- |
| Virtual function | vtable swap - `REL::Relocation<std::uintptr_t>::write_vfunc` |
| Non-virtual function | inline hook - `safetyhook::create_inline` |

**Never `Trampoline::write_branch`.** It does not detour a function body. It assumes the address it is given already holds a 5-byte rel32 branch, decodes that displacement, and returns the branch's original target:

```cpp
const auto disp   = (std::int32_t*)(a_src + N - 4);
const auto func   = (a_src + N) + *disp;
```

Pointed at a function prologue it reads prologue bytes as a displacement and hands back an address in no loaded module, which the thunk then calls. The failure is silent until the hooked path first runs, and then it is an access violation with no useful stack. It is a tool for redirecting an existing call site; three hooks in this project were written against it as if it detoured functions, and none of them had ever run.

Detouring a function body means relocating the displaced prologue, which needs a disassembler. That is why SafetyHook is a dependency.

### The original runs first, and exactly once.

Every thunk calls the original before doing anything of its own, on every path including failure. The value being adjusted is then whatever the engine finished computing, rather than something racing it - and if everything after the call fails, vanilla behavior is intact.

**Exception: a hook whose purpose is to suppress the original.** Some engine functions perform the effect rather than compute a value to be adjusted, and for those there is nothing to call first - calling the original *is* the effect. Undoing it afterwards is not equivalent and is usually not even possible: the engine does not report what the state was beforehand, so the undo cannot tell "granted by this read" from "already had it", and any UI or message the original produced has already happened.

Such a hook may skip the original, under three conditions:

1. **Only on the path that suppresses.** Every other input still calls the original and returns its answer unchanged. A hook that never calls the original for anything is a rewrite, not a hook.
2. **The thunk documents why the effect cannot be undone after the fact**, concretely - which state the engine does not hand back.
3. **Every skipped side effect is accounted for**, listed at the thunk as suppressed on purpose or restored by hand. Skipping a call skips everything it did, not only the part being suppressed, and the ones that were never considered are the ones that surface as bug reports.

`Core/SpellTomes` is the case this was written from: `TESObjectBOOK::Read` teaches the spell itself, so the only way not to teach it is not to call it. It restores `kHasBeenRead` by hand and says so.

### An address is not proven until a hook fires on it.

An address taken from anywhere other than the shipped headers is a hypothesis. An inline hook on a wrong address installs and runs quietly on the wrong function, so "it compiles and the game loads" proves nothing. Prove it with a log-only pass that shows the hook firing on the right event with coherent arguments, then switch the behavior on.

### Measure with other plugins disabled.

If another plugin hooks the same function, the "original" a thunk calls may be that plugin's thunk. Hook chaining leaves no trace in a log and the values look entirely plausible. Anything measured at a shared seam is measured with the other plugins on that seam turned off, or it is measuring them.

---

## Comments

### Document why, with evidence.

The value of this codebase is not that it works - it is that the next person can tell whether a change is safe. That requires knowing what was observed, not what was assumed.

- A non-obvious engine behavior gets the observation that established it, inline, with the concrete value: form ID, measured number, which runtime it was seen on.
- A rejected alternative that looks correct gets a note saying why it was rejected. If it was a trap once, it will look attractive again.
- Code that was removed on purpose and looks like an oversight gets a note saying it was removed on purpose.
- **Do not leave code lying around in advance of a need.** A no-op protects against nothing and costs the next reader the time it takes to work out that it does nothing. If new evidence calls for it later, it gets written later, on that evidence.

### Do not document what the code already says.

`// increments the counter` above `++counter` is noise. The bar is: would a competent reader get this wrong without the comment?

---

## Punctuation and text

- Use a **hyphen** ( - ) as a separator in comments, headers and prose.
- **Never use an em dash or an en dash** in any context.
- **Never use special characters.** ASCII only, everywhere: source, comments, log strings, Papyrus, documents.

This is not style. Non-ASCII characters cause encoding failures inside the game, and a log line that will be pasted into a bug report has to survive the trip.

---

## Language

- **Code, comments, log messages and repository documents: English.** This is a public framework with third-party consumers; a rule nobody can read is not a rule.
- Consumer-side documents in private repositories are not covered by this file.

---

## Logging

- Log file: `Documents/My Games/Skyrim Special Edition/SKSE/Lodestone.log`, derived from the CMake `project()` name. There is one source of truth for that name and it is `CMakeLists.txt`.
- Release builds log at `info`. Debug builds log everything. `spdlog::debug` writes nothing in a shipping build.
- **Guard the arguments, not just the call.** spdlog skips *formatting* below the active level, but arguments are still evaluated at the call site. In a hot path, a `should_log` guard is the difference between one integer compare and a virtual call per event.
- **Log the difference between broken and not installed.** It is the single most valuable thing a module writes.
