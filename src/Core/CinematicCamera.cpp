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
// THE FRAME POINT. The pose is advanced in the PlayerCharacter::Update vtable
// slot, after the original has run (the same slot Incapacitation hooks; the two
// chain). Its delta is the frame time the game itself advanced, so a pause does
// not eat the approach.
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
//     handback=<d> drift=<d> qcheck=<q>
// handback = distance between the last point written and the position of the
// camera root two frames after the hand back. drift = largest distance between
// a point written and the camera root at the next frame while the approach ran
// (0 means the pose we return is the pose on screen). qcheck = |dot| between the
// third person rotation and the one rebuilt from the camera root's matrix at the
// start (1 means this file reads rotations the way the engine does).
// LogCinematicCamera = 1 adds one line per frame.
//
// Phase L-C1

#include "CinematicCamera.h"

#include "Config.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>

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
		};

		Run g_run;
		std::atomic<bool> g_overriding{ false };  // read by the vtable hooks
		bool g_loadPending = false;

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
			if (a_measured) {
				spdlog::info("CinematicCamera: end reason={}{}{} start=({:.1f},{:.1f},{:.1f}) end=({:.1f},{:.1f},{:.1f}) "
							 "frames={} ms={:.0f} handback={:.2f} drift={:.2f} qcheck={:.4f}",
					r.reason, *r.why ? " why=" : "", r.why, r.start.x, r.start.y, r.start.z, r.lastWritten.x,
					r.lastWritten.y, r.lastWritten.z, r.frames, r.ms, a_handback, r.maxDrift, r.qcheck);
			} else {
				spdlog::info("CinematicCamera: end reason={}{}{} start=({:.1f},{:.1f},{:.1f}) end=({:.1f},{:.1f},{:.1f}) "
							 "frames={} ms={:.0f} handback=na drift={:.2f} qcheck={:.4f}",
					r.reason, *r.why ? " why=" : "", r.why, r.start.x, r.start.y, r.start.z, r.lastWritten.x,
					r.lastWritten.y, r.lastWritten.z, r.frames, r.ms, r.maxDrift, r.qcheck);
			}
		}

		// Hands the camera back and announces. Main thread only.
		void HandBack(bool a_toggle)
		{
			if (a_toggle) {
				if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->IsInFreeCameraMode()) {
					cam->ToggleFreeCameraMode(false);
				}
			}
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
		void Step(float a_delta)
		{
			auto& r = g_run;

			if (r.phase == Phase::kHandback) {
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

			// The camera root, one frame after the point written: the measure of
			// whether what we return is what is on screen.
			if (r.haveLast && cam->cameraRoot) {
				const float d = Dist(ToVec(cam->cameraRoot->world.translate), r.lastWritten);
				if (d > r.maxDrift) {
					r.maxDrift = d;
				}
			}

			const auto  wall = std::chrono::duration<float>(std::chrono::steady_clock::now() - r.wallStart).count();
			const auto  actorPtr = r.target.get();
			auto*       actor = actorPtr.get();
			if (!actor || actor->IsDead() || !actor->Is3DLoaded()) {
				Finish("stopped", "target-gone", false, true);
				return;
			}
			if (wall > r.seconds + kSafetySeconds) {
				Finish("timeout", "", false, true);
				return;
			}

			r.elapsed += a_delta;
			++r.frames;
			float t = r.seconds > 0.0f ? r.elapsed / r.seconds : 1.0f;
			if (t > 1.0f) {
				t = 1.0f;
			}
			const float s = t * t * (3.0f - 2.0f * t);

			r.curPos = Lerp(r.start, r.end, s);
			const auto aim = LookAt(r.curPos, HeadOf(actor));
			r.curRot = Slerp(aim, r.endRot, s);
			r.lastWritten = r.curPos;
			r.haveLast = true;

			if (g_logFrames) {
				spdlog::info("CinematicCamera: frame {} t={:.3f} pos=({:.1f},{:.1f},{:.1f}) drift={:.2f}", r.frames, t,
					r.curPos.x, r.curPos.y, r.curPos.z, r.maxDrift);
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
					Step(a_delta);
				} catch (...) {
					spdlog::error("CinematicCamera: the frame step threw - swallowed.");
				}
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
				cam->ToggleFreeCameraMode(false);
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
			GetRotationHook::func = free.write_vfunc(iRot, GetRotationHook::thunk);
			GetTranslationHook::func = free.write_vfunc(iTrans, GetTranslationHook::thunk);

			REL::Relocation<std::uintptr_t> player{ RE::VTABLE_PlayerCharacter[0] };
			const std::size_t               iUpd = vr ? 0x0AF : 0x0AD;
			PlayerUpdateHook::func = player.write_vfunc(iUpd, PlayerUpdateHook::thunk);

			g_installed = true;
			spdlog::info("CinematicCamera: hooks installed (FreeCameraState GetRotation @0x{:X}, GetTranslation @0x{:X}; "
						 "PlayerCharacter Update @0x{:X}, {} runtime). Idle until CinematicApproach.",
				iRot, iTrans, iUpd, vr ? "VR" : "SE/AE");
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
		a_vm->RegisterFunction("CinematicIsActive", "Lodestone", CinematicIsActive);
		a_vm->RegisterFunction("CinematicStop", "Lodestone", CinematicStop);

		Config::ForEachPair([](std::string_view a_key, std::string_view a_value) {
			if (a_key == "logcinematiccamera") {
				g_logFrames = (Config::Trim(a_value) == "1");
			}
		});

		spdlog::info("CinematicCamera: natives registered (CinematicApproach, CinematicIsActive, CinematicStop). "
					 "Frame log {}.",
			g_logFrames ? "ON" : "off");
		return true;
	}
}
