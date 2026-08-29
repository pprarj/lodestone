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
;
; MARKUP. What you store is book markup - the same the Creation Kit puts in a
; book's own text - and the engine treats injected text the same way it treats a
; record's. Measured in game on injected text:
;
;   <p align="center"> / <p align="left">    aligns the paragraph
;   <font face="$HandwrittenFont">           changes the face, and returns
;   [pagebreak]                              forces a new page
;
; [pagebreak] NEEDS A REAL LINE BREAK ON EACH SIDE, alone on its own line. That
; is the same authoring rule a normal book follows, and it is not this plugin's
; addition. Glued to the end of a sentence or to the start of a tag it is not a
; page break at all - it prints as eleven literal characters.
;
; AND THE TRAP IS IN PAPYRUS, NOT IN THE BOOK. Concatenation produces no line
; break. This LOOKS like it obeys the rule above and does not:
;
;   t += "[pagebreak]"
;
; The token sits alone in its own statement, but the pieces join into one running
; line and it ends up glued between a full stop and a tag. Put the breaks inside
; the string:
;
;   t += "\n[pagebreak]\n"
;
; A readable source is not a formatted string. If a page break does not happen,
; read the text back with GetBookText and look for the line break there before
; suspecting anything else - check the string, not the source that built it.

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

; --- Detection reading (added in DLL 1.14.0; filtered pair added in 1.15.0) -
;
; The engine keeps a detection score for every actor that is currently paying
; attention to another one. These four functions report that score, in two
; shapes - the engine's own ready-made aggregate, and a DLL-built one that can
; exclude observers by keyword. None of them change anything: see
; RegisterDetectionMultiplierChannel above for the composition channel, which
; is a separate thing and still applies to nothing on its own.
;
; SCALE (corrected in 1.15.0 - the 1.14.0 text is kept below, struck through
; in prose, so nobody "fixes" this back to it). The value is SIGNED and zero
; is the boundary: negative means not yet detected, zero or above means
; detected - that much was right. What was wrong: the 1.14.0 text described a
; band of -100 to 0 that the number climbs through gradually. In play it
; JUMPS instead - a floor around -1000 while nobody is paying attention, then
; straight to roughly -20 to 0 the instant someone notices, with nothing
; observed in between. A threshold sized at the midpoint of "-100 to 0" (a
; consumer did exactly this, at -50) lands at roughly 95% of the way to
; "detected", not halfway. Do not interpolate across this value - branch on
; it instead (unnoticed / noticed, then how close to zero within the noticed
; band).
;
; Lodestone reports the engine's own number and does NOT normalize it: the
; exact width and shape of the band is still not fully confirmed - the -1000
; floor is observed in play, not documented in any header - and a published
; signature cannot be taken back. If you want a percentage, clamp and scale
; it yourself, and pick your own floor deliberately rather than assuming
; -100.
;
; FRESHNESS. The value is read on the game thread and cached by the DLL; a
; call returns the most recent reading, not a fresh computation. Before the
; first reading lands the sentinel is returned. Do not build anything on
; sub-second latency - this is a poll-grade reading.
;
; THE LEVEL AND THE COUNT ARE TWO SAMPLES, NOT ONE ATOMIC READING (corrected
; in 1.15.1). Each pair below is fed by a single reading, so within ONE call
; the number you get is self-consistent. But you read the pair as two
; separate calls, and the DLL's refresh runs on the game thread - it can
; complete BETWEEN your two calls, handing you the count from one reading and
; the level from the next.
;
; The visible symptom is a level at or above zero ("detected") sitting next
; to a count of zero, which looks like a contradiction and is not: it is the
; instant the reading flipped, caught halfway. Earlier versions of this
; comment claimed the pair could never disagree. That was wrong, and it is
; recorded here so nobody restores the claim.
;
; If your logic cannot tolerate that, branch on ONE of the two - the level
; alone already carries the detected/not-detected boundary - rather than
; requiring the two to agree.
;
; Requires Lodestone.GetVersion() >= 1014000 (1.14.0).

; The highest detection level any actor currently has against akActor.
; None actor, actor with no active AI process, or cache not yet warm -> -1
; (which is also a legal real value: check GetDetectionObserverCount to tell
; "nobody is looking" from "not measured yet").
Int Function GetHighestDetectionLevel(Actor akActor) global native

; How many actors currently have line of sight to akActor, from the same
; reading as GetHighestDetectionLevel. None actor or cache not yet warm -> -1.
; Zero is a real answer.
Int Function GetDetectionObserverCount(Actor akActor) global native

; --- Filtered detection reading (added in DLL 1.15.0) ----------------------
;
; Same SCALE and FRESHNESS as the pair above. The difference: these two build
; their OWN aggregate DLL-side, skipping any observer that carries
; akExcludeType, instead of reading the engine's ready-made one. That answers
; "is a HUMANOID watching me", excluding wildlife with a keyword like
; ActorTypeAnimal - something the unfiltered pair cannot do, because an Int
; has no identity attached and there is nothing on your side to filter.
;
; NOT PROMISED TO MATCH THE UNFILTERED PAIR, even with akExcludeType set to
; None. This aggregate walks its own candidate set through a different engine
; call and does not reproduce whatever internal logic the engine's own
; aggregate uses beyond taking a maximum. Treat the two pairs as related, not
; interchangeable.
;
; BESIDES akExcludeType, THE WALK ALWAYS SKIPS three kinds of candidate, and
; you get this whether you filter by keyword or not: akActor itself, dead
; actors, and any actor the engine flags as not affecting the stealth meter.
; The last two were added in 1.15.1 - before that a corpse left in the cell
; could keep reporting the level it had when it died, and an actor the
; vanilla stealth eye ignores could push this reading to "detected" while the
; eye stayed open.
;
; GetDetectionObserverCountExcluding is NOT a line-of-sight count - the
; engine call this pair uses has no such output. It counts how many
; keyword-filtered candidates currently DETECT akActor (level >= 0), which is
; a different question from "how many can see" it.
;
; "Nobody survived the filter" reports the same floor the unfiltered pair
; shows for "nobody is watching" (see the SCALE note above), on purpose - so
; you do not learn a third convention on top of the sentinel and the real
; scale.
;
; Requires Lodestone.GetVersion() >= 1015000 (1.15.0).

; The highest detection level any actor NOT carrying akExcludeType currently
; has against akActor. akExcludeType may be None (no exclusion applied - see
; above for why the result can still differ from GetHighestDetectionLevel).
; None akActor, actor with no active AI process, or cache not yet warm -> -1.
Int Function GetHighestDetectionLevelExcluding(Actor akActor, Keyword akExcludeType) global native

; How many akExcludeType-filtered candidates currently detect akActor - see
; above for why this is a detection count, not a line-of-sight count. None
; akActor, no active AI process, or cache not yet warm -> -1. Zero is a real
; answer.
Int Function GetDetectionObserverCountExcluding(Actor akActor, Keyword akExcludeType) global native

; --- Incapacitation (added in DLL 1.10.0, usable from 1.11.0) --------------
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
; THE PHYSICAL FALL IS OPTIONAL AND SEPARATE. KnockoutActor on its own
; pacifies the actor without dropping it - that is what it has always done
; and it has not changed. To also put the actor on the ground, call
; KnockoutFall after it, and KnockoutRecover to get the actor back up. A
; consumer that never calls either keeps exactly the behavior it had before
; those two existed.
;
; The two halves are independent on purpose: KnockoutFall/KnockoutRecover
; never touch the life state, and KnockoutActor/WakeActor never touch the
; physical state. You may call WakeActor and KnockoutRecover in either
; order.
;
; PERSISTENCE: the set of managed actors survives a save/reload (this module
; keeps its own cosave record). The consumer's own timer state is Papyrus's
; responsibility, as always - re-arm it on load like any other timer.
;
; THE PHYSICAL FALL DOES NOT SURVIVE A SAVE. It is animation state, and the
; game does not save it. An actor you knocked down comes back from a reload
; standing up, still managed. Reapply KnockoutFall on load to every actor
; you still consider knocked out - KnockoutFall is idempotent, so you do not
; have to work out first whether it is still needed.
;
; A MANAGED ACTOR THAT DIES is dropped automatically: IsManagedUnconscious
; returns False for a corpse and nothing is carried between saves. No
; OnActorWoke is dispatched for it, because it did not wake.
;
; Registration for OnActorWoke is session-scoped (not saved) - re-register
; after each load, the same runtime model the other modules use.
;
; Requires Lodestone.GetVersion() >= 1011000 (1.11.0). The functions below
; first appeared in 1.10.0, but KnockoutActor refused every actor in that
; version - it read the life state through the wrong offset and saw a
; constant. Gate on 1011000, not on 1010000: 1.10.0 answers the version check
; and then knocks nobody out.

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

; Registers akActor for the physical fall. It applies NOTHING by itself -
; the fall lands when Actor.SetUnconscious(True) runs on a registered actor,
; and without that call this is a no-op. Refuses (returns False) a None actor
; or a dead one. Never throws.
;
; It does NOT require the actor to be managed by KnockoutActor, and as of
; 1.12.6 it no longer refuses one that is not - you may call it before or
; after KnockoutActor, or without KnockoutActor at all. KnockoutRecover works
; off this call's own record either way.
;
; IDEMPOTENT: calling it on an actor already registered does nothing and
; returns True. That is what makes it safe to reapply after a game load
; without checking anything first - see PERSISTENCE above.
;
; AS OF 1.13.0 THIS CALL ARMS THE FALL, AND YOU FINISH IT FROM PAPYRUS:
;
;     If Lodestone.KnockoutFall(akTarget)
;         akTarget.SetUnconscious(True)
;     EndIf
;
; The reason is not style. Everything this call can do from the outside was
; tried across eight versions and none of it held the actor down, because a
; Papyrus call lands between engine frames and the state settles before the
; next one runs. The work has to happen inside the engine's own unconscious
; handler, and the only way to get there is for something to call
; Actor.SetUnconscious - a native that exists in Papyrus and nowhere else.
;
; So this call registers the actor, and Lodestone does the real work from
; inside the handler when SetUnconscious runs on an actor it registered.
; Nothing happens to an actor you did not register: install Lodestone next to
; a mod that calls SetUnconscious and that mod behaves exactly as it always
; did.
;
; WAKING IS THE MIRROR, AND THE ORDER MATTERS THERE:
;
;     akTarget.SetUnconscious(False)      ; FIRST - releases from inside
;     Lodestone.KnockoutRecover(akTarget) ; then clean up
;
; SetUnconscious(False) comes first because the release happens inside the
; same handler the fall used, and it only recognises actors still registered.
; KnockoutRecover unregisters, so calling it first leaves nothing for the
; handler to release. It does write a get-up state itself as a fallback, but
; that is the half that never held on its own - do not rely on it.
;
; Getting this order wrong leaves the actor on the ground permanently. If you
; can only make one call, make it SetUnconscious(False).
;
; There is no strength or duration parameter, and there will not be one.
; How long the knockout lasts is your timer's decision, as always; how hard
; the actor falls is not a decision anybody needs to make.
;
; Requires Lodestone.GetVersion() >= 1012000 (1.12.0).
Bool Function KnockoutFall(Actor akActor) global native

; Gets akActor back on its feet, resyncing the 3D model and re-evaluating
; the AI package. A harmless False on an actor KnockoutFall never knocked
; down - nothing is touched. Leaves a corpse alone. Never throws.
;
; Order against WakeActor does not matter when the actor is managed: in that
; case KnockoutActor moved the life state and WakeActor moves it back, and
; this call does not touch it.
;
; When KnockoutFall ran on an actor that was NOT managed, this call also
; restores the life state, because nothing else would - WakeActor refuses an
; unmanaged actor. That is the only case where this call touches it, and it
; is what keeps the unmanaged path from leaving an actor pacified forever.
;
; ONE HOLE, AND IT IS HONEST: that ownership is remembered in memory only.
; If you knock an actor down WITHOUT KnockoutActor and then save and reload
; before waking it, the record is gone and this call will not restore the
; life state. Use the managed path (KnockoutActor first) for anything that
; has to survive a reload, or restore it yourself in your load handler.
;
; Requires Lodestone.GetVersion() >= 1012000 (1.12.0).
Bool Function KnockoutRecover(Actor akActor) global native

; Is akActor currently managed-unconscious by THIS module? Distinct from the
; engine's own IsUnconscious() - see the note above. None actor -> False.
; Returns False for a corpse: a managed actor that dies is dropped
; automatically as of 1.12.0.
Bool Function IsManagedUnconscious(Actor akActor) global native

; The raw engine life state of akActor, as a number. This is a diagnostic
; call, not a gameplay one: it exists so you can see the exact value
; KnockoutActor is judging, instead of inferring it from a refusal.
;
;   0 alive   1 dying    2 dead        3 unconscious   4 reanimate
;   5 recycle 6 restrained  7 essential down  8 bleedout
;
; KnockoutActor accepts 0 and refuses everything else. None actor -> -1.
; Requires Lodestone.GetVersion() >= 1011000 (1.11.0).
Int Function GetActorLifeState(Actor akActor) global native

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

;==============================================================
; EQUIP VETO - since 1.16.0
;==============================================================
;
; Refuses an item on an actor at the point where the game equips it, and
; puts a replacement on instead. No equip means no unequip, no reactive
; loop, and no window in which the actor is wearing something it should
; not.
;
; You decide WHICH items. Lodestone only performs the refusal and the swap.
;
; Gate on GetVersion() >= 1016000.
;
; NOT AVAILABLE IN VR. The hook is not installed on a VR runtime and the
; log says so plainly. These functions still exist there and still register
; blocks, and nothing is refused. Check the log, not the return value: a
; successful BlockEquip on VR means the block was recorded, not enforced.
;
; --------------------------------------------------------------------
; FOUR THINGS THAT DECIDE WHETHER THIS WORKS FOR YOU
; --------------------------------------------------------------------
;
; 1. THE SUBSTITUTE IS NOT OPTIONAL IN PRACTICE.
;
; Passing None as akSubstitute is legal and refuses cleanly. It is also,
; almost always, not what you want - and this was measured rather than
; guessed.
;
; The game's own equip call returns nothing, so a refusal is SILENT. An
; NPC's AI picks the best item it OWNS before trying to equip anything, is
; never told the attempt failed, and goes on picking the same blocked item
; on every pass. With no substitute the actor keeps whatever its base
; outfit put on and NEVER upgrades to the allowed piece it already carries.
;
; Measured on a follower: given a blocked helmet, he stopped equipping the
; allowed one he owned, for the rest of the session.
;
; 2. THE SUBSTITUTE GOES STALE, AND KEEPING IT CURRENT IS YOURS.
;
; It answers "what should this actor wear instead, right now". When the
; right answer changes - the actor got stronger, better gear arrived, the
; substitute left the inventory - call BlockEquip again for the same pair
; with the new one. It overwrites.
;
; Measured: a follower kept wearing an iron helmet after he could have worn
; steel, because the substitute still said iron.
;
; 3. akOwner IS WHAT LETS A BLOCK EXPIRE, AND IT IS REQUIRED.
;
; Blocks survive a save. That is deliberate: for roughly 15 seconds after
; every load you cannot register anything, because your scripts are not
; running yet - and a cell load is exactly when the game runs its equip
; passes. Persisted blocks cover that gap.
;
; Persisting alone would be dangerous: a block left behind by an
; uninstalled mod would stop a player equipping an item forever, with no
; clue as to why. So every block names a Form YOUR OWN plugin defines. On
; load the game maps saved forms onto the current load order, and a form
; whose plugin is gone does not map - so an uninstalled mod's blocks die
; during the load, while yours are live before your scripts wake up.
;
; Any Form you define works: a quest, a global, a keyword. It is identity,
; not data - nothing is read from it. None is refused.
;
; DO NOT pass a vanilla form. A form from Skyrim.esm or another base-game
; master never stops resolving, so a block owned by one can never expire -
; the exact failure this argument prevents. Lodestone warns in the log and
; registers it anyway. The test is: is the Form DEFINED by your plugin, or
; only REFERENCED by it?
;
; Re-registering after a load is still worth doing. It is a correction
; rather than a restoration: the world moved while the save sat on disk.
;
; 4. YOUR SUBSTITUTES HAVE TO BE WEARABLE TOGETHER. NOBODY ELSE CHECKS.
;
; Lodestone equips the substitute it was given, on every refusal of that
; item, and validates nothing about it. It sees one call at a time - never
; the set of blocks you registered, and never the outfit taking shape
; around them. Whether two of your own substitutes can coexist on a body
; is a question only you are holding the pieces to answer.
;
; MEASURED, and it cost a consumer project a day. The mod picked the best
; allowed weapon and the best allowed off-hand piece in SEPARATE passes
; that did not consult each other. The weapon pass elected a GREATSWORD;
; the shield pass kept a SHIELD. Both were registered as substitutes, both
; were equipped on refusal, and the two cannot be worn at once - so each
; one displaced the other, about once a second, at 3.8 calls/s, until the
; session ended. On screen the follower drew and sheathed without stopping
; and took hits while doing it.
;
; Lodestone did exactly what it was told, both times. Nothing in the log
; looked wrong, because from inside the veto nothing WAS wrong.
;
; The rule: substitutes you register for the same actor must be able to go
; on at the same time. A same-slot downgrade of a WORN piece is the easy
; case - iron helmet for ebony helmet - because the slot is occupied
; either way. Anything that competes for the same space on the body is the
; hard case, and two-handed weapons are the sharpest example: they claim
; the off hand, so a shield substitute and a two-handed weapon substitute
; are a contradiction you have to resolve before you register them.
;
; If a blocked item has no substitute that satisfies that, pass None and
; refuse plainly, or keep the item out of the actor's inventory instead -
; a choice never taken needs no refusing.
;
; --------------------------------------------------------------------
; KNOWN INCOMPATIBILITY: Immersive Weapon Switch
; --------------------------------------------------------------------
;
; IMMERSIVE WEAPON SWITCH (Nexus 139762) MAKES THIS UNUSABLE FOR AN NPC'S
; WEAPONS. Measured in game, 08/2026. It is a popular SKSE plugin, so a
; consumer's users will hit this.
;
; What the user sees: the follower draws and sheathes without stopping and
; never attacks, while your policy is correct and the log is clean.
;
; MEASURED, same actor and same loadout, one variable changed:
;
;     IWS 'NPC_Switch' ON    0 attacks     238 IWS failures on that actor
;     IWS 'NPC_Switch' OFF   14 attacks      0
;
; The block stayed armed the whole time in both runs, and the forbidden
; weapon never went on. The veto's policy was never the problem.
;
; THE WORKAROUND IS THAT MOD'S OWN SETTING: turn NPC_Switch off. It leaves
; the mod fully working for the player and lets NPC equips pass straight
; through. Nothing to change on your side, and nothing to change here.
;
; WHY IT HAPPENS IS NOT ESTABLISHED, and this is deliberate wording. Both
; plugins reach ActorEquipManager::EquipObject - this one hooks the body,
; that one patches the call sites, so it runs first. Beyond that the story
; is unverified: the ratio of its failures was never divided by the number
; of attempts, and its own log says the sheathe animation FAILED to play,
; which does not fit the obvious explanation. 'It causes this' is measured;
; 'it causes this because' is not.

; --------------------------------------------------------------------
; COST
; --------------------------------------------------------------------
;
; With no block registered anywhere, the cost is one atomic read per equip
; and nothing else. With blocks registered it is a hash lookup.
;
; The game equips an actor's whole loadout in one pass - measured at 5 to 7
; calls in the same millisecond, and 58 calls in 7 ms across a cell load
; with every NPC in the area dressing itself. A blocked item can be
; attempted several times per pass rather than once; four was measured.
;
; A substitute ALREADY ON THE ACTOR is not put on again - since 1.17.2.
; The AI never learns that an item was refused, so it re-picks the
; blocked one on every pass; without this, each pass put the substitute
; on afresh. On a weapon that restarts the draw animation, and on armor
; it runs unseen - one helmet was measured being re-equipped seven times
; inside 350 ms. The refusal is unaffected either way.

; Blocks akItem on akActor, names what to equip instead, and names who is
; asking.
;
; Returns False if akActor, akItem or akOwner is None, if akItem is not
; something that can be worn, or if akSubstitute was given and is not
; either. Calling it again for the same pair replaces the substitute and
; the owner.
Bool Function BlockEquip(Actor akActor, Form akItem, Form akSubstitute, Form akOwner) global native

; Removes one block. Returns False if there was none, or if either argument
; is None.
Bool Function UnblockEquip(Actor akActor, Form akItem) global native

; Drops every block held for one actor. Returns how many were removed, or
; -1 if akActor is None.
Int Function ClearEquipBlocks(Actor akActor) global native

; Whether a block is held for the pair. Returns False for None arguments.
Bool Function IsEquipBlocked(Actor akActor, Form akItem) global native

; How many blocks are held, across every actor. Diagnostic: a consumer that
; expects a bounded number can watch this and notice a registration loop.
; Returns -1 on failure.
Int Function GetEquipBlockCount() global native

;==============================================================
; PRISMA UI BRIDGE (added in DLL 1.17.0)
;==============================================================
;
; Papyrus access to the Prisma UI framework, which has no Papyrus surface of its
; own: no .psc, no .pex, no .esp, and no native on its DLL. Without this bridge a
; Papyrus-only mod cannot reach it at all, and cannot even ask whether it is
; installed.
;
; PRISMA UI IS OPTIONAL AND IS NOT A DEPENDENCY OF LODESTONE. With it absent
; every function here returns its sentinel and nothing else changes. Nothing
; inside Lodestone consumes this - it is exposed, never depended on.
;
; THERE IS NO FOCUS SURFACE, AND THAT IS DELIBERATE. Focus, Unfocus and
; HasAnyActiveFocus are not exposed. Prisma's focus menu is a single modal menu
; with no focus stack: unfocusing one view closes it for every view, so with two
; Prisma panels on screen the other one's cursor is stranded. That was measured
; in game across all four configurations, with no mitigation found, and reported
; to the framework's author without answer. A panel that never takes focus never
; meets any of it. If you need real keyboard or mouse input, this bridge is not
; enough yet - say so and it becomes its own piece of work, defect included.
;
; VIEWS ARE NAMED BY YOU, NOT BY A HANDLE. Prisma identifies a view with a 64-bit
; value and the Papyrus Int is 32 bits, so handing it back would corrupt it. You
; pass a string id you chose ("StrengthMatters"), and Lodestone keeps the map.
;
; NOTHING HERE SURVIVES A SAVE. After a load, no view exists. Recreate it the
; same way you recreate book text - the state is in your script, not in the DLL.
;
; A MUTATING CALL REPORTS THAT IT WAS ACCEPTED, NOT THAT IT FINISHED. Every call
; into Prisma is handed to the game's main thread, because Papyrus does not run
; there and the renderer does not belong to the Papyrus thread. True means the
; request was queued and its arguments were valid.
;
; THE ORDER THAT WORKS:
;   1. PrismaAvailable()               - if False, offer no panel and stop
;   2. PrismaCreateView(id, path)      - returns immediately, view not ready yet
;   3. wait for the mod event LodestonePrismaViewReady (strArg = your view id),
;      or poll PrismaIsViewReady(id)
;   4. PrismaCall(...)                 - only now is it delivered
;
; Gate on GetVersion() >= 1017000.

; Whether Prisma UI is installed and answered.
;
; A probe, not a failure: False is the expected answer on most load orders and
; writes nothing to the log. Cannot fail.
Bool Function PrismaAvailable() global native

; Asks Prisma to build a view, registered under asViewId - any string you pick,
; scoped to your mod by you (nothing namespaces it for you; prefer your mod's
; name).
;
; asHtmlPath is relative to Data\PrismaUI\views, the framework's own convention.
; "MyMod/index.html" loads Data\PrismaUI\views\MyMod\index.html.
;
; Returns True when the request was accepted - NOT when the panel is on screen.
; Wait for LodestonePrismaViewReady, or poll PrismaIsViewReady, before calling
; PrismaCall.
;
; Idempotent: calling it again with the same id returns True and creates nothing.
;
; Returns False if Prisma UI is absent, or if either argument is empty.
Bool Function PrismaCreateView(String asViewId, String asHtmlPath) global native

; Whether the view exists and its page has finished loading.
;
; Returns False for an unknown id, which is deliberately the same answer as "not
; ready yet" - both mean PrismaCall would do nothing.
Bool Function PrismaIsViewReady(String asViewId) global native

; Calls the JavaScript function asFunction on the view, handing it asJson as its
; single argument. In the page, that is the global function of that name:
; asFunction "MyUpdate" arrives at window.MyUpdate(asJson).
;
; The payload is a plain string as far as the DLL is concerned - JSON is a
; convention between your script and your page, not something checked here.
;
; Returns True when the request was accepted. Returns False for an unknown id, an
; empty function name, and for a view whose page is not ready - Prisma drops
; those calls, so reporting success would be a lie.
Bool Function PrismaCall(String asViewId, String asFunction, String asJson) global native

; Makes the view visible. Returns True when the request was accepted, False for
; an unknown id or a view that has not been built yet.
Bool Function PrismaShow(String asViewId) global native

; Hides the view without destroying it. Returns True when the request was
; accepted, False for an unknown id or a view that has not been built yet.
Bool Function PrismaHide(String asViewId) global native

; Whether the view is hidden.
;
; This reports the last visibility Lodestone applied, not an answer read back
; from Prisma. It is exact for every Show and Hide you issued.
;
; Returns False for an unknown id - the same answer as "visible". Ask
; PrismaIsViewReady first if you need to tell those apart.
Bool Function PrismaIsHidden(String asViewId) global native

; Destroys the view and frees the id, along with every listener registered
; against it. Returns True when the request was accepted, False for an unknown
; id.
Bool Function PrismaDestroy(String asViewId) global native

; Routes a call made BY the page back to Papyrus as a mod event.
;
; asJsFunction is a name Prisma injects into the page as a global function. When
; the page calls it, Lodestone sends the mod event asModEvent, with whatever the
; page passed as strArg and 0.0 as numArg. Receive it with RegisterForModEvent
; like any other mod event.
;
; The event always arrives on the game thread, never on the thread the page's
; callback ran on. That indirection is the point: dispatching a mod event from
; inside the framework's callback thread is the mistake this prevents.
;
; Registering the same view and mod event again reuses its slot rather than
; taking a second one, so re-registering after a load is safe.
;
; There are 32 listener slots across every view and every mod. Running out is
; logged as an error and returns False.
;
; Returns False if Prisma UI is absent, for an unknown or not-yet-built view, for
; an empty name, or when the slots are full.
Bool Function PrismaRegisterListener(String asViewId, String asJsFunction, String asModEvent) global native
