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
