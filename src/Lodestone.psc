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
