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
; NARROWED IN DLL 1.28.0: a spell is in scope only when its type is Spell AND
; its costliest effect has a magic school (an associated skill on the base
; effect). The same test applies to all three channels. A Spell-type record with
; no school is mod machinery, not magic - a buff, marker or cooldown timer cast
; from a script with the player as the source - and through 1.27.x it was scaled
; like any spell, stretching timers that were never the player's magic. A
; script-cast sub-spell or proc with no school of its own no longer scales; the
; spell that casts it still does. If you cast such a spell and WANT it scaled,
; give its costliest effect a school.
;
; TO COUNT ON THIS NARROWING, gate on Lodestone.GetVersion() >= 1028000
; (1.28.0). The three registration functions themselves are older than that
; and did not change - the line below still says >= 1004000, and it is still
; right. A mod that only needs a channel gates on that; a mod that needs the
; helper-spell exclusion to be in effect gates on this one.
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

; --- Magic effect description (added in DLL 1.24.0) -------------------------
;
; Requires Lodestone.GetVersion() >= 1024000 (1.24.0).
;
; Returns a MagicEffect's description - the text the vanilla magic menu shows
; for a spell - RAW, exactly as the engine keeps it. Markers such as <mag> and
; <dur> are left in the text: SUBSTITUTING THEM IS YOUR JOB. You hold the
; numbers, and Lodestone does not reimplement the menu's formatting.
;
; "" IS A NORMAL ANSWER, NOT AN ERROR, and you will get it often. It comes back
; for None, for an effect with no description - close to half of all vanilla
; effects have none - and for a description made only of whitespace, which
; vanilla ships twice and which means "no description" on a card. Handle it
; per effect: one spell can mix effects that have text with effects that do not.
;
; WHAT THE TEXT LOOKS LIKE, measured on a development load order before this
; function was written:
;
;   - It is already in the game's language. The engine keeps resolved text for
;     localized plugins too: Skyrim.esm's Firebolt effect came back as the
;     sentence, not as a string-table id.
;   - <mag> and <dur> are the markers to substitute. MATCH THEM WITHOUT CASE -
;     one vanilla effect writes <MAG>.
;   - <area> DOES NOT APPEAR. Not in vanilla, and not in any of the 5766
;     effects of the measured load order. Do not build on it.
;   - Everything else between < and > is yours to decide. Vanilla uses numeric
;     literals such as <50> and <25>, and <Global=Name> in three DLC effects;
;     mods also wrap plain words in brackets (<Chaurus>, <20%>). 75 distinct
;     markers were counted. How the vanilla menu displays those was NOT
;     measured; showing what is inside the brackets is a reasonable default,
;     not a verified one.
;   - Apart from the whitespace-only case, the text is returned untouched,
;     including any leading or trailing whitespace.
String Function GetEffectDescription(MagicEffect akEffect) global native

; --- Writing a magic effect description (added in DLL 1.29.0) ---------------
;
; Requires Lodestone.GetVersion() >= 1029000 (1.29.0).
;
; SetEffectDescription queues a write of asText onto akEffect's description -
; the same field GetEffectDescription reads. ClearEffectDescription queues a
; restore to whatever that effect held before your first Set call in this
; session. GetEffectDescription reflects the written text as soon as it lands;
; it did not change, it is reading the same field you just wrote.
;
; THE WRITE DOES REACH THE MAGIC MENU CARD. MEASURED IN GAME, 2026-09-17,
; which is the one thing about this pair nobody had measured: a sentence
; written into vanilla Firebolt's and Flames' effects showed on both cards,
; in place of the vanilla text. The menu reads the field when it draws the
; card; it does not cache the record's text from load.
;
; THE Bool THESE TWO RETURN MEANS "ACCEPTED", NOT "WRITTEN". The write happens
; on the game thread and is queued rather than immediate, so the native
; returns before it lands. True is a promise the write was queued, not
; confirmation the card already shows it. False means nothing was queued: a
; None effect, or (Clear only) an effect this plugin never wrote in this
; session - that second case is a normal answer, not a failure.
;
; ONLY A MagicEffect DESCRIPTION IS WRITABLE. A Spell has its own description
; field, and it is NOT writable - it is a string-table id under the hood, the
; same kind of field BookFramework had to hook around rather than write
; directly. Do not ask for a per-Spell version of this pair; there is nothing
; to point it at.
;
; AND A SPELL'S OWN DESCRIPTION HIDES YOURS. MEASURED IN GAME, 2026-09-17: when
; the Spell carrying your effect HAS a Description of its own, the magic menu
; shows THAT and never composes the effect lines - so a description you write
; onto the effect is invisible on that card no matter how correct the write
; was. The measurement was Fusion Meditation, a LesserPower whose SPEL carries
; "Center the mind...": the effect's field was confirmed written, and the card
; showed the spell's sentence anyway.
;
; What this means for you, and it is the difference between working and
; silently doing nothing: THIS PAIR IS FOR SPELLS WITH NO DESCRIPTION OF THEIR
; OWN - which is most of them, vanilla Firebolt and Flames included, and every
; spell built at runtime from a template that has none. If your spell does have
; one, you cannot remove it from Papyrus (see above: not writable), so the
; effect text will not show and nothing here can make it.
;
; WRITING A MagicEffect READ FROM A PLUGIN CHANGES IT FOR THE WHOLE LOAD
; ORDER, FOR THE REST OF THE SESSION - every spell that uses that effect shows
; your text, not just yours. CLONE THE EFFECT FIRST if you do not want that;
; cloning is your job, not this plugin's. This is the same rule already
; written for casting type and delivery, restated here because the mistake is
; the same shape.
;
; SESSION-SCOPED, LIKE BookFramework: nothing here is saved. A written
; effect's description reverts to its record text on the next load, and if you
; want it back, you call SetEffectDescription again - the same reapplication
; your mod already does for anything else session-scoped.
;
; Set WITH AN EMPTY STRING WRITES AN EMPTY DESCRIPTION. It is not a shortcut
; for Clear: the two do different things; a later Clear on that same effect
; still restores the ORIGINAL text, captured before this call, not the empty
; string you just wrote. GetEffectDescription treats whitespace-only text as
; "no description" ("") the same way it always has.
Bool Function SetEffectDescription(MagicEffect akEffect, String asText) global native
Bool Function ClearEffectDescription(MagicEffect akEffect) global native

; --- Spell batch readers (added in DLL 1.25.0) ------------------------------
;
; Requires Lodestone.GetVersion() >= 1025000 (1.25.0).
;
; Three readers that take an array of spells and hand back an array of THE SAME
; LENGTH, one answer per position: element i of the result belongs to element i
; of akSpells. Pass the array you already have - PO3's
; GetAllActorPlayableSpells, or your own - and filter on the results yourself.
;
; WHY THEY EXIST: reading these values one spell at a time costs about one frame
; per call, because MagicEffect.GetAssociatedSkill and Form.GetName wait for the
; game's main thread. Each function here is ONE call for the whole array - about
; one frame, however many spells you pass.
;
; WHICH EFFECT: school and level come from the spell's COSTLIEST effect, and both
; from the same one. That is what the vanilla magic menu shows. IF YOUR SCRIPT
; READS EFFECT 0 TODAY, THE ANSWER CAN CHANGE for a spell with several effects
; whose costliest is not the first. For a single-effect spell it is the same
; effect.
;
; NONE AND EMPTY SPELLS: a None position gives "" (school, name) or -1 (level)
; in that same position. A spell with no effects gives "" and -1 for school and
; level; its name is still returned. A None array gives back an empty array.
;
; ARRAY SIZE: Lodestone imposes no limit of its own. Arrays above 128 elements
; were not measured.

; The school of each spell's costliest effect, as the engine names the skill:
; "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration". An
; effect with no associated skill gives "". Meant to be the exact text
; MagicEffect.GetAssociatedSkill() returns for that effect.
String[] Function GetSpellSchools(Spell[] akSpells) global native

; The minimum skill level of each spell's costliest effect - the value
; MagicEffect.GetSkillLevel() returns for that effect (0, 25, 50, 75 or 100 on
; vanilla spells).
Int[] Function GetSpellSkillLevels(Spell[] akSpells) global native

; The display name of each spell, "" when it has none.
String[] Function GetSpellNames(Spell[] akSpells) global native

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
; WORK IN PROGRESS. Every other module in this file is driven in play by a
; consumer that uses it. This one is not: the mod it was built for is in
; suspended development, so nothing calls it today. The code is complete and
; the hooks install and report themselves in the log, but "no consumer has
; exercised this" and "this works" are different claims, and only the first
; is verified.
;
; The specific gap, named so you do not have to find it: the managed-actor
; registry is serialized to the cosave and read back on load, and that round
; trip has never run with a non-empty registry - only KnockoutActor fills it,
; and nothing outside development has called it. The log line
; "Incapacitation: saved 0 managed actor(s)" is the normal state today.
;
; Everything below is accurate about what the code DOES. None of it is a
; claim that it has been proven in play. Build on it if it fits, and report
; what you find.
;
; FOR A KNOCKDOWN - AN ACTOR ON THE GROUND AND BACK UP - USE KnockDown AND
; KnockDownRelease, in their own block right after this one (1.31.0). The
; physical-fall pair below, KnockoutFall and KnockoutRecover, is DEPRECATED:
; it stays registered and behaves exactly as documented, so nothing compiled
; against it breaks, but it never held an actor on the ground reliably and it
; will not be changed to. KnockoutActor, WakeActor and the rest of this block
; are not deprecated - they are the managed pacify state, a different thing.
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

; DEPRECATED since 1.31.0 - use KnockDown. Kept registered and unchanged.
;
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

; DEPRECATED since 1.31.0 - use KnockDownRelease. Kept registered and
; unchanged.
;
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
;
; KnockDownRelease dispatches this same event - see the block below.

; --- Knockdown (added in DLL 1.31.0) ---------------------------------------
;
; An actor on the ground, held there, and stood back up - one call each way:
;
;     Int result = Lodestone.KnockDown(akTarget, akAttacker)
;     If result >= 0
;         ; down now (0), or already down through KnockDown (1)
;         ; ... your own timer ...
;         Lodestone.KnockDownRelease(akTarget)
;     Else
;         ; refused - result says why, table below
;     EndIf
;
; YOU NEVER CALL Actor.SetUnconscious. KnockDown and KnockDownRelease run the
; engine's own unconscious handler themselves, and the fall and the stand-up
; happen inside it. Calling SetUnconscious yourself on an actor you knocked
; down is not supported.
;
; akSource IS WHERE THE FALL COMES FROM - the attacker, normally. The actor is
; pushed away from that point, so it falls away from whoever hit it. None
; means the player; a source standing on top of the target falls back to a
; point behind the target. It is not a force: how hard the actor falls is not
; a parameter, and there will not be one.
;
; What KnockDown does besides the fall: the target stops combat, its cast is
; interrupted, it stops any furniture interaction, and it leaves the stealth
; meter while down. Its life state is unconscious while down, and alive again
; after the release.
;
; DURATION IS YOURS. There is no timer in this framework - see the note at the
; top of this file. The actor stays down until you call KnockDownRelease.
;
; A LOAD ENDS EVERY KNOCKDOWN. An actor that was down when the game was saved
; comes back STANDING after that save is loaded - life state alive,
; IsKnockedDown False - and NO OnActorWoke is dispatched for it. Treat a game
; load as the end of every knockdown you started: re-arm nothing, and a
; KnockDownRelease that arrives afterwards is a harmless False. The reason is
; that the event registration is per session, so an event fired at load time
; would reach you on some loads and not on others.
;
; A TARGET THAT DIES while down is dropped automatically; KnockDownRelease then
; returns True and dispatches nothing, because it did not wake.
;
; THE TWO PATHS DO NOT MIX ON ONE ACTOR. KnockDown refuses (-5) an actor that
; KnockoutActor is managing, or that the deprecated KnockoutFall armed.
;
; Returned by KnockDown - NEGATIVE IS A REFUSAL, zero or positive is success:
;
;     0   knocked down now
;     1   already down through KnockDown - nothing done
;    -1   akTarget is None
;    -2   akTarget is dead
;    -3   akTarget is the player
;    -4   life state not alive (bleeding out, unconscious by another path...)
;    -5   managed by KnockoutActor, or armed by the deprecated KnockoutFall
;    -6   no 3D loaded, or no AI process
;    -7   the fall did not apply inside the handler - undone, actor unchanged
;   -10   unavailable: Knockout Extensions is loaded
;   -11   unavailable: the handler hook is not installed (VR, or install
;         failed - the log says which)
;   -12   unavailable: another plugin redirected the handler's call site
;
; -10, -11 and -12 hold for the WHOLE SESSION; GetKnockDownAvailability
; returns the same value up front, so a menu can say why before anything is
; tried.
;
; KNOCKOUT EXTENSIONS. It rewrites the same call site this module hooks, and
; the two cannot share it. With it loaded, this module does not install its
; hook at all and every KnockDown returns -10. The deprecated KnockoutFall path
; goes through the same hook and stops working too. Nothing else changes.
;
; Requires Lodestone.GetVersion() >= 1031000 (1.31.0).

; Drops akTarget and holds it on the ground until KnockDownRelease. Returns a
; code - see the table above. Never throws.
Int Function KnockDown(Actor akTarget, ObjectReference akSource = None) global native

; Stands akTarget back up and gives it back to its AI, then dispatches
; OnActorWoke. False, and nothing touched, on an actor that is not down
; through KnockDown - including after a load, which already stood it up.
; Never throws.
Bool Function KnockDownRelease(Actor akTarget) global native

; Is akTarget down through KnockDown right now? From this module's own
; record, not from the engine. None -> False.
Bool Function IsKnockedDown(Actor akTarget) global native

; 0 if KnockDown can work this session; otherwise -10, -11 or -12, with the
; meaning in the table above. Fixed once the game has loaded its data.
Int Function GetKnockDownAvailability() global native

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
; WEB UI BRIDGE (added in DLL 1.17.0, renamed in 1.18.0)
;==============================================================
;
; Papyrus access to a web UI backend, which has no Papyrus surface of its own:
; no .psc, no .pex, no .esp, and no native on its DLL. Without this bridge a
; Papyrus-only mod cannot reach one at all, and cannot even ask whether one is
; installed.
;
; Two backends are supported: Meridian UI, since DLL 1.21.0, and Prisma UI,
; since 1.17.0. ONE of them is used per session. Ask WebUIGetBackend at runtime
; if you want to know which - do not assume, and do not branch on it: ask
; WebUIHasCapability when the answer decides something.
;
; WHICH ONE YOU GET, when both are installed: Meridian UI, unless the user says
; otherwise with WebUIBackend in Data\SKSE\Plugins\Lodestone.ini. The choice is
; made once at load and the log line says which won.
;
; If you need Meridian UI specifically, gate on GetVersion() >= 1021000. If any
; backend will do - which is the point of this surface - gate on 1018000 and ask
; WebUIAvailable.
;
; THAT SAID 1017000 UNTIL 1.23.1, AND IT WAS WRONG IN A WAY THAT WOULD HAVE BITTEN.
; 1017000 is the floor of the OLD Prisma* names; this paragraph is about the
; WebUI* ones, which arrived in 1.18.0. A consumer that followed it would gate on
; a version where the very function named on the next line - WebUIAvailable - does
; not exist, and calling a missing native is a Papyrus error it cannot suppress.
; The number survived the 1.18.0 rename inside a paragraph that had moved on.
;
; THIS SURFACE WAS RENAMED IN 1.18.0. The 1.17.x names still answer and are
; listed at the end of this section, each pointing at its replacement. They are
; removed in 2.0.0, not before. The rename is not cosmetic: the name of one
; supplier had been written into the contract of every consumer, and a contract
; that names its supplier cannot outlive it.
;
; A BACKEND IS OPTIONAL AND IS NOT A DEPENDENCY OF LODESTONE. With none present
; every function here returns its sentinel and nothing else changes. Nothing
; inside Lodestone consumes this - it is exposed, never depended on.
;
; THERE IS A FOCUS SURFACE SINCE 1.22.0, AND IT ANSWERS DIFFERENTLY PER BACKEND.
; WebUIFocusView, WebUIClearFocus and WebUIIsViewFocused give one view the mouse
; and keyboard. ASK WebUIHasCapability("view-focus") FIRST: it is True on Meridian
; UI, True on Prisma UI since 1.27.0 and False there before it, and False with no
; backend. What focus costs on each backend is written at those functions.
;
; DO NOT ASK "focus-stack" TO FIND THIS OUT. It answers False on both backends,
; before AND after 1.22.0, and it is a different question:
;
;   focus-stack   can TWO views hold focus independently?   False, everywhere
;   view-focus    can ONE view receive a click at all?      ask - it varies
;
; Asking the first when you meant the second gives you a wrong answer twice over:
; before 1.22.0 you conclude the surface does not exist, and after it you conclude
; it was built and left unwired.
;
; VIEWS ARE NAMED BY YOU, NOT BY A HANDLE. The backend identifies a view with a
; 64-bit value and the Papyrus Int is 32 bits, so handing it back would corrupt
; it. You pass a string id you chose ("StrengthMatters"), and Lodestone keeps the
; map.
;
; NOTHING HERE SURVIVES A SAVE. After a load, no view exists. Recreate it the
; same way you recreate book text - the state is in your script, not in the DLL.
;
; A MUTATING CALL REPORTS THAT IT WAS ACCEPTED, NOT THAT IT FINISHED. Every call
; into the backend is handed to the game's main thread, because Papyrus does not
; run there and the renderer does not belong to the Papyrus thread. True means
; the request was queued and its arguments were valid.
;
; THE ORDER THAT WORKS:
;   1. WebUIAvailable()             - if False, offer no panel and stop
;   2. WebUICreateView(id, path)    - returns immediately, view not ready yet
;   3. wait for the mod event LodestoneWebUIViewReady (strArg = your view id),
;      or poll WebUIIsViewReady(id)
;   4. WebUICall(...)               - only now is it delivered
;   5. WebUIShow(id)                - and do NOT gate this on anything your page
;                                     sends: a hidden view does not run its page
;
; And if the panel takes input, three more steps that only work on a backend that
; answers True to "view-focus":
;   6. wait for a signal YOUR PAGE sends after it has drawn - the only proof it
;      is your page on screen and not an error page. It cannot arrive before 5
;   7. WebUIFocusView(id)           - only now
;   8. poll WebUIIsViewFocused(id)  - True when the view really has it
;
; STEPS 5 AND 6 ARE IN THAT ORDER FOR A REASON, and swapping them deadlocks:
; showing would wait for the announcement, the announcement waits for the page to
; run, and the page waits to be shown. Nothing errors and nothing logs.
;
; Gate on GetVersion() >= 1018000 for the surface, and >= 1022000 for focus.
;
; AND >= 1023000 IF YOU SHIP FOR MERIDIAN UI, because 1.23.0 moved that root:
; Data\MeridianUI\<asViewPath>, not Data\MeridianUI\Lodestone\<asViewPath>. See
; WebUICreateView. A page installed under the old root is not found, and the
; framework cannot tell you so - it reports the error page as a page that
; loaded. Gate, and move the folder.

; Whether a web UI backend is present and answered.
;
; A probe, not a failure: False is the expected answer on most load orders and
; writes nothing to the log. Cannot fail.
Bool Function WebUIAvailable() global native

; Asks the backend to build a view, registered under asViewId - any string you
; pick, scoped to your mod by you (nothing namespaces it for you; prefer your
; mod's name).
;
; asViewPath is relative to the active backend's view root. It must be at least
; a folder and a file - "MyMod/index.html" - because the first folder names YOU.
;
; NAME THE FILE. There is no directory index and no default document on the
; Meridian side: "MyMod/" does not serve index.html, it serves a 404.
;
; PACKAGING, AND THIS PART IS SPECIFIC TO THE BACKEND YOU ARE RUNNING RATHER THAN
; PART OF THIS CONTRACT. The path you pass does not change; where it is rooted
; does:
;
;   Prisma UI      Data\PrismaUI\views\<asViewPath>
;   Meridian UI    Data\MeridianUI\<asViewPath>
;
; So "MyMod/index.html" reads Data\PrismaUI\views\MyMod\index.html under one and
; Data\MeridianUI\MyMod\index.html under the other.
;
; SHIP YOUR PAGE TO BOTH ROOTS if you want to work with either backend, and that
; is the same folder copied twice, not two pages: the same file was measured
; working unmodified on both.
;
; THE MERIDIAN ROOT CHANGED IN 1.23.0, AND IT IS A HARD BREAK. It used to be
; Data\MeridianUI\Lodestone\<asViewPath> - one host owned by this framework, with
; every consumer nested inside it. That layout cannot work: with two mods both
; shipping Data\MeridianUI\Lodestone\, only ONE is visible and which one depends
; on your mod order. Measured in game, three times, with the roles swapping when
; the order was inverted.
;
; It is not a defect in Meridian UI. That framework serves each mod from its own
; folder on purpose - Data\MeridianUI\<YourModName>\, with the folder name as the
; URL host - and its traversal guard refuses a file that does not resolve inside
; the host's own real folder. Two mods sharing one host is outside that model.
;
; So if you shipped to the old root, MOVE YOUR FOLDER UP ONE LEVEL. There is no
; fallback and there cannot be one: a missing page on that backend produces an
; error page that loads successfully, so the framework cannot tell "wrong root"
; from "working" and cannot retry the other one.
;
; THE TWO ROOTS ARE NO LONGER SYMMETRIC, AND THAT IS CORRECT. Prisma UI keeps its
; shared Data\PrismaUI\views\ root, where four consumers coexist today, because
; it has no such guard. Do not "fix" one to look like the other.
;
; Returns True when the request was accepted - NOT when the panel is on screen.
; Wait for LodestoneWebUIViewReady, or poll WebUIIsViewReady, before calling
; WebUICall.
;
; Idempotent: calling it again with the same id returns True and creates nothing.
;
; Returns False if no backend is present, if either argument is empty, if
; asViewPath names no folder before the file, or - on Meridian UI - if that first
; folder is not usable as a URL host. All four write a line naming the reason;
; none of them fails quietly.
Bool Function WebUICreateView(String asViewId, String asViewPath) global native

; Whether the view exists and its page has finished loading.
;
; IT SAYS THE PAGE LOADED, NOT THAT IT RAN, and the difference has cost in-game
; rounds. This answers True with the view still HIDDEN - and a hidden view does
; not execute its page, so nothing your page would send has been sent yet. It
; also answers True for a browser error page, because an error page loads like
; any other. Use it to know WebUICall will be delivered; do not read it as "my
; page is up and working".
;
; Returns False for an unknown id, which is deliberately the same answer as "not
; ready yet" - both mean WebUICall would do nothing. WebUIGetViewState is what
; tells them apart.
Bool Function WebUIIsViewReady(String asViewId) global native

; Calls the JavaScript function asJsFunction on the view, handing it asJson as
; its single argument. In the page, that is the global function of that name:
; asJsFunction "MyUpdate" arrives at window.MyUpdate(asJson).
;
; The payload is a plain string as far as the DLL is concerned - JSON is a
; convention between your script and your page, not something checked here.
;
; A STRING LITERAL CAN REACH YOUR PAGE WITH DIFFERENT CAPITALIZATION THAN YOU
; WROTE, and that is the engine, not this bridge. Papyrus String literals are
; interned in the game's string pool, which ignores case: a literal matching an
; entry already there, differing only in case, comes back in THE SPELLING THE
; POOL HOLDS - and any script, any mod, or the engine itself may have put that
; entry there. Measured in game on 2026-09-22: a script sending "stop" reached
; the page as "Stop", "done" as "Done", and "Master" as "master". The bridge was
; instrumented at both ends in the same run and carried every payload byte for
; byte, so there is nothing here to fix and no native could fix it - the spelling
; is already changed before this function is entered. It is not specific to
; WebUICall either; it reaches every native that takes a String. It only becomes
; visible here because a page reads the payload as data.
;
; WHAT DECIDES IS COLLISION, AND EVERY LITERAL IS INTERNED ON ITS OWN, BEFORE
; ANY CONCATENATION - so composing the payload at runtime does NOT protect it,
; because the pieces are still literals. "stop" and "done" are common words, they
; collide, and they flip. "start|" + pct + "|" + seg survives because the literal
; is "start|", which is not a word anything else would have interned. Give every
; literal a shape nothing else would use - a prefix, a separator, a marker - or
; compare case-insensitively in your page.
;
; Do not branch on the exact case of a word you wrote as a literal, and do not
; assume the case you saw once is stable: the same literal has been seen arriving
; both ways within a single session.
;
; Returns True when the request was accepted. Returns False for an unknown id, an
; empty function name, and for a view whose page is not ready - the backend drops
; those calls, so reporting success would be a lie.
Bool Function WebUICall(String asViewId, String asJsFunction, String asJson) global native

; Makes the view visible. Returns True when the request was accepted, False for
; an unknown id or a view that has not been built yet.
Bool Function WebUIShow(String asViewId) global native

; Hides the view without destroying it. Returns True when the request was
; accepted, False for an unknown id or a view that has not been built yet.
Bool Function WebUIHide(String asViewId) global native

; True when the view exists, is ready, and is visible.
;
; False for an unknown id, for a view that is not ready yet, and for a hidden
; view - call WebUIGetViewState when you need to tell those three apart.
;
; This reports the last visibility Lodestone applied, not an answer read back
; from the backend. It is exact for every Show and Hide you issued.
Bool Function WebUIIsViewVisible(String asViewId) global native

; Destroys the view and frees the id, along with every listener registered
; against it. Returns True when the request was accepted, False for an unknown
; id.
Bool Function WebUIDestroyView(String asViewId) global native

; Routes a call made BY the page back to Papyrus as a mod event.
;
; Calling asJsFunction in the page delivers its argument to the mod event
; asModEvent, with whatever the page passed as strArg and 0.0 as numArg. How that
; name becomes callable in the page is the backend's business. Receive the event
; with RegisterForModEvent like any other mod event.
;
; The event always arrives on the game thread, never on the thread the page's
; callback ran on. That indirection is the point: dispatching a mod event from
; inside the backend's callback thread is the mistake this prevents.
;
; Registering the same view and mod event again reuses its slot rather than
; taking a second one, so re-registering after a load is safe.
;
; There are a finite number of listener slots, 32 today, shared across every view
; and every mod - ask WebUIGetListenerSlotsFree. Running out is logged as an
; error and returns False.
;
; Returns False if no backend is present, for an unknown or not-yet-built view,
; for an empty name, or when the slots are full.
Bool Function WebUIRegisterListener(String asViewId, String asJsFunction, String asModEvent) global native

; Name of the active web UI backend - "MeridianUI" or "PrismaUI" today. Empty
; string when no backend is present.
;
; THIS IS FOR YOUR LOG, NOT FOR YOUR CONTROL FLOW. Do not branch on it. A
; consumer that writes If WebUIGetBackend() == "PrismaUI" has moved the vendor
; name out of nine function names and into a string compare that no compiler
; checks. Ask WebUIHasCapability what the backend can do; ask this only when a
; human is going to read the answer.
String Function WebUIGetBackend() global native

; Whether the active backend supports a named capability.
;
; An unknown name returns False and logs nothing - asking about a capability
; that does not exist yet is a valid question with a valid answer. That is what
; makes this safe to call from a consumer written against an older Lodestone.
;
; Names defined in 1.18.0:
;   "focus-stack"  can two views hold focus independently
;   "view-order"   can a view stacking order be set
;   "inspector"    can a developer inspector be opened on a view
;
; Added in 1.22.0:
;   "view-focus"   can ONE view be given the mouse and keyboard
;
; THE ANSWERS DEPEND ON THE BACKEND, WHICH IS THE ENTIRE POINT OF ASKING HERE
; RATHER THAN ASKING WHICH BACKEND IT IS. As of 1.27.0:
;
;                  Prisma UI   Meridian UI
;   focus-stack    False       False
;   view-focus     True        True
;   view-order     True        True
;   inspector      True        False
;
; From 1.22.0 to 1.26.x, "view-focus" was False on Prisma UI. A consumer that
; already asked needs no change: the answer grew, and the check starts passing.
;
; READ THOSE FIRST TWO ROWS TOGETHER, BECAUSE THEY ARE THE TRAP OF THIS WHOLE
; SURFACE. They look like the same question and they are not:
;
;   focus-stack   can TWO views hold focus independently?
;   view-focus    can ONE view receive a click at all?
;
; Meridian UI answers False to the first and True to the second, and both answers
; are correct: it gives one view the keyboard and forbids two views holding it at
; once. If you ask "focus-stack" meaning "can my panel take a click", you get
; False on the one backend that can, and you ship a panel that never offers input.
;
; "focus-stack" is False on both, for different reasons - one has no focus stack,
; the other arbitrates so that exactly one view holds focus at a time - and a
; consumer asking that question does not have to care which. IT STAYS False. It
; is not waiting to be implemented.
;
; Returns False when no backend is present.
Bool Function WebUIHasCapability(String asCapability) global native

; State of one view in a single call, resolving the ambiguity between unknown,
; still building, ready and hidden, and ready and visible.
;
;   -1  unknown id, or no backend present
;    0  created, page has not loaded yet
;    1  ready, hidden
;    2  ready, visible
Int Function WebUIGetViewState(String asViewId) global native

; How many listener slots are still free, out of a finite pool shared by every
; view and every mod.
;
; Diagnostic: a consumer that expects a known number of listeners can watch this
; and notice a registration loop. Returns -1 when no backend is present.
Int Function WebUIGetListenerSlotsFree() global native

; --- Focus, added in 1.22.0 ---------------------------------------------------
;
; Gate on GetVersion() >= 1022000, and then ask
; WebUIHasCapability("view-focus") - the version tells you the functions exist,
; the capability tells you whether the installed backend can honour them.
;
; BOTH BACKENDS CAN SINCE 1.27.0. Still ask - with no backend installed the
; answer is False, and a later backend may answer differently. Write the panel
; so it degrades to display-only when the answer is False.
;
; FROM 1.22.0 TO 1.26.x PRISMA UI ANSWERED False, for three reasons. The third
; is what 1.27.0 changed; the first two are still true, and they are now yours
; to weigh rather than Lodestone's to decide:
;
;   1. Its input capture is per PROCESS, not per view. Focusing your view takes
;      the keyboard from every other Prisma panel in the game, including panels
;      belonging to mods that never heard of Lodestone.
;   2. Its unfocus closes a single shared modal menu for every view at once, so
;      a second Prisma panel on screen is left with a stranded cursor when yours
;      lets go. Measured in game across all four flag configurations, with no
;      mitigation found, and reported to that backend's author without answer.
;   3. It publishes no panic key. SINCE 1.27.0 LODESTONE PROVIDES ONE: an input
;      handler of its own, on the same WebUIPanicKeys chord, measured in game to
;      see the chord with a Prisma panel focused.
;
; So on Prisma UI, if your panel can open while another mod's Prisma panel is
; on screen, expect to take its keyboard, and expect its cursor to be stranded
; when yours lets go. Nothing in this bridge can prevent either.
;
; THE CHORD IS NOT THE SAME MECHANISM ON THE TWO BACKENDS, and the difference
; can reach your page:
;
;                    Meridian UI                 Prisma UI
;   who checks it    the backend                 Lodestone
;   what it does     toggles focus               releases focus, never gives it
;   which views      the one holding focus       views of THIS bridge only
;   your page        never sees the chord        MAY see the chord first
;
; The last row was measured: with a Prisma text field focused, Ctrl+Backspace
; reached the page as a keystroke while Lodestone was releasing the focus. If
; your page reacts to that chord, it will react before it loses focus. And the
; chord on Prisma UI does not touch another mod's Prisma panel - that panel is
; not this bridge's to release.
;
; ONE VIEW AT A TIME. At most one view created through this bridge holds focus,
; and a second asker is REFUSED rather than queued. That is Lodestone's rule, not
; the backend's, and it is not the same promise as "focus-stack" - see
; WebUIHasCapability.
;
; THAT RULE STOPS AT THE BRIDGE. On Meridian UI, taking focus DEPOSES whichever
; Meridian UI browser held it, including browsers that belong to mods which never
; heard of Lodestone: the backend keeps one owner across all of its consumers,
; and the call this bridge makes is the one that claims rather than waits. The
; reverse is covered - if another mod takes focus from your view, the bridge
; observes it and WebUIIsViewFocused turns False, a fraction of a second later.
;
; AND IT DOES NOT REACH PRISMA UI AT ALL. If another mod has a Prisma UI panel
; focused when your view takes focus on Meridian UI, both frameworks hold input
; at once and keys reach both. That was measured in game, with Meridian UI
; claiming focus over a focused Prisma UI panel: the two frameworks do not
; negotiate focus with each other, and the Prisma UI side never lets go. The
; panic chord below releases only the Meridian UI side - Lodestone's own chord
; for Prisma UI exists only when Prisma UI is the backend, and even then it
; releases only views of this bridge. Nothing in this bridge
; can prevent it, so if your panel can open while another mod's Prisma UI panel
; has focus, plan for that.
;
; FOCUS IS TAKEN AWAY BEHIND YOUR BACK, ON PURPOSE. Lodestone releases it when a
; save is loaded, when a new game starts, and when any menu that pauses the game
; opens. The player can also drop it at any moment with the panic chord - the
; backend's own on Meridian UI, Lodestone's on Prisma UI, the same setting on
; both - Ctrl+Backspace by default, settable as WebUIPanicKeys in
; Data\SKSE\Plugins\Lodestone.ini. YOU CANNOT DISABLE THAT, and you should not
; want to: it is what stops a broken page from making the game unplayable.
;
; So never assume you still have focus because you asked for it once. Ask
; WebUIIsViewFocused.

; Asks for the view to receive the mouse and keyboard.
;
; Returns True when the request was ACCEPTED, not when the view has focus - the
; same contract as every other mutating call here. Poll WebUIIsViewFocused, which
; is the answer that matters.
;
; Returns False, immediately and for a reason you can act on, when: no backend is
; present, the backend answers False to "view-focus", the view id is unknown, the
; view is not ready yet, the view is hidden, or another view already holds focus.
; On Prisma UI also when Lodestone could not install its panic chord - focus
; without a way out is what the bridge will not give. The log says so.
;
; Call it AFTER WebUIShow. A hidden view is refused, because focus on something
; the player cannot see is the exact state the panic chord exists to undo.
;
; AND DO NOT TRUST "READY" AS PROOF THAT YOUR PAGE LOADED. This is the one place
; where that distinction can hurt the player rather than you.
;
; A missing or misnamed page does not fail on the Meridian backend: it produces
; an error page, and an error page is HTML that loads successfully. So the view
; reports ready, LodestoneWebUIViewReady fires, WebUIGetViewState answers 2, and
; a JS listener registers - all exactly as they would for the page you meant. The
; bridge cannot tell the two apart and does not pretend to.
;
; Give that view focus and the player is holding a browser error page with no
; button on it. Measured: a consumer did exactly this, and the only way out was
; killing the game.
;
; THE FIX IS ON YOUR SIDE AND IT IS SMALL: have the PAGE tell you it is alive.
; Register a listener, call it from your page after it has drawn, and gate FOCUS
; on it. A page that did not load cannot send it, and that is the only signal
; that distinguishes the two. The interactive example shipped with this framework
; does it with LodestoneExampleReady.
;
; SHOW FIRST, THEN WAIT, THEN FOCUS - AND THE ORDER IS NOT INTERCHANGEABLE:
;
;   WebUIShow(id)                  always, without waiting for anything
;   wait for your page's signal    it cannot arrive before the Show
;   WebUIFocusView(id)             only now
;
; BECAUSE A HIDDEN VIEW DOES NOT RUN ITS PAGE. Gate the Show on the page's signal
; and you have built a deadlock: showing waits for the announcement, the
; announcement waits for the page to run, and the page waits to be shown. It
; produces no error and no log line - WebUICreateView returned True, the view
; reported ready, your listeners registered - and the panel simply never appears.
; Measured by a consumer, who lost two rounds of in-game testing to it.
;
; AND THAT IS WHY "READY" IS NOT ENOUGH ON ITS OWN: WebUIIsViewReady answers True
; with the view still hidden. It says the page LOADED, not that it RAN.
;
; Showing is safe; focusing is what is not. An error page on screen is ugly and
; recoverable - the player closes your panel. An error page WITH FOCUS is the one
; that traps them.
;
; The panic chord still saves the player if you skip this - Ctrl+Backspace, and
; no mod can disable it. Do not make them use it.
Bool Function WebUIFocusView(String asViewId) global native

; Gives the mouse and keyboard back to the game.
;
; Returns True when asViewId was the view holding focus and the release was
; requested, False otherwise - so it also answers "did I still have it".
;
; IT TAKES A VIEW ID ON PURPOSE. Every function here is reachable by every mod in
; the load order; a version without the id would let any mod drop any other mod's
; focus from a script that never mentioned it.
Bool Function WebUIClearFocus(String asViewId) global native

; Whether the view is receiving the mouse and keyboard right now.
;
; THIS IS THE ONE TO POLL. WebUIFocusView answers "accepted"; this answers "has
; it". It also catches every way focus goes away without you - the automatic
; releases above, the panic chord, and the backend handing focus elsewhere.
;
; Answered from what the backend last reported observing, so it can lag a change
; by a fraction of a second. Returns False for an unknown id and when no backend
; is present.
Bool Function WebUIIsViewFocused(String asViewId) global native

; --- Enumeration, added in 1.30.0 ---------------------------------------------
;
; Gate on GetVersion() >= 1030000.
;
; WHY THIS EXISTS. Two mods can each open a panel, and until now neither had any
; way to notice the other. Both pick a corner, both ship the same corner as the
; default, and one lands on top of the other. That is not a bug in either mod:
; nothing they could call answered "is anyone else on screen". These two answer
; it.
;
; THEY ANSWER "WHO IS OPEN", NOT "WHERE THEY ARE", AND THAT LIMIT IS REAL. This
; bridge does not know where any view sits. The rectangle is CSS inside your own
; page, and the numbers driving it are yours, pushed through WebUICall - both
; halves are on your side of this contract and neither is on this one. What you
; get here is who is sharing the screen with you. Deciding where to put yourself
; is still your job, and still your code.
;
; NOT A CONTROL SURFACE. You can see another mod's view; you cannot move it,
; hide it, outrank it, or refuse to share with it. Which panel sits where is a
; decision for the mod that owns that panel, and handing one consumer a way to
; shove another aside would be arbitration with bad manners. This is the same
; posture as GetChannelContributorCount, for the same reason.
;
; THE ID IS THE ONLY IDENTITY THERE IS. You get back the ids passed to
; WebUICreateView and nothing else. The bridge never learns which plugin a view
; came from - WebUICreateView takes two strings and no Form, so there is no file
; to derive a name from. In practice the id is the mod's name, because that is
; what this contract tells you to pick, but that is a convention among
; consumers and not something the framework enforces. Do not parse it for
; anything load-bearing.
;
; ONE CALL EACH, NOT A COUNT AND A LOOP. Both hand back the whole list in a
; single call. That is one frame instead of one per view, and - the part that
; matters more - one consistent snapshot: there is no index that can go stale
; between two calls because a third mod opened or closed a panel in between.

; Every view this bridge knows, in any state: still building, ready and hidden,
; ready and visible. Sorted by id.
;
; Sorted so that two calls over an unchanged set compare equal. That is the only
; promise made about order - a given id's position is not stable across a create
; or a destroy, and nothing here is an index worth keeping.
;
; Cannot fail. AN EMPTY ARRAY IS NOT A SENTINEL, it means no views. That is also
; what you get with no backend installed, and the two do not need telling apart:
; with no backend your own view does not exist either, so every decision you
; would make from this list comes out the same.
String[] Function WebUIGetViewIds() global native

; The views that are on screen right now: created, page loaded, not hidden.
; Sorted by id.
;
; THIS IS THE ONE TO ASK BEFORE YOU PLACE YOUR PANEL. A view that exists but is
; hidden takes up no screen, and screen is what you are trying to share; reading
; the other list here would have you dodging a panel nobody can see.
;
; The test is exactly WebUIIsViewVisible's, so this list and that function can
; never disagree about one view.
;
; IF YOUR OWN ID IS MISSING, THIS LIST CANNOT TELL YOU WHY. Hidden, still
; building, never created, and already destroyed all read the same here. Ask
; WebUIGetViewIds, or WebUIGetViewState for the one id.
;
; Cannot fail; an empty array reads the same way as in WebUIGetViewIds.
String[] Function WebUIGetVisibleViewIds() global native

; --- Menu prompts (added in DLL 1.26.0) ---------------------------------------
;
; Asks the player a question on screen and hands the answer back to your script.
; Two questions, which are the two a consumer actually needs: pick one of N
; texts, and type a line of text.
;
; THESE TWO CALLS WAIT. The script that calls MenuPromptList or MenuPromptText
; stops there until the player answers, exactly like the menus you may be
; replacing. The game is PAUSED while a prompt is open.
;
; A REQUIREMENT YOU INHERIT, WHICH LODESTONE DOES NOT HAVE. The menus are drawn
; by SKSE Menu Framework (Nexus mod 120352), which Lodestone does not depend on
; and does not install.
; Without it every function here answers its sentinel, nothing is written to the
; log as a failure, and your mod keeps working - but it shows no prompt. If you
; ship a flow that needs one, then YOUR mod depends on that framework and on
; what it requires in turn (Address Library and SSE Engine Fixes), and your mod
; page is where that has to be written. Ask MenuPromptAvailable and keep
; whatever you do today as the fallback.
;
; THE ORDER THAT WORKS:
;   1. MenuPromptAvailable()        - if False, no prompt can be shown. Use your
;                                     own fallback and stop
;   2. MenuPromptList(...)          - or MenuPromptText(...). Your script stops
;                                     here until the player is done
;   3. read the returned Int        - see the three outcomes below
;   4. for text only, and ONLY when the return is 1 or greater:
;      MenuPromptTakeText(iReturn)  - takes the typed line, once
;
; THREE OUTCOMES, AND THEY ARE DISTINCT ON PURPOSE:
;
;   -2  REFUSED. Nothing was shown at all. Either no framework is present, or
;       another prompt was already open, or the list had no entries. Nobody saw
;       anything, so do not treat it as a decision by the player
;   -1  CANCELLED. The player dismissed the prompt with Escape or Cancel, or a
;       save was loaded and took it away
;    0  and above - an ANSWER. For a list it is the index into the array you
;       passed. For text it is a ticket, and MenuPromptTakeText turns it into
;       the line the player typed
;
; AN EMPTY LINE IS AN ANSWER, NOT A CANCELLATION, and that is the whole reason
; text answers come back as a ticket instead of as a String. If the player
; clears the box and accepts, MenuPromptText returns a ticket and
; MenuPromptTakeText returns "". A single String return could not tell that from
; a cancellation, which is why flows built on the older surface treat empty as
; cancel - they had no other signal. You do.
;
; ONE PROMPT AT A TIME, ACROSS THE WHOLE LOAD ORDER. A second request while one
; is open is REFUSED rather than queued, because a queue makes a script wait on
; a window the player never asked for and cannot see. Refused is -2, and it is
; not cancellation: the player made no choice.
;
; A LOAD TAKES THE PROMPT AWAY. Loading a save or starting a new game closes an
; open prompt and hands the waiting script -1. A script waiting on a window that
; belongs to a world which no longer exists is the state this avoids.
;
; NO 128 ENTRY CEILING IN THIS SURFACE, and the list is drawn through a clipper,
; so 300 entries cost what 30 do. Long entries widen the window up to most of
; the screen instead of being cut. BUT PAPYRUS ITSELF IS THE OTHER HALF OF THAT
; QUESTION: the array literal "new String[N]" stops at 128 in the language, and
; whether an array built past that by other means survives the trip into a
; native is NOT MEASURED. If you go above 128, measure it.
;
; Gate on GetVersion() >= 1026000.

; Whether a prompt can be shown at all.
;
; A probe, not a failure: False is the expected answer on a load order without
; the menu framework, and it writes nothing to the log. ASK THIS FIRST, and keep
; your existing path for the False case. Cannot fail.
Bool Function MenuPromptAvailable() global native

; Whether a prompt is open right now, anywhere in the load order. A call to
; MenuPromptList or MenuPromptText would be refused.
;
; You do not need this to use the two prompts - they answer -2 when refused, and
; checking first cannot remove the race. It is here for a script that wants to
; avoid asking at all while the player is busy. Returns False when no prompt can
; be shown at all.
Bool Function MenuPromptBusy() global native

; Shows a single-choice list titled asTitle and WAITS for the player.
;
; Returns the index into asEntries that the player picked, -1 if it was
; cancelled, or -2 if nothing was shown. The title is yours to write and there
; is no entry limit in this surface - read the notes above.
;
; An empty asEntries is refused (-2) rather than shown as an empty window.
Int Function MenuPromptList(String asTitle, String[] asEntries) global native

; Shows a text box titled asTitle, seeded with asSuggestion and SELECTED, so
; that typing replaces it and Enter alone accepts it unchanged. WAITS for the
; player.
;
; Returns a TICKET of 1 or more when the player accepted, -1 if cancelled, or -2
; if nothing was shown. The ticket is not the text: hand it to
; MenuPromptTakeText to get the line.
;
; The box holds 512 BYTES, and text comes back as UTF-8, so an accented letter
; costs more than one. A longer suggestion is truncated at a character boundary.
Int Function MenuPromptText(String asTitle, String asSuggestion) global native

; Takes the line the player typed, for a ticket MenuPromptText returned.
;
; ONCE PER TICKET. The text is handed over and forgotten, so a second call with
; the same ticket returns "". Take it as soon as the call returns: only the last
; few answers are kept, and an older one is dropped when they are.
;
; Returns "" for a ticket that was already taken, for a made-up number, and for
; the -1 and -2 returns - which is why you only call this when the return was 1
; or more. An empty String from a VALID ticket means the player accepted an
; empty line, and that is an answer.
String Function MenuPromptTakeText(Int aiTicket) global native

;--------------------------------------------------------------
; DEPRECATED - the 1.17.x names
;--------------------------------------------------------------
;
; Each forwards to its replacement and keeps working for the whole 1.18.x cycle,
; so a .pex built against 1.17.x does not have to be recompiled. They are removed
; in 2.0.0. Papyrus has no deprecation attribute; a comment is what there is.
;
; The mod event LodestonePrismaViewReady is deprecated on the same terms. It is
; still sent, with the same strArg, from the same point as
; LodestoneWebUIViewReady - both arrive in the same session. Register for the new
; one.

; DEPRECATED - use WebUIAvailable instead.
Bool Function PrismaAvailable() global native

; DEPRECATED - use WebUICreateView instead.
Bool Function PrismaCreateView(String asViewId, String asHtmlPath) global native

; DEPRECATED - use WebUIIsViewReady instead.
Bool Function PrismaIsViewReady(String asViewId) global native

; DEPRECATED - use WebUICall instead.
Bool Function PrismaCall(String asViewId, String asFunction, String asJson) global native

; DEPRECATED - use WebUIShow instead.
Bool Function PrismaShow(String asViewId) global native

; DEPRECATED - use WebUIHide instead.
Bool Function PrismaHide(String asViewId) global native

; DEPRECATED - use WebUIIsViewVisible instead, and read its comment first: it
; answers the OPPOSITE question. This one is left with its old sense on purpose,
; so that a .pex built against 1.17.x is not silently handed an inverted answer.
; True when the view was hidden; False for an unknown id, which is the same
; answer as "visible".
Bool Function PrismaIsHidden(String asViewId) global native

; DEPRECATED - use WebUIDestroyView instead.
Bool Function PrismaDestroy(String asViewId) global native

; DEPRECATED - use WebUIRegisterListener instead.
Bool Function PrismaRegisterListener(String asViewId, String asJsFunction, String asModEvent) global native
