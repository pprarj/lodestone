Scriptname Lodestone Hidden

; Lodestone - Shared SKSE framework
;
; Papyrus-facing API for the native plugin (Lodestone.dll). This script holds no
; state and declares no properties - it is a Hidden container for the plugin's
; global native functions. Consumers call them qualified: Lodestone.GetVersion().
;
; ERROR CONVENTION (mirrors the native side in PluginInfo.cpp): a native reports
; failure by return value, never by throwing. Sentinels: Int -> -1, String -> ""
; (empty), Bool -> false. Each function documents its sentinel below. If the DLL
; is not installed at all the call fails at the VM level and Papyrus yields the
; type default (0 / "") instead of the sentinel - see GetVersion.
;
; CHANNELS ARE MULTI-CONTRIBUTOR FROM 1.9.0. A "channel" is a pair of
; GlobalVariable records you own and drive, handed to the DLL through a
; Register...Channel function, applied as value = (value * multiplier) + offset.
; Up to 1.8.2 each channel had ONE owner: the first mod to register won it, and a
; second mod registering a DIFFERENT pair was refused. From 1.9.0 every distinct
; plugin that registers CONTRIBUTES, and the DLL composes them:
;
;   multiplier total = the product of every registered multiplier
;   offset total     = the sum of every registered offset
;   result           = (value * multiplier total) + offset total
;
; applied once, not once per contributor. While you are the only registrant -
; which is every consumer shipping today - the result is identical to what the
; old single-owner code produced, so nothing you have written needs to change.
; Re-registering the SAME pair from the same plugin is still a harmless
; idempotent refresh, which is what re-registering on every game load relies on.
;
; You cannot see, outrank or remove another contributor. The diagnostics at the
; end of this file tell you how many there are and which plugins they come from,
; and that is deliberately all they do.

; Returns the packed DLL version: major * 1000000 + minor * 1000 + patch.
; Example: 1.0.0 -> 1000000.
;
; Doubles as a presence and minimum-version guard. If the DLL is absent the call
; yields 0 (VM default), which is below any real version, so a single
; "GetVersion() >= required" test covers both "absent" and "too old".
;
; Cannot fail. The sentinel on native error would be -1, but this function reads
; a compile-time constant and has no error path.
Int Function GetVersion() global native

; Returns the human-readable DLL version, e.g. "1.1.0".
; For display and logging only - do NOT parse this for version gating, use
; GetVersion() instead.
;
; Cannot fail. The sentinel on native error would be "" (empty), never returned
; here.
String Function GetVersionString() global native

; --- CastTime (added in DLL 1.1.0) -----------------------------------------

; Registers the two globals that drive dynamic cast time. Call this once at
; startup (a load/init event or an init quest), BEFORE the first cast you want
; scaled. From this call on, the DLL scales the player's cast time on every
; charge as: castingTimer = castingTimer * akMultiplier.Value + akOffset.Value.
; Until it is called, cast time is untouched (vanilla passthrough).
;
; Both globals belong to YOUR mod - the DLL knows no plugin by name. Drive their
; Value fields from your own Papyrus state (INT, fatigue, spell tier, MCM, etc.).
;
; MULTI-CONTRIBUTOR (DLL 1.9.0+): every distinct plugin that registers a
; DIFFERENT pair contributes to the channel - multipliers compose by product,
; offsets compose by sum, applied once. Re-registering the SAME pair from the
; same plugin remains a harmless idempotent refresh. Before 1.9.0 a second,
; different registrant was rejected; from 1.9.0 on it is ACCEPTED and composed. A
; consumer written against the old single-channel behavior sees no change while
; it is the only registrant.
;
; Returns True when YOUR pair is contributing to the channel after this call
; (which includes the case where other plugins are also contributing). Returns
; False on a None argument. If the DLL is absent the call yields the VM default
; False as well.
Bool Function RegisterCastTimeChannel(GlobalVariable akMultiplier, GlobalVariable akOffset) global native

; --- BookFramework (added in DLL 1.2.0) ------------------------------------

; Supply runtime-built text for a book. When akBook is opened, the DLL shows the
; text you stored instead of the book's own text. Identify the book by its own
; Book form; the DLL keys the text by that form and knows no mod by name.
;
; The stored text lives for the SESSION only - it is not saved. Re-establish a
; book's text after each load (call SetBookText from your own Papyrus state),
; the same runtime model the cast time channel uses. Set it before the book is
; opened for the new text to show. A book with no stored text opens unchanged
; (vanilla text). Book text over 64 KB is truncated - the engine's own limit.

; Replaces the stored text for akBook.
; Returns True on success, False on a None book (or the VM default False if the
; DLL is absent).
Bool Function SetBookText(Book akBook, String asText) global native

; Appends asText to akBook's stored text, starting fresh if none is stored yet.
; Returns True on success, False on a None book.
Bool Function AppendBookText(Book akBook, String asText) global native

; Drops akBook's stored text; the book reverts to its own text.
; Returns True on success, False on a None book.
Bool Function ClearBookText(Book akBook) global native

; Reads back akBook's stored text, or "" if none is stored (including after a
; load, until you re-set it). For display or bookkeeping - not a version gate.
; Returns "" on a None book, or the VM default "" if the DLL is absent.
String Function GetBookText(Book akBook) global native

; --- SpellTomes (added in DLL 1.3.0, full interception since 1.5.0, ---------
; --- suppression gated on registration since 1.8.0) ------------------------

; Once at least one receiver is registered, reading a spell tome does NOTHING
; on its own: the spell is not learned and the book is not consumed. The book
; is flagged as read, because the player did open it, and that is all that
; happens. With NO receiver registered the module is pure passthrough: tomes
; teach their spell and are eaten exactly as in vanilla, as if the DLL were
; not installed.
;
; ORDERING - same requirement as RegisterCastTimeChannel: register BEFORE the
; first tome read you want intercepted, and re-register on every game load
; (registration is session-scoped). A tome read before your registration lands
; is vanilla - spell learned, book consumed - and is NOT reverted afterwards.
;
; CHANGED IN 1.8.0: from 1.5.0 to 1.7.0 the suppression was unconditional -
; installing the DLL alone stopped every spell tome from working. If your
; script relied on suppression happening without any registration, it must now
; register (and it should have anyway, to be told about the read). Gate on
; Lodestone.GetVersion() >= 1008000.
;
; To be told when a tome is read, register the form
; or the alias whose script implements OnSpellTomeRead - RegisterForSpellTomeRead
; for a form, RegisterForSpellTomeReadAlias for an alias script - and then decide
; everything yourself:
;
;   - to teach the spell, call akBook.GetSpell() and AddSpell on the reader,
;     whenever your own system says the spell is earned. There is no Lodestone
;     native for this and none is needed - it is plain Papyrus.
;   - to eat the book, call ConsumeSpellTome. If nothing ever calls it, the
;     tome stays in the inventory.
;
; Vanilla behavior is reproducible: call both immediately in your handler.
;
; NOTE FOR 1.4.0 AND EARLIER CONSUMERS: up to 1.4.0 the DLL kept the book but
; still let the spell be learned on read. If your script assumed the spell was
; already known when OnSpellTomeRead fired, it no longer is, and teaching is
; now your call. Gate on Lodestone.GetVersion() >= 1005000.

; Registers akReceiver's script to receive OnSpellTomeRead. Registration is
; session-scoped (not saved) - re-register after each load. akReceiver is any
; form carrying the script (a quest, a magic effect, a reference). For a handler
; on an alias script, use RegisterForSpellTomeReadAlias instead.
; Returns True on success, False on a None form.
Bool Function RegisterForSpellTomeRead(Form akReceiver) global native

; Stops akReceiver from receiving OnSpellTomeRead.
; Returns True on success, False on a None form.
Bool Function UnregisterForSpellTomeRead(Form akReceiver) global native

; Registers akAlias's script to receive OnSpellTomeRead. Use this when the
; handler lives on an alias script (e.g. a ReferenceAlias) - a Form-keyed
; registration cannot reach an alias, and a Papyrus Alias has no Form cast.
; Registration is session-scoped (not saved) - re-register after each load.
; Requires Lodestone.GetVersion() >= 1006000 (1.6.0).
; Returns True on success, False on a None alias.
Bool Function RegisterForSpellTomeReadAlias(Alias akAlias) global native

; Stops akAlias's script from receiving OnSpellTomeRead.
; Returns True on success, False on a None alias.
Bool Function UnregisterForSpellTomeReadAlias(Alias akAlias) global native

; Removes one copy of akBook from akActor - the explicit "consume the tome now"
; call. Use it when your own logic decides the book should be eaten. Nothing in
; the DLL consumes a tome on its own.
; Returns True on success, False on a None argument.
Bool Function ConsumeSpellTome(Book akBook, ObjectReference akActor) global native

; --- Event, implemented by a registered script -----------------------------
; Sent to each form registered via RegisterForSpellTomeRead and each alias
; registered via RegisterForSpellTomeReadAlias, every time a spell tome is read.
; akBook is the tome, akReader is who read it (normally the player).
;
; When this fires the spell has NOT been learned and the book has NOT been
; consumed - both are yours to decide. The event carries no return value and
; cannot: Papyrus events do not have one, and this dispatch is asynchronous, so
; the DLL is long done by the time your handler runs. It never waits to be told
; what to do; it suppresses, reports, and you act.
;
; Declare it in your script exactly as below:
;
;   Event OnSpellTomeRead(Book akBook, ObjectReference akReader)
;       ; nothing has happened yet. Start a study session, open a menu, check a
;       ; gate - and teach with AddSpell / eat with ConsumeSpellTome if and when
;       ; you decide to.
;   EndEvent


; --- Magic scaling (L3) ----------------------------------------------------
; Scales a spell's magnitude, duration and magicka cost by values you drive from
; your own script. Three independent channels; each is passthrough until you
; register it, so registering only one scales only that one. Formula per
; channel: value = (value * multiplier) + offset. A multiplier of 1.0 with an
; offset of 0.0 is a no-op, so you can neutralise a channel without
; unregistering it.
;
; Scope: ordinary castable spells only - not abilities, enchantments or powers.
; Player only. Only values that already exist are scaled: an effect with no
; magnitude keeps none, and a spell that costs nothing keeps costing nothing.
;
; Registration is session-scoped (not saved) - re-register after each load.
;
; MULTI-CONTRIBUTOR (DLL 1.9.0+), per quantity: every distinct plugin that
; registers a DIFFERENT pair contributes to that channel - multipliers compose by
; product, offsets compose by sum, applied once. Re-registering the SAME pair
; from the same plugin remains a harmless idempotent refresh. Before 1.9.0 a
; second, different registrant was rejected; from 1.9.0 on it is ACCEPTED and
; composed. A consumer written against the old single-channel behavior sees no
; change while it is the only registrant.
;
; Requires Lodestone.GetVersion() >= 1004000 (1.4.0).
; All three return True when YOUR pair is contributing to the channel after the
; call (which includes the case where other plugins are also contributing), and
; False on a None argument.

; Scales the magnitude of a spell's effects.
Bool Function RegisterMagicMagnitudeChannel(GlobalVariable akMultiplier, GlobalVariable akOffset) global native

; Scales the duration of a spell's effects.
Bool Function RegisterMagicDurationChannel(GlobalVariable akMultiplier, GlobalVariable akOffset) global native

; Scales a spell's magicka cost. Use a multiplier below 1.0 to make casting
; cheaper.
Bool Function RegisterMagicCostChannel(GlobalVariable akMultiplier, GlobalVariable akOffset) global native

; --- Detection scaling channel (added in DLL 1.9.0) -----------------------
;
; Scales a detection-related value you compute yourself, the same shape as the
; magic scaling channels: value = (value * multiplier) + offset.
; MULTI-CONTRIBUTOR from the start - see the note at the top of this file.
;
; This channel does NOT read game state for you. There is no hook yet that feeds
; it environmental context (light, noise, movement) - that is a separate, future
; capability. Today this channel is infrastructure only: register it, drive your
; own globals from your own Papyrus logic, and the DLL composes correctly when
; more than one consumer registers.
;
; Registration is session-scoped (not saved) - re-register after each load.
;
; Requires Lodestone.GetVersion() >= 1009000 (1.9.0).
; Returns True when YOUR pair is contributing to the channel after the call,
; False on a None argument.
Bool Function RegisterDetectionMultiplierChannel(GlobalVariable akMultiplier, GlobalVariable akOffset) global native

; --- Diagnostics (added in DLL 1.9.0) ---------------------------------
;
; Read-only. They tell you who else is driving a channel; they give you no way to
; change it. Which contributor "wins" is not a question this framework answers -
; all of them do, by composition.
;
; Returns how many distinct plugins currently contribute to asChannel.
; Valid asChannel values: "CastTime", "MagicMagnitude", "MagicDuration",
; "MagicCost", "Detection". Unknown name or DLL absent -> -1. Zero is a real
; answer: the channel exists and nobody has registered on it.
Int Function GetChannelContributorCount(String asChannel) global native

; Returns the source plugin filename of the contributor at aiIndex
; (0-based, order not guaranteed stable across registrations). Out-of-range
; index, unknown channel name, or DLL absent -> "".
String Function GetChannelContributorPlugin(String asChannel, Int aiIndex) global native

; --- Incapacitation (added in DLL 1.10.0) ----------------------------------
;
; A managed non-lethal knockout: mark an actor as managed-unconscious, query
; that state, and wake the actor - automatically (your own Papyrus timer) or
; forced. Built for a stealth takedown feature, but nothing here is specific
; to any one consumer.
;
; IsManagedUnconscious answers from Lodestone's OWN registry, not from the
; vanilla Actor.IsUnconscious() - so it distinguishes "this module knocked
; this actor out" from "this actor is stunned by something else" (magic
; paralysis, Unrelenting Force, etc), which never set the same life state.
;
; NO DURATION LOGIC LIVES HERE. How long a knockout lasts is a balance
; decision, and this framework does not make those - see the note at the top
; of this file. Drive your own timer (RegisterForSingleUpdate, re-armed on
; every load, the same pattern this file already expects of you for anything
; timer-based) and call WakeActor when you decide the knockout ends. The same
; WakeActor also serves a forced wake - the only difference is who calls it
; and when.
;
; V1 HAS NO PAIRED ANIMATION. The actor stops acting hostile and its AI
; package is re-evaluated on waking, but it does not physically fall over.
; An animated knockout is a separate, future capability.
;
; PERSISTENCE: the set of managed actors survives a save/reload (this module
; keeps its own cosave record). The consumer's own timer state is Papyrus's
; responsibility, as always - re-arm it on load like any other timer.
;
; Registration for OnActorWoke is session-scoped (not saved) - re-register
; after each load, the same runtime model the other modules use.
;
; Requires Lodestone.GetVersion() >= 1010000 (1.10.0) for every function below.

; Marks akActor as managed-unconscious: interrupts what it is doing and sets
; it to stop acting hostile. Refuses (returns False) a None actor, a dead
; actor, an actor not currently in a plain alive state (already bleeding
; out, restrained, etc - this module does not stack on another life state),
; or an actor already managed by this module. Never throws.
Bool Function KnockoutActor(Actor akActor) global native

; Ends a managed-unconscious state, automatically-timed or forced - the same
; call either way. Safe and a harmless False on an actor this module was not
; managing (nothing is touched). Dispatches OnActorWoke when it does wake a
; managed actor. Never throws.
Bool Function WakeActor(Actor akActor) global native

; Is akActor currently managed-unconscious by THIS module? Distinct from the
; engine's own IsUnconscious() - see the note above. None actor -> False.
Bool Function IsManagedUnconscious(Actor akActor) global native

; Registers a form whose script implements OnActorWoke(Actor akActor).
Bool Function RegisterForActorWoke(Form akReceiver) global native

; Reverses RegisterForActorWoke.
Bool Function UnregisterForActorWoke(Form akReceiver) global native

; Registers a ReferenceAlias whose script implements
; OnActorWoke(Actor akActor). Needed for alias-bound consumers - see
; RegisterForSpellTomeReadAlias above for why a Form-keyed registration
; cannot reach an alias script.
Bool Function RegisterForActorWokeAlias(Alias akAlias) global native

; Reverses RegisterForActorWokeAlias.
Bool Function UnregisterForActorWokeAlias(Alias akAlias) global native

; Dispatched when a managed-unconscious actor wakes, automatically or
; forced. A script that registered via RegisterForActorWoke or
; RegisterForActorWokeAlias implements this to find out - declare it in
; YOUR OWN script as:
;
;   Event OnActorWoke(Actor akActor)
;       ; your handler
;   EndEvent
