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
; SINGLE CHANNEL (DLL 1.1.0): the first mod to register owns the channel for the
; session. Re-registering the SAME two globals is a harmless refresh. A second,
; DIFFERENT mod that registers while a channel is held is warned in the log and
; ignored (this function returns False for it). Arbitration between several cast
; time mods is a possible future addition, not part of this version.
;
; Returns True when YOUR channel is the active one after the call. Returns False
; on a None argument, or when a channel from a different registration is already
; held. If the DLL is absent the call yields the VM default False as well.
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

; --- SpellTomes (added in DLL 1.3.0, full interception since 1.5.0) --------

; With the DLL installed, reading a spell tome does NOTHING on its own: the
; spell is not learned and the book is not consumed. The book is flagged as
; read, because the player did open it, and that is all that happens.
;
; This is unconditional - it does not wait for anyone to register. Registering
; only adds the notification. To be told when a tome is read, register a form
; whose script implements OnSpellTomeRead, and then decide everything yourself:
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
; form carrying the script (a quest, an alias, a magic effect, a reference).
; Returns True on success, False on a None form.
Bool Function RegisterForSpellTomeRead(Form akReceiver) global native

; Stops akReceiver from receiving OnSpellTomeRead.
; Returns True on success, False on a None form.
Bool Function UnregisterForSpellTomeRead(Form akReceiver) global native

; Removes one copy of akBook from akActor - the explicit "consume the tome now"
; call. Use it when your own logic decides the book should be eaten. Nothing in
; the DLL consumes a tome on its own.
; Returns True on success, False on a None argument.
Bool Function ConsumeSpellTome(Book akBook, ObjectReference akActor) global native

; --- Event, implemented by a registered script -----------------------------
; Sent to each form registered via RegisterForSpellTomeRead, every time a spell
; tome is read. akBook is the tome, akReader is who read it (normally the player).
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
; One channel per quantity: the first registrant wins, re-registering the same
; pair is a harmless refresh, and a different second registrant is rejected.
; Requires Lodestone.GetVersion() >= 1004000 (1.4.0).
; All three return True when your pair is the active one, False on a None
; argument or a rejected second registrant.

; Scales the magnitude of a spell's effects.
Bool Function RegisterMagicMagnitudeChannel(GlobalVariable akMultiplier, GlobalVariable akOffset) global native

; Scales the duration of a spell's effects.
Bool Function RegisterMagicDurationChannel(GlobalVariable akMultiplier, GlobalVariable akOffset) global native

; Scales a spell's magicka cost. Use a multiplier below 1.0 to make casting
; cheaper.
Bool Function RegisterMagicCostChannel(GlobalVariable akMultiplier, GlobalVariable akOffset) global native
