// CinematicCamera.cpp
// Lodestone - Shared SKSE framework
//
// L-C1 - Lodestone.CinematicApproach, CinematicIsActive, CinematicStop.
//
// MECHANISM. The player camera is put in the engine's free camera state with
// PlayerCamera::ToggleFreeCameraMode(false) (false: the time is NOT frozen).
// From then on the pose of the camera is ours: the free state's two virtuals,
// GetTranslation and GetRotation, are swapped in its vtable, and while an
// approach runs they return the pose computed for the frame instead of the one
// the free state built from keyboard and mouse. The free state keeps reading
// the input and keeps writing its own fields; nothing here reads them back, so
// the input has no effect on the pose we return. (INFERRED, not measured: that
// the camera root takes its pose from those two virtuals. The log line measures
// it - see drift= below.)
//
// THE FRAME POINT. The pose is advanced in the free state's own Update virtual
// (TESCameraState slot 03 SE/AE, 04 VR), before the original runs. 1.42.0 used
// PlayerCharacter::Update and it never reached the step while the camera was
// in the free state. That slot is still hooked (chained with Incapacitation):
// it measures the hand back, catches a camera taken out of the free state and
// keeps the wall clock safety net. The approach clock adds the engine's frame
// time (GetSecondsSinceLastFrame), and nothing while a menu pauses the game.
//
// THE CONTROLS stay the caller's. Each toggle of the free state is measured on
// the ControlMap (before and after, and again at the first frame in the free
// state); any control flag a toggle turned ON that the caller had off is turned
// back off at once. Nothing the caller left on is turned off.
//
// THE PATH. The start is afBack units behind the target, along the opposite of
// the direction it faces, and afUp above its feet. The end is the pose the
// third person state reports when the approach starts. In between the position
// is interpolated with smoothstep (soft start and soft end). The aim is the
// target's head all along, blended into the end rotation with the same weight.
//
// THE END. Always announced: ModEvent Lodestone_CinematicDone, strArg
//   done     the time ran out normally
//   stopped  CinematicStop, the target died or went away, or someone else took
//            the camera out of the free state
//   load     a save was loaded or a new game started
//   timeout  wall clock passed afSeconds + 3 s without the frames arriving
// The camera is handed back by ToggleFreeCameraMode(false), which restores the
// state it came from.
//
// THE LOG LINE. One per approach, always on, written when the hand back has
// been measured (two frames after it):
//   CinematicCamera: end reason=<r> start=(x,y,z) end=(x,y,z) frames=N ms=M
//     handback=<d> drift=<d> qcheck=<q> ticks=T
// ticks = calls of the free state's Update while the approach ran (frames counts
// the ones that advanced the pose). Right before it, also always on:
//   CinematicCamera: measure pcupdate=P ctl before=.. free=.. first=..
//     fixin=.. backbefore=.. backafter=.. fixout=..
// pcupdate = calls of the PlayerCharacter::Update hook with the camera in the
// free state; the ctl values are the enabled control flags (hex, kMovement is
// bit 0) and fixin/fixout what this file turned back off.
// handback = distance between the last point written and the position of the
// camera root two frames after the hand back. drift = largest distance between
// a point written and the camera root at the next frame while the approach ran
// (0 means the pose we return is the pose on screen). qcheck = |dot| between the
// third person rotation and the one rebuilt from the camera root's matrix at the
// start (1 means this file reads rotations the way the engine does).
// LogCinematicCamera = 1 adds one line per frame.
//
// L-C2 - Lodestone.CinematicOrbit. The same machine (free state, the same
// frame point, the same end, guards, event and log line) with another path:
// a circle around the target's position at the call, starting afStartAngle
// degrees from the direction it faces (0 in front, 180 behind) and turning
// afSweep degrees (positive = clockwise seen from above). The sweep is adjusted
// by at most 180 degrees so that the last angle is the third person camera's
// angle around the target; radius and height go from afRadius / afUp to the
// third person camera's in the last 180 degrees, and the rotation from "look at
// the head" to the third person rotation in the last 90 degrees. The angle
// follows smoothstep in time (soft start and soft end). The last frame is the
// third person pose.
// abFreezeWorld enters the free state with ToggleFreeCameraMode(true) (the
// world's time frozen, as the console's tfc 1). Then the orbit clock adds the
// wall clock between frames, not the engine's frame time. Every end hands back
// with ToggleFreeCameraMode(false) and, if Main's freezeTime is still set and
// was not set before the call, clears it. A wall clock watchdog (a thread that
// queues a task at afSeconds + 3.5 s) ends an orbit even if no frame point runs.
// The end line gets kind=approach|orbit freeze=0|1 at the end, and an orbit adds
// one line before the measure line:
//   CinematicCamera: orbit sweep=<deg> total=<deg> radius=<a>-><b> up=<a>-><b>
//     freezebefore=.. freezein=.. freezeout=.. unfroze=.. zerodt=N
// (freeze* = Main freezeTime before the call, after entering, after the hand
// back; zerodt = steps whose engine frame time was 0).
//
// Phase L-C1, L-C2

#include "CinematicCamera.h"

#include "Config.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

namespace Lodestone::Core::CinematicCamera
{
	namespace
	{
		constexpr const char* kDoneEvent = "Lodestone_CinematicDone";
		constexpr float       kSafetySeconds = 3.0f;

		bool g_logFrames = false;  // Lodestone.ini LogCinematicCamera = 1
		bool g_installed = false;

		enum class Phase
		{
			kIdle,
			kRunning,   // the pose is ours, the clock runs
			kEnding,    // the end is decided and the camera still has to be handed back
			kHandback,  // handed back; waiting for the frame that measures it
		};

		struct Vec3
		{
			float x{ 0 }, y{ 0 }, z{ 0 };
		};

		struct Run
		{
			Phase           phase{ Phase::kIdle };
			RE::ActorHandle target;
			RE::FormID      targetID{ 0 };
			float           seconds{ 0 };
			float           elapsed{ 0 };
			std::uint32_t   frames{ 0 };
			std::chrono::steady_clock::time_point wallStart;
			Vec3            start;       // first point
			Vec3            end;         // pose of the third person camera when it began
			RE::NiQuaternion endRot{ 1, 0, 0, 0 };
			Vec3            curPos;      // what the getters return
			RE::NiQuaternion curRot{ 1, 0, 0, 0 };
			Vec3            lastWritten;
			bool            haveLast{ false };
			float           maxDrift{ 0 };
			float           qcheck{ 0 };
			const char*     reason{ "done" };
			const char*     why{ "" };
			std::uint32_t   handbackTicks{ 0 };
			double          ms{ 0 };
			// Calls of the frame point that drives the pose (the free state's
			// Update), from the start until the end is decided: ticks= on the
			// end line, always on.
			std::uint32_t   ticks{ 0 };
			// Calls of the PlayerCharacter::Update thunk in the same span, with
			// the camera in the free state: the measure of the 1.42.0 frame point.
			std::uint32_t   pcTicks{ 0 };
			// The enabled control flags (ControlMap) around each toggle of the
			// free state, and what this file turned back off after each one.
			bool            ctlRead{ false };
			std::uint32_t   ctlBefore{ 0 };      // before entering the free state
			std::uint32_t   ctlFree{ 0 };        // right after entering it
			std::uint32_t   ctlTick{ 0 };       // at the first tick in it
			bool            ctlTickRead{ false };
			std::uint32_t   fixIn{ 0 };          // turned back off, entry and first tick
			bool            ctlBackRead{ false };
			std::uint32_t   ctlBackBefore{ 0 };  // before handing back
			std::uint32_t   ctlBackAfter{ 0 };   // right after handing back
			std::uint32_t   fixOut{ 0 };         // turned back off at the hand back
			// L-C2 - the orbit. kind=orbit on the end line.
			bool            orbit{ false };
			bool            freeze{ false };     // abFreezeWorld
			Vec3            center;              // target position at the call
			float           heading{ 0 };        // target GetAngleZ at the call
			float           a0{ 0 };             // start angle, radians from the heading
			float           total{ 0 };          // adjusted sweep, radians (signed)
			float           sweepDeg{ 0 };       // as asked
			float           r0{ 0 }, r1{ 0 };    // radius at the start and of the third person camera
			float           up0{ 0 }, up1{ 0 };  // height above the center, same
			float           sR{ 0 }, sQ{ 0 };    // where radius/height and rotation start to converge
			std::chrono::steady_clock::time_point lastStepWall;
			bool            haveStepWall{ false };
			std::uint32_t   zeroDt{ 0 };
			int             freezeBefore{ -1 };  // -1 = not read
			int             freezeIn{ -1 };
			int             freezeOut{ -1 };
			bool            unfroze{ false };
		};

		Run g_run;
		std::atomic<bool> g_overriding{ false };  // read by the vtable hooks
		bool g_loadPending = false;
		// Bumped by every start; the orbit watchdog acts only on its own run.
		std::atomic<std::uint32_t> g_generation{ 0 };
		// Main freezeTime was set by an orbit and not yet cleared (load path).
		bool g_frozeByUs = false;

		constexpr float kPi = 3.14159265358979f;
		constexpr float kDegToRad = kPi / 180.0f;

		// ---------------------------------------------------------------
		// maths
		// ---------------------------------------------------------------
		Vec3 Lerp(const Vec3& a, const Vec3& b, float t)
		{
			return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
		}

		float Dist(const Vec3& a, const Vec3& b)
		{
			const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		Vec3 ToVec(const RE::NiPoint3& p)
		{
			return { p.x, p.y, p.z };
		}

		RE::NiQuaternion Normalize(RE::NiQuaternion q)
		{
			const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
			if (n < 1e-6f) {
				return RE::NiQuaternion(1, 0, 0, 0);
			}
			return RE::NiQuaternion(q.w / n, q.x / n, q.y / n, q.z / n);
		}

		RE::NiQuaternion Slerp(RE::NiQuaternion a, RE::NiQuaternion b, float t)
		{
			float d = a.Dot(b);
			if (d < 0.0f) {
				b.Neg();
				d = -d;
			}
			if (d > 0.9995f) {
				return Normalize(RE::NiQuaternion(a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t,
					a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t));
			}
			const float theta = std::acos(d);
			const float s = std::sin(theta);
			const float wa = std::sin((1.0f - t) * theta) / s;
			const float wb = std::sin(t * theta) / s;
			return Normalize(RE::NiQuaternion(a.w * wa + b.w * wb, a.x * wa + b.x * wb,
				a.y * wa + b.y * wb, a.z * wa + b.z * wb));
		}

		// Standard rotation matrix to quaternion (the same convention as
		// NiQuaternion::ToRotation in CommonLib: M * v rotates a column vector).
		RE::NiQuaternion FromMatrix(const RE::NiMatrix3& m)
		{
			const auto& e = m.entry;
			const float tr = e[0][0] + e[1][1] + e[2][2];
			RE::NiQuaternion q(1, 0, 0, 0);
			if (tr > 0.0f) {
				const float s = std::sqrt(tr + 1.0f) * 2.0f;
				q.w = 0.25f * s;
				q.x = (e[2][1] - e[1][2]) / s;
				q.y = (e[0][2] - e[2][0]) / s;
				q.z = (e[1][0] - e[0][1]) / s;
			} else if (e[0][0] > e[1][1] && e[0][0] > e[2][2]) {
				const float s = std::sqrt(1.0f + e[0][0] - e[1][1] - e[2][2]) * 2.0f;
				q.w = (e[2][1] - e[1][2]) / s;
				q.x = 0.25f * s;
				q.y = (e[0][1] + e[1][0]) / s;
				q.z = (e[0][2] + e[2][0]) / s;
			} else if (e[1][1] > e[2][2]) {
				const float s = std::sqrt(1.0f + e[1][1] - e[0][0] - e[2][2]) * 2.0f;
				q.w = (e[0][2] - e[2][0]) / s;
				q.x = (e[0][1] + e[1][0]) / s;
				q.y = 0.25f * s;
				q.z = (e[1][2] + e[2][1]) / s;
			} else {
				const float s = std::sqrt(1.0f + e[2][2] - e[0][0] - e[1][1]) * 2.0f;
				q.w = (e[1][0] - e[0][1]) / s;
				q.x = (e[0][2] + e[2][0]) / s;
				q.y = (e[1][2] + e[2][1]) / s;
				q.z = 0.25f * s;
			}
			return Normalize(q);
		}

		// The Gamebryo camera looks along its local +Y, with +Z up and +X right.
		RE::NiQuaternion LookAt(const Vec3& from, const Vec3& to)
		{
			Vec3 f{ to.x - from.x, to.y - from.y, to.z - from.z };
			const float fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
			if (fl < 1e-4f) {
				return RE::NiQuaternion(1, 0, 0, 0);
			}
			f = { f.x / fl, f.y / fl, f.z / fl };
			// r = f x (0,0,1)
			Vec3  r{ f.y, -f.x, 0.0f };
			float rl = std::sqrt(r.x * r.x + r.y * r.y);
			if (rl < 1e-4f) {
				r = { 1, 0, 0 };
				rl = 1;
			}
			r = { r.x / rl, r.y / rl, 0.0f };
			// u = r x f
			const Vec3 u{ r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x };
			RE::NiMatrix3 m;
			m.entry[0][0] = r.x;
			m.entry[1][0] = r.y;
			m.entry[2][0] = r.z;
			m.entry[0][1] = f.x;
			m.entry[1][1] = f.y;
			m.entry[2][1] = f.z;
			m.entry[0][2] = u.x;
			m.entry[1][2] = u.y;
			m.entry[2][2] = u.z;
			return FromMatrix(m);
		}

		// ---------------------------------------------------------------
		// helpers
		// ---------------------------------------------------------------
		Vec3 HeadOf(RE::Actor* a)
		{
			const auto p = a->GetPosition();
			float      h = a->GetHeight();
			if (!(h > 1.0f)) {
				h = 128.0f;
			}
			return { p.x, p.y, p.z + h * 0.93f };
		}

		bool MenuBlocks()
		{
			if (auto* ui = RE::UI::GetSingleton(); ui) {
				if (ui->GameIsPaused() || ui->IsModalMenuOpen()) {
					return true;
				}
			}
			return false;
		}

		// Into (-pi, pi].
		float WrapPi(float a)
		{
			a = std::fmod(a + kPi, 2.0f * kPi);
			if (a <= 0.0f) {
				a += 2.0f * kPi;
			}
			return a - kPi;
		}

		float Smooth(float t)
		{
			if (t <= 0.0f) {
				return 0.0f;
			}
			if (t >= 1.0f) {
				return 1.0f;
			}
			return t * t * (3.0f - 2.0f * t);
		}

		// 0 until a_from, then smoothstep up to 1 at s = 1.
		float Window(float a_s, float a_from)
		{
			if (a_from >= 1.0f) {
				return a_s >= 1.0f ? 1.0f : 0.0f;
			}
			return Smooth((a_s - a_from) / (1.0f - a_from));
		}

		// Main freezeTime: 1 / 0, or -1 when Main is not there.
		int ReadFreeze()
		{
			if (auto* main = RE::Main::GetSingleton(); main) {
				return main->GetRuntimeData().freezeTime ? 1 : 0;
			}
			return -1;
		}

		// After the hand back of an orbit that froze the world: the world runs
		// again unless it was already frozen before the call.
		void Unfreeze(Run& a_run)
		{
			if (!a_run.freeze) {
				return;
			}
			a_run.freezeOut = ReadFreeze();
			if (a_run.freezeOut == 1 && a_run.freezeBefore != 1) {
				if (auto* main = RE::Main::GetSingleton(); main) {
					main->GetRuntimeData().freezeTime = false;
					a_run.unfroze = true;
				}
			}
			g_frozeByUs = false;
		}

		// ---------------------------------------------------------------
		// controls
		// ---------------------------------------------------------------
		// kMovement (bit 0) up to kVATS (bit 11), UserEvents.h USER_EVENT_FLAG.
		constexpr std::uint32_t kControlMask = 0xFFF;

		bool ReadControls(std::uint32_t& a_out)
		{
			const auto* map = RE::ControlMap::GetSingleton();
			if (!map) {
				return false;
			}
			a_out = map->GetRuntimeData().enabledControls.underlying() & kControlMask;
			return true;
		}

		// A toggle of the free state is ours, the controls are the caller's:
		// whatever the toggle turned ON that was off before it is turned back
		// off. Nothing the caller left on is ever turned off. Returns the bits.
		std::uint32_t TurnBackOff(std::uint32_t a_before, std::uint32_t a_after)
		{
			const std::uint32_t on = a_after & ~a_before & kControlMask;
			if (on != 0) {
				if (auto* map = RE::ControlMap::GetSingleton(); map) {
					map->ToggleControls(static_cast<RE::UserEvents::USER_EVENT_FLAG>(on), false, false);
				}
			}
			return on;
		}

		std::string Hex(bool a_read, std::uint32_t a_value)
		{
			return a_read ? std::format("0x{:03X}", a_value) : std::string("na");
		}

		void SendDone(const char* a_reason)
		{
			auto* source = SKSE::GetModCallbackEventSource();
			if (!source) {
				spdlog::error("CinematicCamera: no mod callback event source - '{}' not sent.", a_reason);
				return;
			}
			SKSE::ModCallbackEvent modEvent{
				RE::BSFixedString(kDoneEvent),
				RE::BSFixedString(a_reason),
				0.0f,
				nullptr
			};
			source->SendEvent(&modEvent);
		}

		void FlushLine(bool a_measured, float a_handback)
		{
			const auto& r = g_run;
			if (r.orbit) {
				spdlog::info("CinematicCamera: orbit sweep={:.1f} total={:.1f} radius={:.1f}->{:.1f} up={:.1f}->{:.1f} "
							 "freezebefore={} freezein={} freezeout={} unfroze={} zerodt={}",
					r.sweepDeg, r.total / kDegToRad, r.r0, r.r1, r.up0, r.up1, r.freezeBefore, r.freezeIn, r.freezeOut,
					r.unfroze ? 1 : 0, r.zeroDt);
			}
			spdlog::info("CinematicCamera: measure pcupdate={} ctl before={} free={} first={} fixin=0x{:03X} "
						 "backbefore={} backafter={} fixout=0x{:03X}",
				r.pcTicks, Hex(r.ctlRead, r.ctlBefore), Hex(r.ctlRead, r.ctlFree), Hex(r.ctlTickRead, r.ctlTick),
				r.fixIn, Hex(r.ctlBackRead, r.ctlBackBefore), Hex(r.ctlBackRead, r.ctlBackAfter), r.fixOut);
			if (a_measured) {
				spdlog::info("CinematicCamera: end reason={}{}{} start=({:.1f},{:.1f},{:.1f}) end=({:.1f},{:.1f},{:.1f}) "
							 "frames={} ms={:.0f} handback={:.2f} drift={:.2f} qcheck={:.4f} ticks={} kind={} freeze={}",
					r.reason, *r.why ? " why=" : "", r.why, r.start.x, r.start.y, r.start.z, r.lastWritten.x,
					r.lastWritten.y, r.lastWritten.z, r.frames, r.ms, a_handback, r.maxDrift, r.qcheck, r.ticks,
					r.orbit ? "orbit" : "approach", r.freeze ? 1 : 0);
			} else {
				spdlog::info("CinematicCamera: end reason={}{}{} start=({:.1f},{:.1f},{:.1f}) end=({:.1f},{:.1f},{:.1f}) "
							 "frames={} ms={:.0f} handback=na drift={:.2f} qcheck={:.4f} ticks={} kind={} freeze={}",
					r.reason, *r.why ? " why=" : "", r.why, r.start.x, r.start.y, r.start.z, r.lastWritten.x,
					r.lastWritten.y, r.lastWritten.z, r.frames, r.ms, r.maxDrift, r.qcheck, r.ticks,
					r.orbit ? "orbit" : "approach", r.freeze ? 1 : 0);
			}
		}

		// Hands the camera back and announces. Main thread only.
		void HandBack(bool a_toggle)
		{
			if (a_toggle) {
				if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->IsInFreeCameraMode()) {
					auto&      r = g_run;
					const bool read = ReadControls(r.ctlBackBefore);
					cam->ToggleFreeCameraMode(false);
					if (read && ReadControls(r.ctlBackAfter)) {
						r.ctlBackRead = true;
						r.fixOut = TurnBackOff(r.ctlBackBefore, r.ctlBackAfter);
					}
				}
			}
			Unfreeze(g_run);  // every end of a frozen orbit, toggle or not
			g_run.phase = Phase::kHandback;
			g_run.handbackTicks = 0;
			g_overriding.store(false, std::memory_order_relaxed);
			SendDone(g_run.reason);
		}

		// Decides the end. From a frame the hand back is queued; from a native
		// (already on the main thread) it is immediate.
		void Finish(const char* a_reason, const char* a_why, bool a_immediate, bool a_toggle)
		{
			if (g_run.phase != Phase::kRunning) {
				return;
			}
			g_run.reason = a_reason;
			g_run.why = a_why;
			g_run.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_run.wallStart).count();
			if (a_immediate) {
				HandBack(a_toggle);
				return;
			}
			g_run.phase = Phase::kEnding;
			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				HandBack(a_toggle);
				return;
			}
			task->AddTask([a_toggle]() {
				if (g_run.phase == Phase::kEnding) {
					HandBack(a_toggle);
				}
			});
		}

		// ---------------------------------------------------------------
		// the per frame step
		// ---------------------------------------------------------------
		// Which frame point is calling. kCamera is the free state's own Update,
		// which runs while the free state is the current camera state; kPlayer is
		// PlayerCharacter::Update, which the 1.42.0 field run measured NOT reaching
		// this file while the camera was in the free state (frames=0 and no
		// timeout, in eight approaches) and reaching it outside it (camera-taken,
		// every hand back).
		enum class Source
		{
			kCamera,
			kPlayer,
		};

		float FrameDelta()
		{
			if (MenuBlocks()) {
				return 0.0f;  // a pause does not eat the approach
			}
			const float dt = RE::GetSecondsSinceLastFrame();
			if (!(dt > 0.0f)) {
				return 0.0f;
			}
			return dt > 0.25f ? 0.25f : dt;
		}

		// The orbit clock. Unfrozen: the engine's frame time, as the approach.
		// Frozen (abFreezeWorld): the wall clock between two steps, because the
		// engine's frame time is not known to keep running while the world is
		// frozen (zerodt= on the orbit line measures it). A pause still stops it.
		float OrbitDelta(Run& a_run)
		{
			const auto now = std::chrono::steady_clock::now();
			float      real = 0.0f;
			if (a_run.haveStepWall) {
				real = std::chrono::duration<float>(now - a_run.lastStepWall).count();
			}
			a_run.lastStepWall = now;
			a_run.haveStepWall = true;
			if (!(RE::GetSecondsSinceLastFrame() > 0.0f)) {
				++a_run.zeroDt;
			}
			if (!a_run.freeze) {
				return FrameDelta();
			}
			if (MenuBlocks() || !(real > 0.0f)) {
				return 0.0f;
			}
			return real > 0.25f ? 0.25f : real;
		}

		void Step(Source a_source)
		{
			auto& r = g_run;

			if (r.phase == Phase::kHandback) {
				// Out of the free state its Update no longer runs; the player
				// frame measures the hand back, as in 1.42.0.
				if (a_source != Source::kPlayer) {
					return;
				}
				if (++r.handbackTicks >= 2) {
					float hb = -1.0f;
					if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
						hb = Dist(ToVec(cam->cameraRoot->world.translate), r.lastWritten);
					}
					FlushLine(hb >= 0.0f, hb);
					r.phase = Phase::kIdle;
				}
				return;
			}
			if (r.phase != Phase::kRunning) {
				return;
			}

			auto* cam = RE::PlayerCamera::GetSingleton();
			if (!cam || !cam->IsInFreeCameraMode()) {
				Finish("stopped", "camera-taken", false, false);
				return;
			}

			const auto wall = std::chrono::duration<float>(std::chrono::steady_clock::now() - r.wallStart).count();
			if (a_source == Source::kPlayer) {
				// Only the safety net from here: the pose is the free state's.
				if (wall > r.seconds + kSafetySeconds) {
					Finish("timeout", "", false, true);
				}
				return;
			}

			// The camera root, one frame after the point written: the measure of
			// whether what we return is what is on screen.
			if (r.haveLast && cam->cameraRoot) {
				const float d = Dist(ToVec(cam->cameraRoot->world.translate), r.lastWritten);
				if (d > r.maxDrift) {
					r.maxDrift = d;
				}
			}

			const auto actorPtr = r.target.get();
			auto*      actor = actorPtr.get();
			if (!actor || actor->IsDead() || !actor->Is3DLoaded()) {
				Finish("stopped", "target-gone", false, true);
				return;
			}
			if (wall > r.seconds + kSafetySeconds) {
				Finish("timeout", "", false, true);
				return;
			}

			r.elapsed += r.orbit ? OrbitDelta(r) : FrameDelta();
			++r.frames;
			float t = r.seconds > 0.0f ? r.elapsed / r.seconds : 1.0f;
			if (t > 1.0f) {
				t = 1.0f;
			}
			const float s = t * t * (3.0f - 2.0f * t);

			if (r.orbit) {
				if (t >= 1.0f) {
					r.curPos = r.end;  // the third person pose, exactly
					r.curRot = r.endRot;
				} else {
					const float a = r.heading + r.a0 + r.total * s;
					const float wr = Window(s, r.sR);
					const float wq = Window(s, r.sQ);
					const float rad = r.r0 + (r.r1 - r.r0) * wr;
					const float up = r.up0 + (r.up1 - r.up0) * wr;
					r.curPos = { r.center.x + std::sin(a) * rad, r.center.y + std::cos(a) * rad, r.center.z + up };
					const auto aim = LookAt(r.curPos, HeadOf(actor));
					r.curRot = Slerp(aim, r.endRot, wq);
				}
			} else {
				r.curPos = Lerp(r.start, r.end, s);
				const auto aim = LookAt(r.curPos, HeadOf(actor));
				r.curRot = Slerp(aim, r.endRot, s);
			}
			r.lastWritten = r.curPos;
			r.haveLast = true;

			if (g_logFrames) {
				spdlog::info("CinematicCamera: frame {} tick {} pc {} t={:.3f} pos=({:.1f},{:.1f},{:.1f}) drift={:.2f}",
					r.frames, r.ticks, r.pcTicks, t, r.curPos.x, r.curPos.y, r.curPos.z, r.maxDrift);
			}

			if (t >= 1.0f) {
				Finish("done", "", false, true);
			}
		}

		// ---------------------------------------------------------------
		// hooks
		// ---------------------------------------------------------------
		struct PlayerUpdateHook
		{
			static void thunk(RE::Actor* a_this, float a_delta)
			{
				func(a_this, a_delta);
				if (g_run.phase == Phase::kIdle) {
					return;
				}
				try {
					if (g_run.phase == Phase::kRunning || g_run.phase == Phase::kEnding) {
						if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->IsInFreeCameraMode()) {
							++g_run.pcTicks;
						}
					}
					Step(Source::kPlayer);
				} catch (...) {
					spdlog::error("CinematicCamera: the player frame step threw - swallowed.");
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The free state's own per frame virtual. The step runs BEFORE the
		// original, so the getters the engine asks later in the same frame
		// already return this frame's pose.
		struct FreeUpdateHook
		{
			static void thunk(RE::TESCameraState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_nextState)
			{
				if (g_run.phase == Phase::kRunning || g_run.phase == Phase::kEnding) {
					try {
						auto& r = g_run;
						++r.ticks;
						if (r.ticks == 1 && r.phase == Phase::kRunning && r.ctlRead && ReadControls(r.ctlTick)) {
							// What the free state turned on after the toggle had
							// returned (its first frame), against the caller's.
							r.ctlTickRead = true;
							r.fixIn |= TurnBackOff(r.ctlBefore, r.ctlTick);
						}
						Step(Source::kCamera);
					} catch (...) {
						spdlog::error("CinematicCamera: the camera frame step threw - swallowed.");
					}
				}
				func(a_this, a_nextState);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct GetTranslationHook
		{
			static void thunk(RE::TESCameraState* a_this, RE::NiPoint3& a_out)
			{
				func(a_this, a_out);
				if (g_overriding.load(std::memory_order_relaxed)) {
					a_out.x = g_run.curPos.x;
					a_out.y = g_run.curPos.y;
					a_out.z = g_run.curPos.z;
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct GetRotationHook
		{
			static void thunk(RE::TESCameraState* a_this, RE::NiQuaternion& a_out)
			{
				func(a_this, a_out);
				if (g_overriding.load(std::memory_order_relaxed)) {
					a_out = g_run.curRot;
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// ---------------------------------------------------------------
		// natives
		// ---------------------------------------------------------------
		bool CinematicApproach(RE::StaticFunctionTag*, RE::Actor* a_target, float a_back, float a_up, float a_seconds)
		{
			try {
				if (!g_installed) {
					spdlog::warn("CinematicCamera: approach refused - the hooks are not installed.");
					return false;
				}
				if (!a_target || a_target->IsDeleted() || !a_target->Is3DLoaded()) {
					return false;
				}
				if (!(a_seconds > 0.0f) || a_back < 0.0f || a_up < 0.0f) {
					return false;
				}
				if (g_run.phase == Phase::kRunning || g_run.phase == Phase::kEnding) {
					return false;
				}
				if (MenuBlocks()) {
					return false;
				}
				auto* cam = RE::PlayerCamera::GetSingleton();
				if (!cam || !cam->IsInThirdPerson() || cam->IsInFreeCameraMode() || !cam->currentState ||
					!cam->cameraRoot) {
					return false;
				}

				if (g_run.phase == Phase::kHandback) {
					FlushLine(false, 0.0f);  // the previous one never got measured
				}

				Run run;
				run.target = a_target->CreateRefHandle();
				run.targetID = a_target->GetFormID();
				run.seconds = a_seconds;

				// The pose to arrive at, from the third person state itself.
				RE::NiPoint3 tp;
				cam->currentState->GetTranslation(tp);
				cam->currentState->GetRotation(run.endRot);
				run.end = ToVec(tp);
				run.qcheck = std::fabs(FromMatrix(cam->cameraRoot->world.rotate).Dot(run.endRot));

				// The first point: behind the target (opposite its heading) and above.
				const float heading = a_target->GetAngleZ();
				const auto  pos = a_target->GetPosition();
				run.start = { pos.x - std::sin(heading) * a_back, pos.y - std::cos(heading) * a_back, pos.z + a_up };
				run.curPos = run.start;
				run.curRot = LookAt(run.start, HeadOf(a_target));
				run.lastWritten = run.start;
				run.wallStart = std::chrono::steady_clock::now();
				run.phase = Phase::kRunning;

				// Ours from the first frame the free state exists.
				g_run = run;
				g_overriding.store(true, std::memory_order_relaxed);
				const bool ctlRead = ReadControls(g_run.ctlBefore);
				cam->ToggleFreeCameraMode(false);
				if (ctlRead && ReadControls(g_run.ctlFree)) {
					g_run.ctlRead = true;
					g_run.fixIn = TurnBackOff(g_run.ctlBefore, g_run.ctlFree);
				}
				if (!cam->IsInFreeCameraMode()) {
					g_overriding.store(false, std::memory_order_relaxed);
					g_run.phase = Phase::kIdle;
					spdlog::warn("CinematicCamera: approach refused - the camera did not enter the free state.");
					return false;
				}

				if (g_logFrames) {
					spdlog::info("CinematicCamera: begin target=0x{:08X} start=({:.1f},{:.1f},{:.1f}) end=({:.1f},{:.1f},{:.1f}) "
								 "seconds={:.2f} rootdiff={:.2f}",
						g_run.targetID, g_run.start.x, g_run.start.y, g_run.start.z, g_run.end.x, g_run.end.y,
						g_run.end.z, a_seconds, Dist(ToVec(cam->cameraRoot->world.translate), g_run.end));
				}
				return true;
			} catch (...) {
				spdlog::error("CinematicCamera: CinematicApproach threw - refused.");
				g_overriding.store(false, std::memory_order_relaxed);
				return false;
			}
		}

		// L-C2. Ends an orbit from the wall clock even if no frame point runs
		// (a frozen world is not measured to keep the free state's Update).
		void StartWatchdog(std::uint32_t a_generation, float a_seconds)
		{
			const auto wait = std::chrono::milliseconds(static_cast<long long>((a_seconds + kSafetySeconds + 0.5f) * 1000.0f));
			std::thread([a_generation, wait]() {
				std::this_thread::sleep_for(wait);
				auto* task = SKSE::GetTaskInterface();
				if (!task) {
					return;
				}
				task->AddTask([a_generation]() {
					try {
						if (g_generation.load() != a_generation || !g_run.orbit) {
							return;  // another run since, or already over
						}
						if (g_run.phase == Phase::kRunning) {
							Finish("timeout", "watchdog", true, true);
						} else if (g_run.phase == Phase::kEnding) {
							HandBack(true);  // the queued hand back never ran
						}
					} catch (...) {
						spdlog::error("CinematicCamera: the orbit watchdog threw - swallowed.");
					}
				});
			}).detach();
		}

		bool CinematicOrbit(RE::StaticFunctionTag*, RE::Actor* a_target, float a_startAngle, float a_sweep, float a_radius,
			float a_up, float a_seconds, bool a_freezeWorld)
		{
			try {
				if (!g_installed) {
					spdlog::warn("CinematicCamera: orbit refused - the hooks are not installed.");
					return false;
				}
				if (!a_target || a_target->IsDeleted() || !a_target->Is3DLoaded()) {
					return false;
				}
				if (!(a_seconds > 0.0f) || !(a_radius >= 0.0f) || !(a_up >= 0.0f) || !std::isfinite(a_seconds) ||
					!std::isfinite(a_radius) || !std::isfinite(a_up) || !std::isfinite(a_startAngle) ||
					!std::isfinite(a_sweep) || a_sweep == 0.0f) {
					return false;
				}
				if (g_run.phase == Phase::kRunning || g_run.phase == Phase::kEnding) {
					return false;
				}
				if (MenuBlocks()) {
					return false;
				}
				auto* cam = RE::PlayerCamera::GetSingleton();
				if (!cam || !cam->IsInThirdPerson() || cam->IsInFreeCameraMode() || !cam->currentState ||
					!cam->cameraRoot) {
					return false;
				}

				if (g_run.phase == Phase::kHandback) {
					FlushLine(false, 0.0f);  // the previous one never got measured
				}

				Run run;
				run.orbit = true;
				run.freeze = a_freezeWorld;
				run.target = a_target->CreateRefHandle();
				run.targetID = a_target->GetFormID();
				run.seconds = a_seconds;

				// The pose to arrive at, from the third person state itself.
				RE::NiPoint3 tp;
				cam->currentState->GetTranslation(tp);
				cam->currentState->GetRotation(run.endRot);
				run.end = ToVec(tp);
				run.qcheck = std::fabs(FromMatrix(cam->cameraRoot->world.rotate).Dot(run.endRot));

				// The circle: around the target's position now, angles from the
				// direction it faces, positive clockwise seen from above (the
				// direction GetAngleZ grows: 0 north, 90 east).
				const auto pos = a_target->GetPosition();
				run.center = { pos.x, pos.y, pos.z };
				run.heading = a_target->GetAngleZ();
				run.sweepDeg = a_sweep;
				run.a0 = a_startAngle * kDegToRad;
				run.r0 = a_radius;
				run.up0 = a_up;
				const float dx = run.end.x - run.center.x;
				const float dy = run.end.y - run.center.y;
				run.r1 = std::sqrt(dx * dx + dy * dy);
				run.up1 = run.end.z - run.center.z;
				const float sweep = a_sweep * kDegToRad;
				const float endRel = run.r1 > 1.0f ? WrapPi(std::atan2(dx, dy) - run.heading) : WrapPi(run.a0 + sweep);
				run.total = sweep + WrapPi(endRel - (run.a0 + sweep));
				const float absTotal = std::fabs(run.total);
				run.sR = absTotal > kPi ? 1.0f - kPi / absTotal : 0.0f;
				run.sQ = absTotal > 0.5f * kPi ? 1.0f - 0.5f * kPi / absTotal : 0.0f;

				const float a = run.heading + run.a0;
				run.start = { run.center.x + std::sin(a) * run.r0, run.center.y + std::cos(a) * run.r0, run.center.z + run.up0 };
				run.curPos = run.start;
				run.curRot = LookAt(run.start, HeadOf(a_target));
				run.lastWritten = run.start;
				run.wallStart = std::chrono::steady_clock::now();
				run.phase = Phase::kRunning;
				run.freezeBefore = ReadFreeze();

				g_run = run;
				g_overriding.store(true, std::memory_order_relaxed);
				const bool ctlRead = ReadControls(g_run.ctlBefore);
				cam->ToggleFreeCameraMode(a_freezeWorld);
				if (ctlRead && ReadControls(g_run.ctlFree)) {
					g_run.ctlRead = true;
					g_run.fixIn = TurnBackOff(g_run.ctlBefore, g_run.ctlFree);
				}
				g_run.freezeIn = ReadFreeze();
				g_frozeByUs = a_freezeWorld && g_run.freezeIn == 1 && g_run.freezeBefore != 1;
				if (!cam->IsInFreeCameraMode()) {
					g_overriding.store(false, std::memory_order_relaxed);
					Unfreeze(g_run);
					g_run.phase = Phase::kIdle;
					spdlog::warn("CinematicCamera: orbit refused - the camera did not enter the free state.");
					return false;
				}

				const auto gen = g_generation.fetch_add(1) + 1;
				StartWatchdog(gen, a_seconds);

				if (g_logFrames) {
					spdlog::info("CinematicCamera: begin orbit target=0x{:08X} start=({:.1f},{:.1f},{:.1f}) end=({:.1f},{:.1f},{:.1f}) "
								 "seconds={:.2f} total={:.1f} sR={:.3f} sQ={:.3f} freeze={} freezein={}",
						g_run.targetID, g_run.start.x, g_run.start.y, g_run.start.z, g_run.end.x, g_run.end.y,
						g_run.end.z, a_seconds, g_run.total / kDegToRad, g_run.sR, g_run.sQ, a_freezeWorld ? 1 : 0,
						g_run.freezeIn);
				}
				return true;
			} catch (...) {
				spdlog::error("CinematicCamera: CinematicOrbit threw - refused.");
				g_overriding.store(false, std::memory_order_relaxed);
				if (g_frozeByUs) {
					if (auto* main = RE::Main::GetSingleton(); main) {
						main->GetRuntimeData().freezeTime = false;
					}
					g_frozeByUs = false;
				}
				return false;
			}
		}

		bool CinematicIsActive(RE::StaticFunctionTag*)
		{
			return g_run.phase == Phase::kRunning || g_run.phase == Phase::kEnding;
		}

		void CinematicStop(RE::StaticFunctionTag*)
		{
			try {
				if (g_run.phase == Phase::kRunning) {
					Finish("stopped", "native", true, true);
				} else if (g_run.phase == Phase::kEnding) {
					// The queued hand back is still on its way; do it now and let
					// the queued one find nothing to do.
					HandBack(true);
				}
			} catch (...) {
				spdlog::error("CinematicCamera: CinematicStop threw - swallowed.");
			}
		}
	}

	void Install()
	{
		if (g_installed) {
			return;
		}
		try {
			REL::Relocation<std::uintptr_t> free{ RE::VTABLE_FreeCameraState[0] };
			// TESCameraState: GetRotation is 04 / 05 and GetTranslation 05 / 06 on
			// SE-AE / VR - VR has an extra virtual at 03 (TESCameraState.h:31-37).
			const bool        vr = REL::Module::IsVR();
			const std::size_t iRot = vr ? 0x05 : 0x04;
			const std::size_t iTrans = vr ? 0x06 : 0x05;
			const std::size_t iFreeUpd = vr ? 0x04 : 0x03;  // TESCameraState::Update, TESCameraState.cpp:21
			GetRotationHook::func = free.write_vfunc(iRot, GetRotationHook::thunk);
			GetTranslationHook::func = free.write_vfunc(iTrans, GetTranslationHook::thunk);
			FreeUpdateHook::func = free.write_vfunc(iFreeUpd, FreeUpdateHook::thunk);

			REL::Relocation<std::uintptr_t> player{ RE::VTABLE_PlayerCharacter[0] };
			const std::size_t               iUpd = vr ? 0x0AF : 0x0AD;
			PlayerUpdateHook::func = player.write_vfunc(iUpd, PlayerUpdateHook::thunk);

			g_installed = true;
			spdlog::info("CinematicCamera: hooks installed (FreeCameraState Update @0x{:X}, GetRotation @0x{:X}, "
						 "GetTranslation @0x{:X}; PlayerCharacter Update @0x{:X}, {} runtime). Idle until CinematicApproach.",
				iFreeUpd, iRot, iTrans, iUpd, vr ? "VR" : "SE/AE");
		} catch (const std::exception& e) {
			spdlog::error("CinematicCamera: failed to install the hooks: {} - CinematicApproach will refuse.", e.what());
		} catch (...) {
			spdlog::error("CinematicCamera: failed to install the hooks - CinematicApproach will refuse.");
		}
	}

	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		try {
			switch (a_msg->type) {
			case SKSE::MessagingInterface::kPreLoadGame:
			case SKSE::MessagingInterface::kNewGame:
				if (g_run.phase == Phase::kRunning || g_run.phase == Phase::kEnding) {
					g_overriding.store(false, std::memory_order_relaxed);
					g_run.reason = "load";
					g_run.why = "";
					g_run.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_run.wallStart).count();
					Unfreeze(g_run);  // L-C2: a frozen orbit unfreezes on load too
					FlushLine(false, 0.0f);
					g_run.phase = Phase::kIdle;
					g_loadPending = true;
				}
				break;
			default:
				break;
			}
			if (g_loadPending && (a_msg->type == SKSE::MessagingInterface::kPostLoadGame ||
									 a_msg->type == SKSE::MessagingInterface::kNewGame)) {
				g_loadPending = false;
				if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->IsInFreeCameraMode()) {
					cam->ToggleFreeCameraMode(false);
				}
				SendDone("load");
			}
		} catch (...) {
			spdlog::error("CinematicCamera: the load handler threw - swallowed.");
		}
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			spdlog::error("CinematicCamera: null VM, cannot register natives.");
			return false;
		}

		a_vm->RegisterFunction("CinematicApproach", "Lodestone", CinematicApproach);
		a_vm->RegisterFunction("CinematicOrbit", "Lodestone", CinematicOrbit);
		a_vm->RegisterFunction("CinematicIsActive", "Lodestone", CinematicIsActive);
		a_vm->RegisterFunction("CinematicStop", "Lodestone", CinematicStop);

		Config::ForEachPair([](std::string_view a_key, std::string_view a_value) {
			if (a_key == "logcinematiccamera") {
				g_logFrames = (Config::Trim(a_value) == "1");
			}
		});

		spdlog::info("CinematicCamera: natives registered (CinematicApproach, CinematicOrbit, CinematicIsActive, CinematicStop). "
					 "Frame log {}.",
			g_logFrames ? "ON" : "off");
		return true;
	}
}
