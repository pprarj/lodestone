# Conventions and confirmed traps - C++/SKSE projects of this tree

Code conventions and confirmed engine traps, shared by every C++/SKSE plugin in
`G:\Modlists\Skyrim Dev\`.

They were born in the Lodestone as that framework's own conventions file. This
core is the part of it that is **not** framework policy - engine traps, native
function rules, hook mechanics, comment and text rules - carried out so that the
sibling plugins stop paying for them one at a time.

This file is cumulative: every trap listed here cost somebody a debugging
session. Reading it first saves repeating one.

**Why it exists at all.** The Lodestone documented "never reach a base class of
`Actor` through the C++ hierarchy", with a table of the correct accessors. A
sibling plugin in this same tree hit that exact trap anyway and paid for it from
zero - three crashes with the same register signature, a bisection, and two
decisions across two sessions. The file that would have prevented it already
existed, two folders away, and nothing carried it over.

## How this file is maintained

**The source is `_Steward\Compartilhado\CONVENTIONS-cpp.md`.** The **core** -
everything above the appendix - is verified by `sha256` and is **not edited in
place**: a correction arrives by campaign, or the source starts lying and the
copies diverge again.

**The APPENDIX belongs to the project, and is edited locally.** These are two
halves with opposite rules, and confusing them once already froze a session on
the Papyrus side of this tree: it had a correction ready for its own appendix
and did not apply it, having read that the whole file was untouchable.

The campaign invariant is the **core**, never the whole file. Copies are expected
to have different file-level `sha256` and the same core `sha256`. A divergent
appendix is not a dirty target.

Learned a new trap? It applies to everybody. Emit it in the closing report
(`phase-gate` section 9) and the orchestration brings it here.

**What belongs only to your project goes in the Appendix**, at the end - the only
section that differs between copies. If you need to change anything outside the
appendix, that is a change for everybody, and the route is a campaign.

**There are two shared cores in this tree, and they do not overlap.** This one is
C++/SKSE. `_Steward\Compartilhado\CONVENTIONS.md` is the Papyrus one. A campaign
against one never touches the other, and they do not share an appendix marker.

## Architecture

### Pure logic and engine-bound code live in separate layers.

One layer computes and can be reasoned about with the game not running; the other
one touches the engine. The directory layout mirrors the split.

**The layer names are the project's own** and are declared in the appendix. They
differ across this tree, and the same word does not mean the same thing in all of
them - so read the appendix before assuming what a folder is. In particular,
`Core` means "pure logic" in some projects here and "never knows a consumer by
name" in the Lodestone. Those are two different axes wearing one word.

Test: a file in the pure layer that includes an engine header is in the wrong
layer.

---

## Native functions

- **A native never lets a C++ exception cross into the Papyrus VM.** It is undefined behavior and can take the game down. Any fallible native wraps its body and returns a sentinel.
- **The same applies to hooks.** A C++ exception escaping a hook into the engine is undefined behavior. Thunks wrap their body.
- **Errors are reported by return value, never by throwing.** Sentinels: `Int -> -1`, `String -> "" (empty)`, `Bool -> false`.
- **The sentinel is documented in the `.psc`**, which is the contract Papyrus reads, and in a comment on the native itself stating what it returns and when it fails.
- **Every public native gets a comment** stating what it returns and when it fails, including "cannot fail" when that is the case.

---

## Reading engine object state

### Never reach a base class of `Actor` through the C++ hierarchy.

This DLL is built for SE, AE and VR from one binary. In that configuration CommonLibSSE-NG's compile-time layout is a stub that matches no runtime - `Actor.h` asserts `sizeof(Actor) == 0xC0` for the multi-runtime build against `0x2B0` (SE, VR) and `0x2B8` (AE) for the real object. No base subobject sits where the running game keeps it.

So `a_actor->GetLifeState()` compiles, links, and reads a wrong address. Nothing reports it. Use the accessor the library generates for exactly this, and which the library's own code uses everywhere:

| Reaching | Route |
| -------- | ----- |
| `ActorState` - life state, sneaking, sitting, weapon state, knock state | `a_actor->AsActorState()` |
| `MagicTarget` | `a_actor->AsMagicTarget()` |
| `ActorValueOwner` | `a_actor->AsActorValueOwner()` |
| `Actor`'s own runtime members | `a_actor->GetActorRuntimeData()` |
| `TESObjectREFR`'s runtime members | `a_refr->GetReferenceRuntimeData()` |

The accessors are declared right above the members in `Actor.h` (`RUNTIME_CAST_ACCESSOR_VERSIONED`, `RUNTIME_DATA_ACCESSOR_VERSIONED_EX`) and resolve the offset from the runtime version at call time.

**The symptom is a constant.** `Core/Incapacitation` in Lodestone shipped in 1.10.0 with the direct call, and `KnockoutActor` refused every actor in the game with the same "life state 7" - identical for a chicken at 5/5 health and a blacksmith at 131/131, across six actors, two saves and four play sessions. A value that does not vary with the actor is not a game state, it is a bad address. Four sessions went into proving that from the Papyrus side, because the DLL had no way to be asked directly; `Lodestone.GetActorLifeState` exists so the next one costs a single call.

Methods called *on* `Actor` itself are fine - `SetLifeState`, `EvaluatePackage`, `InterruptCast`, `StopCombat` and the rest resolve through the Address Library and take `this` as the engine expects. It is member and base-class *data* access that has to go through an accessor.

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

Pointed at a function prologue it reads prologue bytes as a displacement and hands back an address in no loaded module, which the thunk then calls. The failure is silent until the hooked path first runs, and then it is an access violation with no useful stack. It is a tool for redirecting an existing call site; three hooks in Lodestone were written against it as if it detoured functions, and none of them had ever run.

Detouring a function body means relocating the displaced prologue, which needs a disassembler. That is why SafetyHook is a dependency in any project that hooks a non-virtual function.

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

- **Code, comments and log messages: English.** Every project in this tree,
  public or private. A log line ends up pasted into a bug report.
- **Repository documents: pt-BR**, except in a **public** repository, where they
  are English too - a rule nobody can read is not a rule.
- Which repositories are public is declared in the project appendix.

---

## Logging

- Log file: `Documents/My Games/Skyrim Special Edition/SKSE/<project>.log`,
  derived from the CMake `project()` name. There is one source of truth for that
  name and it is `CMakeLists.txt`. The project's own log file name is declared in
  the appendix.
- Release builds log at `info`. Debug builds log everything. `spdlog::debug`
  writes nothing in a shipping build.
- **Guard the arguments, not just the call.** spdlog skips *formatting* below the
  active level, but arguments are still evaluated at the call site. In a hot path,
  a `should_log` guard is the difference between one integer compare and a virtual
  call per event.
- **Log the difference between broken and not installed.** It is the single most
  valuable thing a module writes.

---

## Internal version numbers

### A version number that named a binary that ran is spent. It does not come back.

When a phase bumps the internal version and the resulting DLL is installed and run
in game, that number has named a real artifact. Measurements quoted in a handoff or
a report refer to it. If the work is later discarded without a commit, the next
session **continues from where the phase stopped** - it does not return to the
number in the last commit. Reusing a spent number makes a record point at a binary
that is not the one it measured, which destroys the only thing an internal version
is for.

### Git hands the old number back, silently, and nothing warns you.

The version lives in a **tracked** file - `CMakeLists.txt`, `project(VERSION ...)`,
which `configure_file()` turns into `Version.h` at build time. The build directory
and the `.dll` are **gitignored**. A `git checkout` of that file therefore restores
the committed number with no trace that the range above it was ever spent.

Measured on 2026-08-26 in the Lodestone: HEAD at `1.15.1`, working tree at `1.15.7`,
six builds installed and run in game, not one commit. A discard would have silently
rewound six spent numbers.

### The floor only exists where someone writes it.

There is no counter that enforces this. Before discarding work that bumped the
version, write the floor **outside the repository**: the project handoff **and** the
session memory. The handoff alone is not enough - it is rewritten when the phase
closes.

### The test that decides, and it outlives this file's project type.

**Does the artifact the version names go into the commit?**

- **Yes** - the risk does not exist. Version and artifact travel in the same commit
  and cannot disagree. The Papyrus projects of this tree are in this case: their
  `.pex` and `.esp` are tracked.
- **No** - the version can outlive its artifact's absence. Write the floor down.

Apply the test, not the example. A future project type that ships something else
entirely is covered by the question; it is not covered by a rule that only mentions
`CMakeLists.txt`.

## Project appendix

Everything below this line belongs to this project alone and is edited locally.
Everything above it is the shared core - see `How this file is maintained`.

### Layers

| Layer | Namespace | What it is |
| ----- | --------- | ---------- |
| Core | `Lodestone::Core` | never references a consumer, its plugin file, its forms or its concepts |
| Domain | `Lodestone::Modules::<Name>` | may hardcode its own consumer's plugin file |

**This project's `Core` is not the same axis as the sibling plugins' `Core`.**
Here it means "knows no consumer by name". In Dwemer Logistics and Soul Ledger
it means "pure logic, no engine headers". Do not carry one reading into the
other.

Directory layout mirrors the namespace. No exceptions.

### Repository visibility

**Public** - `github.com/pprarj/lodestone`. Per the core's `Language` rule, every
document in this repository is in English, not only the code.

### Log file

`Lodestone.log`.

### Framework policy

The six rules below were the `Architecture` section of this file until
2026-08-24. They are this project's own - a framework with a published, versioned
Papyrus API and third-party consumers - and they do not apply to a gameplay
plugin. They moved here unchanged when the rest of this file became the shared
C++/SKSE core.

### The framework provides capability. Papyrus decides policy.

No balance value, no design decision, no tuning number lives in this DLL. The native side exposes hooks, actor values and channels; the consumer's Papyrus decides what to do with them.

Test: if changing a number here would change how a mod plays, that number is in the wrong place.

### Combining several consumers' requests is Core's job, not policy leaking in.

A channel accepts N contributors and composes them: multipliers by product, offsets by sum, applied in one pass. That looks like the rule above being broken - a decision about numbers, living in the DLL - and it is the inverse of it.

No consumer can see the other contributors to a channel. Only Core can. Combining them is therefore precisely the work nothing else is in a position to do, and refusing to do it does not avoid the decision: it just makes the decision "the first registrant wins and the rest are discarded", which is a balance outcome too, and a worse one, arrived at by load order.

Balancing stays out. **How much** each consumer asks for - its own multiplier and offset - is its Papyrus's decision. **How several requests combine** is the Core's. Changing the composition rule would change how a mod plays, so it is a published behavior under the versioning contract, not a number anyone tunes.

**A contributor's identity is derived, never asked for.** The key is `TESForm::GetFile(0)` on the multiplier global - the filename of the plugin that *created* the record, not whichever one edited it last, so a patch overriding a consumer's global does not change who the contributor is. Deriving it is what let this change ship without touching a published signature: consumers pass the same two arguments they always passed and became contributors without recompiling.

`GetFile(0)` can return null, and the fallback matters more than it looks. A global handed in from Papyrus came out of a loaded plugin, so this has never been observed - but the two obvious responses to it are both worse than a synthetic key. Rejecting the registration resurrects exactly the silent failure the multi-contributor change removed. Returning an empty key makes every such global collide, so the second registrant silently overwrites the first. The fallback keys by FormID instead: registrants stay separate and keep composing, at the price of identity no longer being per-plugin. That price is what the "unusually many contributors" warning exists to make visible - a channel accumulating FormID-shaped keys is a registration loop or a bad key, not a crowded modlist.

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
