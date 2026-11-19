// Portrait.cpp
// Lodestone - Shared SKSE framework
//
// Lodestone.CapturePlayerPortrait(String asFolder, Int aiSize) -> String
//
// HOW ONE CAPTURE GOES. The native only files a request; the work is done in
// a mid hook at the entry of the engine's frame swap (Main::Swap,
// RELOCATION_ID(35556, 36555)), where the immediate context is the engine's
// and no draw is in flight:
//
//   take (swap K)     refuse, or switch the player's head tracking off (the
//                     actor state bit, the IsNPC graph variable and, when True
//                     Directional Movement is loaded, its own head tracking
//                     through its API)
//   settle            when the head is turned away from the body's front,
//                     wait until it comes back, stops moving, or a frame cap
//   arm               a pose 75 units in front of the head, looking at it,
//                     and cull the third person geometry of an armour in the
//                     head, hair, long hair and circlet slots
//   next 3 updates    three vtable hooks on the plain ThirdPersonState
//                     (Update 03, GetRotation 04, GetTranslation 05) write the
//                     pose after the state's own update, and the engine
//                     function that rebuilds the camera's worldToCam (SE 69271
//                     / AE 70641) is called after each write
//   swap after the    the head guard: the face more than 30 degrees off the
//   third update      camera fails the capture; else copy of the square
//                     around the head from renderTargets[kMAIN] to a staging
//                     texture; disarm, put the culled nodes and the head
//                     tracking back
//   next swaps        Map with D3D11_MAP_FLAG_DO_NOT_WAIT, once per frame,
//                     until the GPU is done
//   worker thread     conversion to RGBA8, scale to aiSize, PNG by WIC to
//                     Data\PrismaUI\views\<folder>\ and Data\Magelight\<folder>\
//                     under a new name; the previous capture of the folder is
//                     deleted; the waiting script gets the name on the game
//                     thread
//
// That sequence is the one measured in game (pose held for three camera
// updates plus the worldToCam rebuild): front and centred in every PNG on both
// backends. Holding the pose without the rebuild showed the face off centre
// (the crop box is projected through worldToCam); the rebuild alone showed
// the back.
//
// THE CROP. The renderer's eye (RendererShadowState posAdjust) is checked
// against the pose at the copy: at the pose, the frame is the pose's and the
// head is in its centre, so the crop is centred and sized from the distance
// and the frustum. The scene camera's worldToCam places the crop when the
// eye is not at the pose. Neither number proves the frame: in one interior
// the eye read ~634 units off while the frame was the pose's.
//
// THE HEAD. The pose follows the body's heading; a head turned by head
// tracking shows in profile from it. The face direction is the head node's
// local +Y: in the vanilla skeleton (male and female) the head's bind
// rotation against NPC Root is the identity within 0.011, and the eye bone
// and the head magic node sit 8.1 and 11.5 units along +Y from it.
//
// The camera state's own fields are never written, so the update after the
// copy builds the usual pose: the player sees three frames from the front.
// Nothing is unequipped, nothing is created, nothing goes into the save. With
// no request open the hook does one atomic load per frame.
//
// THE CONVERSION. kMAIN is a float target (HDR) on the measured setups; it is
// mapped with x / (1 + x) and a 1/2.2 gamma. 8 bit and 10 bit targets are
// copied as they are.

#include "Portrait.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <safetyhook.hpp>

#include "RE/N/NativeLatentFunction.h"

#pragma comment(lib, "windowscodecs.lib")

namespace Lodestone::Core::Portrait
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		double MsSince(Clock::time_point a_t0)
		{
			return std::chrono::duration<double, std::milli>(Clock::now() - a_t0).count();
		}

		constexpr float        kFrontDist = 75.0f;   // units in front of the head
		constexpr float        kFrontUp = 4.0f;      // units above the head
		constexpr float        kCrop = 2.4f;         // crop side, in units of the 40 unit head span
		constexpr float        kHeadSpan = 40.0f;    // units of the head span the crop is sized on
		constexpr float        kPoseTol = 1.0f;      // renderer eye within this of the pose = the frame is the pose's
		constexpr std::int32_t kHoldFrames = 3;      // camera updates with the pose before the copy
		constexpr std::int32_t kMaxWaitFrames = 6;   // extra frames to wait for them (a paused menu)
		constexpr std::int32_t kMaxMapFrames = 120;  // frames to wait for the GPU
		constexpr float        kHeadReady = 15.0f;   // degrees face to camera: the head is back in front, arm the pose
		constexpr float        kHeadLimit = 30.0f;   // degrees face to camera at the copy: past this the capture fails
		constexpr float        kHeadStill = 0.25f;   // degrees per frame: the head stopped moving
		constexpr std::int32_t kStillFrames = 3;     // frames under kHeadStill that end the wait
		constexpr std::int32_t kMinSettleFrames = 5; // frames before a still head ends the wait
		constexpr std::int32_t kMaxSettleFrames = 30;  // frames to wait for the head at most
		constexpr std::int32_t kMinSize = 16;
		constexpr std::int32_t kMaxSize = 1024;
		constexpr std::size_t  kMaxFolder = 128;
		constexpr const char*  kPrefix = "lodestone-portrait-";

		struct Job
		{
			std::int32_t      seq = 0;
			std::string       folder;
			std::int32_t      size = 256;
			Clock::time_point t0;
			// take and pose
			std::int32_t armFrames = 0;    // swaps from the take to the copy
			std::int32_t poseFrames = 0;   // of those, swaps after a camera update that carried the pose
			std::int32_t matrixCalls = 0;  // worldToCam rebuilds
			std::string  helmet;           // what was culled, slot:formid
			float        shadowToPose = -1.0f;  // renderer camera position vs the pose, at the copy
			float        camToPose = -1.0f;     // scene camera world position vs the pose, at the copy
			float        rootToPose = -1.0f;    // camera root world position vs the pose, at the copy
			std::string  slots;                 // who owns the ThirdPersonState slots 03/04/05 at the take
			std::string  crop;                  // what placed the crop: "pose" or "matrix"
			std::string  port;                  // the head through the scene camera's worldToCam
			std::string  diag;                  // frustum, port rect and worldToCam, when the matrix was not used
			// head
			Clock::time_point takeAt;
			std::int32_t settleFrames = 0;   // swaps waited for the head before the pose was armed
			double       settleMs = 0.0;     // time from the take to the pose
			std::string  settle = "none";    // what ended the wait: front, still, cap, or none
			float        headTake = -1.0f;   // degrees face to camera at the take
			float        headArm = -1.0f;    // at the pose
			float        headCopy = -1.0f;   // at the copy
			float        yawCopy = 0.0f;     // camera in the head's frame at the copy: yaw to the head's right
			float        pitchCopy = 0.0f;   // and pitch above its forward axis
			float        axisDeg = -1.0f;    // face direction vs the head magic node, at the take
			std::string  track;              // head tracking before the take and what TDM answered
			// copy
			std::uint32_t format = 0;
			std::uint32_t texW = 0;
			std::uint32_t texH = 0;
			std::uint32_t boxX = 0;
			std::uint32_t boxY = 0;
			std::uint32_t boxS = 0;
			std::uint32_t rowBytes = 0;
			std::int32_t  mapTries = 0;
			double        hookMaxMs = 0.0;
			std::vector<std::uint8_t> raw;
			// outcome
			bool        ok = false;
			std::string why;
		};

		// The script waiting for the capture. Taken exactly once: by the
		// answer, or by a load.
		struct Waiter
		{
			std::int32_t                            seq = 0;
			RE::BSScript::Internal::VirtualMachine* vm = nullptr;
			RE::VMStackID                           stack = 0;
		};

		std::mutex            g_waitLock;
		std::optional<Waiter> g_waiter;

		std::atomic<std::int32_t> g_seq{ 0 };
		std::atomic<bool>         g_busy{ false };   // a request is filed or in flight
		std::atomic<bool>         g_ready{ false };  // every hook and the rebuild function are in place

		// Native -> hook hand over.
		std::mutex           g_reqLock;
		std::shared_ptr<Job> g_request;
		std::atomic<bool>    g_hasRequest{ false };
		SafetyHookMid        g_swapHook;

		// Hook thread state of the job in flight.
		enum class Phase
		{
			kIdle,
			kSettling,
			kArmed,
			kMapping,
		};
		Phase                g_phase = Phase::kIdle;
		std::shared_ptr<Job> g_active;
		ID3D11Texture2D*     g_staging = nullptr;
		std::int32_t         g_lastWrites = 0;

		// Read by the camera hooks.
		std::atomic<bool>         g_armed{ false };
		std::atomic<std::int32_t> g_writes{ 0 };
		std::atomic<std::int32_t> g_matrixCalls{ 0 };
		RE::NiPoint3              g_frontPos;
		RE::NiMatrix3             g_frontRot;
		RE::NiQuaternion          g_frontQuat{ 1, 0, 0, 0 };
		std::vector<RE::NiPointer<RE::NiAVObject>> g_hidden;

		// The wait for the head (hook thread).
		float        g_prevHead = -1.0f;
		std::int32_t g_stillFrames = 0;

		// The player's head tracking, switched off from the take until the
		// copy and put back as it was.
		struct Tracking
		{
			bool        active = false;   // switched off by the capture in flight
			bool        bitPrev = false;  // ActorState2 headTracking before the take
			bool        npcRead = false;  // the player's graph has IsNPC
			bool        npcPrev = false;  // its value before the take
			bool        tdmOwned = false; // True Directional Movement handed this plugin its switch
		};
		Tracking g_track;

		// The engine function that rebuilds NiCamera::worldToCam from the
		// camera's world transform. Not in the pinned CommonLib; the id is the
		// one SmoothCam (mwilsnd, GPL-3.0) calls after it moves the camera.
		using ToScreenFn = void (*)(RE::NiCamera*);
		ToScreenFn g_toScreen = nullptr;

		// ---------------------------------------------------------------
		// game thread hand back
		// ---------------------------------------------------------------

		void DispatchToGame(std::function<void()> a_work)
		{
			try {
				auto* task = SKSE::GetTaskInterface();
				if (!task) {
					spdlog::error("Portrait: no SKSE task interface - a waiting script stays parked.");
					return;
				}
				task->AddTask(std::move(a_work));
			} catch (...) {
				spdlog::error("Portrait: AddTask threw - a waiting script stays parked.");
			}
		}

		void ReturnToScript(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stack, const std::string& a_value)
		{
			if (!a_vm) {
				return;
			}
			try {
				a_vm->ReturnLatentResult(a_stack, RE::BSFixedString(a_value.c_str()));
			} catch (...) {
				spdlog::error("Portrait: returning to stack {} threw - that script stays parked.", a_stack);
			}
		}

		// Worker or hook thread: the waiting script of capture a_seq gets
		// a_value on the game thread, unless a load already answered it.
		void Answer(std::int32_t a_seq, std::string a_value)
		{
			DispatchToGame([a_seq, value = std::move(a_value)]() {
				std::optional<Waiter> w;
				{
					std::scoped_lock l(g_waitLock);
					if (g_waiter && g_waiter->seq == a_seq) {
						w = g_waiter;
						g_waiter.reset();
					}
				}
				if (w) {
					ReturnToScript(w->vm, w->stack, value);
				}
			});
		}

		// ---------------------------------------------------------------
		// the front pose
		// ---------------------------------------------------------------

		float Dist3(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		// Same convention as CinematicCamera: the camera root looks along its
		// local +Y, +Z up, +X right; columns are the axes.
		RE::NiMatrix3 LookAtMatrix(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
		{
			RE::NiPoint3 f{ a_to.x - a_from.x, a_to.y - a_from.y, a_to.z - a_from.z };
			RE::NiMatrix3 m;
			const float fl = f.Length();
			if (fl < 1e-4f) {
				return m;
			}
			f = { f.x / fl, f.y / fl, f.z / fl };
			RE::NiPoint3 r{ f.y, -f.x, 0.0f };
			float rl = std::sqrt(r.x * r.x + r.y * r.y);
			if (rl < 1e-4f) {
				r = { 1, 0, 0 };
				rl = 1;
			}
			r = { r.x / rl, r.y / rl, 0.0f };
			const RE::NiPoint3 u{ r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x };
			m.entry[0][0] = r.x;
			m.entry[1][0] = r.y;
			m.entry[2][0] = r.z;
			m.entry[0][1] = f.x;
			m.entry[1][1] = f.y;
			m.entry[2][1] = f.z;
			m.entry[0][2] = u.x;
			m.entry[1][2] = u.y;
			m.entry[2][2] = u.z;
			return m;
		}

		RE::NiQuaternion QuatFromMatrix(const RE::NiMatrix3& m)
		{
			const auto& e = m.entry;
			const float tr = e[0][0] + e[1][1] + e[2][2];
			RE::NiQuaternion q(1, 0, 0, 0);
			if (tr > 0.0f) {
				const float s = std::sqrt(tr + 1.0f) * 2.0f;
				q = RE::NiQuaternion(0.25f * s, (e[2][1] - e[1][2]) / s, (e[0][2] - e[2][0]) / s, (e[1][0] - e[0][1]) / s);
			} else if (e[0][0] > e[1][1] && e[0][0] > e[2][2]) {
				const float s = std::sqrt(1.0f + e[0][0] - e[1][1] - e[2][2]) * 2.0f;
				q = RE::NiQuaternion((e[2][1] - e[1][2]) / s, 0.25f * s, (e[0][1] + e[1][0]) / s, (e[0][2] + e[2][0]) / s);
			} else if (e[1][1] > e[2][2]) {
				const float s = std::sqrt(1.0f + e[1][1] - e[0][0] - e[2][2]) * 2.0f;
				q = RE::NiQuaternion((e[0][2] - e[2][0]) / s, (e[0][1] + e[1][0]) / s, 0.25f * s, (e[1][2] + e[2][1]) / s);
			} else {
				const float s = std::sqrt(1.0f + e[2][2] - e[0][0] - e[1][1]) * 2.0f;
				q = RE::NiQuaternion((e[1][0] - e[0][1]) / s, (e[0][2] + e[2][0]) / s, (e[1][2] + e[2][1]) / s, 0.25f * s);
			}
			const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
			return n < 1e-6f ? RE::NiQuaternion(1, 0, 0, 0) : RE::NiQuaternion(q.w / n, q.x / n, q.y / n, q.z / n);
		}

		// After the state's own update: the pose on the camera root and the
		// scene camera, then the worldToCam rebuild.
		void WriteFrontPose()
		{
			auto* cam = RE::PlayerCamera::GetSingleton();
			auto* root = cam ? cam->cameraRoot.get() : nullptr;
			if (!root) {
				return;
			}
			RE::NiTransform want;
			want.rotate = g_frontRot;
			want.translate = g_frontPos;
			want.scale = root->world.scale;
			root->local = root->parent ? root->parent->world.Invert() * want : want;
			root->world = want;
			auto* niCam = RE::Main::WorldRootCamera();
			if (niCam && niCam->parent == root) {
				niCam->world = want * niCam->local;
			}
			if (niCam && g_toScreen) {
				g_toScreen(niCam);
				g_matrixCalls.fetch_add(1, std::memory_order_relaxed);
			}
		}

		struct StateUpdateHook
		{
			static void thunk(RE::TESCameraState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_next)
			{
				func(a_this, a_next);
				if (g_armed.load(std::memory_order_acquire)) {
					try {
						WriteFrontPose();
						g_writes.fetch_add(1, std::memory_order_relaxed);
					} catch (...) {
					}
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct StateTranslationHook
		{
			static void thunk(RE::TESCameraState* a_this, RE::NiPoint3& a_out)
			{
				func(a_this, a_out);
				if (g_armed.load(std::memory_order_acquire)) {
					a_out = g_frontPos;
					g_writes.fetch_add(1, std::memory_order_relaxed);
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct StateRotationHook
		{
			static void thunk(RE::TESCameraState* a_this, RE::NiQuaternion& a_out)
			{
				func(a_this, a_out);
				if (g_armed.load(std::memory_order_acquire)) {
					a_out = g_frontQuat;
					g_writes.fetch_add(1, std::memory_order_relaxed);
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Culls the third person geometry of the head, hair, long hair and
		// circlet slots when an armour fills them. Only the node's own
		// AppCulled flag, and only on nodes that were not culled, so the
		// restore puts back exactly what was.
		std::string HideHelmet(RE::PlayerCharacter* a_player)
		{
			std::string out;
			const auto& biped = a_player->GetBiped(false);
			if (!biped) {
				return "no biped";
			}
			static constexpr RE::BIPED_OBJECTS::BIPED_OBJECT kSlots[] = { RE::BIPED_OBJECTS::kHead, RE::BIPED_OBJECTS::kHair,
				RE::BIPED_OBJECTS::kLongHair, RE::BIPED_OBJECTS::kCirclet };
			for (auto slot : kSlots) {
				auto& obj = biped->objects[slot];
				if (!obj.item || obj.item->GetFormType() != RE::FormType::Armor || !obj.partClone) {
					continue;
				}
				if (!obj.partClone->GetAppCulled()) {
					obj.partClone->SetAppCulled(true);
					g_hidden.push_back(obj.partClone);
				}
				out += std::format("{}{}:{:08X}", out.empty() ? "" : "|", static_cast<std::uint32_t>(slot) + 30, obj.item->GetFormID());
			}
			return out.empty() ? "none" : out;
		}

		// ---------------------------------------------------------------
		// the head
		// ---------------------------------------------------------------

		// True Directional Movement (ersh1) turns the player's head with its
		// own head tracking and has a plugin API to switch it off. Only the
		// order of the version 1 interface's virtual functions is declared
		// here; two of them are called. Absent DLL or export: skipped.
		namespace Tdm
		{
			enum class Result : std::uint8_t
			{
				kOK,
				kNotOwner,
				kMustKeep,
				kAlreadyGiven,
				kAlreadyTaken,
				kBadThread,
			};

			class IV1
			{
			public:
				virtual unsigned long      GetThreadId() const noexcept = 0;
				virtual bool               GetDirectionalMovementState() const noexcept = 0;
				virtual bool               GetTargetLockState() const noexcept = 0;
				virtual RE::ActorHandle    GetCurrentTarget() const noexcept = 0;
				virtual Result             RequestDisableDirectionalMovement(SKSE::PluginHandle) noexcept = 0;
				virtual Result             RequestDisableHeadtracking(SKSE::PluginHandle) noexcept = 0;
				virtual SKSE::PluginHandle GetDisableDirectionalMovementOwner() const noexcept = 0;
				virtual SKSE::PluginHandle GetDisableHeadtrackingOwner() const noexcept = 0;
				virtual Result             ReleaseDisableDirectionalMovement(SKSE::PluginHandle) noexcept = 0;
				virtual Result             ReleaseDisableHeadtracking(SKSE::PluginHandle) noexcept = 0;
			};

			using RequestFn = void* (*)(std::uint8_t);  // interface version, 0 = V1

			const char* Name(Result a_r)
			{
				switch (a_r) {
				case Result::kOK:
					return "ok";
				case Result::kNotOwner:
					return "not owner";
				case Result::kMustKeep:
					return "must keep";
				case Result::kAlreadyGiven:
					return "already given";
				case Result::kAlreadyTaken:
					return "already taken";
				case Result::kBadThread:
					return "bad thread";
				}
				return "unknown";
			}

			// Looked up once, on the first capture.
			IV1* Api(std::string& a_state)
			{
				static bool        looked = false;
				static IV1*        api = nullptr;
				static std::string state = "absent";
				if (!looked) {
					looked = true;
					if (HMODULE mod = GetModuleHandleW(L"TrueDirectionalMovement.dll")) {
						state = "no RequestPluginAPI";
						if (auto fn = reinterpret_cast<RequestFn>(GetProcAddress(mod, "RequestPluginAPI"))) {
							api = static_cast<IV1*>(fn(0));
							state = api ? "v1" : "no v1 interface";
						}
					}
				}
				a_state = state;
				return api;
			}
		}

		RE::NiAVObject* HeadNode(RE::PlayerCharacter* a_player, RE::NiAVObject** a_root = nullptr)
		{
			auto* root = a_player ? a_player->Get3D(false) : nullptr;
			if (a_root) {
				*a_root = root;
			}
			return root ? root->GetObjectByName(RE::BSFixedString("NPC Head [Head]")) : nullptr;
		}

		RE::NiPoint3 FrontOf(const RE::NiPoint3& a_head, float a_heading)
		{
			return RE::NiPoint3(a_head.x + std::sin(a_heading) * kFrontDist, a_head.y + std::cos(a_heading) * kFrontDist, a_head.z + kFrontUp);
		}

		constexpr float kRadToDeg = 57.2957795f;

		float AngleDeg(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const float la = a.Length(), lb = b.Length();
			if (la < 1e-4f || lb < 1e-4f) {
				return -1.0f;
			}
			return std::acos((std::clamp)(a.Dot(b) / (la * lb), -1.0f, 1.0f)) * kRadToDeg;
		}

		// Where a_eye is seen from the face. The face direction is the head
		// node's local +Y, column 1 of its world rotation.
		struct Aim
		{
			float deg = -1.0f;   // face direction to the eye
			float yaw = 0.0f;    // the eye in the head's frame: towards its +X (right)
			float pitch = 0.0f;  // and above its forward axis
			float axis = -1.0f;  // face direction to the head magic node
		};

		Aim AimAt(RE::NiAVObject* a_head, RE::NiAVObject* a_root, const RE::NiPoint3& a_eye)
		{
			Aim out;
			const auto& r = a_head->world.rotate.entry;
			const RE::NiPoint3 face(r[0][1], r[1][1], r[2][1]);
			const RE::NiPoint3 h = a_head->world.translate;
			const RE::NiPoint3 v(a_eye.x - h.x, a_eye.y - h.y, a_eye.z - h.z);
			out.deg = AngleDeg(face, v);
			const float lx = r[0][0] * v.x + r[1][0] * v.y + r[2][0] * v.z;
			const float ly = face.Dot(v);
			const float lz = r[0][2] * v.x + r[1][2] * v.y + r[2][2] * v.z;
			out.yaw = std::atan2(lx, ly) * kRadToDeg;
			out.pitch = std::atan2(lz, std::sqrt(lx * lx + ly * ly)) * kRadToDeg;
			if (a_root) {
				if (auto* mag = a_root->GetObjectByName(RE::BSFixedString("NPC Head MagicNode [Hmag]"))) {
					const RE::NiPoint3 m = mag->world.translate;
					out.axis = AngleDeg(face, RE::NiPoint3(m.x - h.x, m.y - h.y, m.z - h.z));
				}
			}
			return out;
		}

		// Every frame from the take to the copy: the engine and TDM can switch
		// it back on in their own updates.
		void TrackingOff(RE::PlayerCharacter* a_player)
		{
			if (auto* st = a_player->AsActorState()) {
				st->actorState2.headTracking = false;
			}
			if (g_track.npcRead) {
				a_player->SetGraphVariableBool(RE::BSFixedString("IsNPC"), false);
			}
		}

		// The take: remember, switch off, ask TDM. Returns what was found.
		std::string SuspendTracking(RE::PlayerCharacter* a_player)
		{
			g_track = Tracking{};
			if (auto* st = a_player->AsActorState()) {
				g_track.bitPrev = st->actorState2.headTracking;
			}
			bool npc = false;
			g_track.npcRead = a_player->GetGraphVariableBool(RE::BSFixedString("IsNPC"), npc);
			g_track.npcPrev = npc;
			std::string tdm;
			if (auto* api = Tdm::Api(tdm)) {
				const auto res = api->RequestDisableHeadtracking(SKSE::GetPluginHandle());
				g_track.tdmOwned = res == Tdm::Result::kOK;
				tdm = std::format("v1 {}", Tdm::Name(res));
			}
			g_track.active = true;
			TrackingOff(a_player);
			return std::format("bit:{} isNPC:{} tdm:{}", g_track.bitPrev ? 1 : 0, g_track.npcRead ? (g_track.npcPrev ? "1" : "0") : "absent", tdm);
		}

		// Puts back what the take found. Safe to call more than once.
		void ResumeTracking()
		{
			if (!g_track.active) {
				return;
			}
			g_track.active = false;
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				if (auto* st = player->AsActorState()) {
					st->actorState2.headTracking = g_track.bitPrev;
				}
				if (g_track.npcRead) {
					player->SetGraphVariableBool(RE::BSFixedString("IsNPC"), g_track.npcPrev);
				}
			}
			if (g_track.tdmOwned) {
				g_track.tdmOwned = false;
				std::string tdm;
				if (auto* api = Tdm::Api(tdm)) {
					const auto res = api->ReleaseDisableHeadtracking(SKSE::GetPluginHandle());
					if (res != Tdm::Result::kOK) {
						spdlog::warn("Portrait: TDM did not take its head tracking switch back: {}.", Tdm::Name(res));
					}
				}
			}
		}

		void Disarm()
		{
			g_armed.store(false, std::memory_order_release);
			for (auto& n : g_hidden) {
				if (n) {
					n->SetAppCulled(false);
				}
			}
			g_hidden.clear();
			ResumeTracking();
		}

		// The camera position the renderer used for the last frame. Measured
		// to agree with what the PNG shows; the scene camera's worldToCam did
		// not. SE/AE layout only (VR installs nothing).
		bool ShadowEye(RE::NiPoint3& a_out)
		{
			auto* rss = RE::BSGraphics::RendererShadowState::GetSingleton();
			if (!rss) {
				return false;
			}
			a_out = rss->GetRuntimeData().posAdjust.getEye(0);
			return true;
		}

		// Where a vtable slot points: "ours", or module+offset. Read at the
		// take, so a plugin that wrote the slot after this one installed shows.
		std::string SlotOwner(std::uintptr_t a_fn, std::uintptr_t a_ours)
		{
			if (a_fn == a_ours) {
				return "ours";
			}
			try {
				HMODULE mod = nullptr;
				if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCWSTR>(a_fn), &mod) &&
					mod) {
					wchar_t buf[MAX_PATH]{};
					const DWORD n = GetModuleFileNameW(mod, buf, MAX_PATH);
					const auto name = std::filesystem::path(std::wstring(buf, n)).filename().string();
					return std::format("{}+0x{:X}", name, a_fn - reinterpret_cast<std::uintptr_t>(mod));
				}
			} catch (...) {
			}
			return std::format("0x{:X}", a_fn);
		}

		std::string SlotOwners()
		{
			REL::Relocation<std::uintptr_t> third{ RE::VTABLE_ThirdPersonState[0] };
			const auto* vt = reinterpret_cast<const std::uintptr_t*>(third.address());
			return std::format("03={} 04={} 05={}", SlotOwner(vt[0x03], reinterpret_cast<std::uintptr_t>(&StateUpdateHook::thunk)),
				SlotOwner(vt[0x04], reinterpret_cast<std::uintptr_t>(&StateRotationHook::thunk)),
				SlotOwner(vt[0x05], reinterpret_cast<std::uintptr_t>(&StateTranslationHook::thunk)));
		}

		// The scene camera's frustum, port rect and worldToCam, raw: what the
		// head was projected through when that projection was not used.
		std::string CameraDiag(RE::NiCamera* a_cam)
		{
			const auto& d2 = a_cam->GetRuntimeData2();
			const auto& f = d2.viewFrustum;
			// NiRect keeps left, right, top, bottom protected; same order, 4 floats.
			float r[4]{};
			static_assert(sizeof(d2.port) == sizeof(r));
			std::memcpy(r, &d2.port, sizeof(r));
			const auto& m = a_cam->GetRuntimeData().worldToCam;
			std::string w2c;
			for (int i = 0; i < 4; ++i) {
				w2c += std::format("{}{:.6g},{:.6g},{:.6g},{:.6g}", i ? "|" : "", m[i][0], m[i][1], m[i][2], m[i][3]);
			}
			return std::format("frustum=({:.4f},{:.4f},{:.4f},{:.4f},{:.2f},{:.0f},{}) portRect=({:.3f},{:.3f},{:.3f},{:.3f}) worldToCam=[{}]",
				f.fLeft, f.fRight, f.fTop, f.fBottom, f.fNear, f.fFar, f.bOrtho ? "ortho" : "persp", r[0], r[1], r[2], r[3], w2c);
		}

		// ---------------------------------------------------------------
		// pixels and files
		// ---------------------------------------------------------------

		float HalfToFloat(std::uint16_t a_h)
		{
			const std::uint32_t sign = (a_h >> 15) & 1u;
			const std::uint32_t exp = (a_h >> 10) & 0x1Fu;
			const std::uint32_t man = a_h & 0x3FFu;
			float v;
			if (exp == 0) {
				v = std::ldexp(static_cast<float>(man), -24);
			} else if (exp == 31) {
				v = man ? 0.0f : 65504.0f;
			} else {
				v = std::ldexp(static_cast<float>(man | 0x400u), static_cast<int>(exp) - 25);
			}
			return sign ? -v : v;
		}

		// Unsigned small float: a_manBits mantissa bits, 5 exponent bits, bias 15.
		float SmallFloat(std::uint32_t a_bits, int a_manBits)
		{
			const std::uint32_t man = a_bits & ((1u << a_manBits) - 1u);
			const std::uint32_t exp = (a_bits >> a_manBits) & 0x1Fu;
			if (exp == 0) {
				return std::ldexp(static_cast<float>(man), -14 - a_manBits);
			}
			if (exp == 31) {
				return 65504.0f;
			}
			return std::ldexp(static_cast<float>(man | (1u << a_manBits)), static_cast<int>(exp) - 15 - a_manBits);
		}

		std::uint8_t ToneMap(float a_x)
		{
			if (!(a_x > 0.0f)) {
				return 0;
			}
			const float t = a_x / (1.0f + a_x);
			const float g = std::pow(t, 1.0f / 2.2f);
			return static_cast<std::uint8_t>(std::lround((std::clamp)(g, 0.0f, 1.0f) * 255.0f));
		}

		bool ConvertRow(DXGI_FORMAT a_fmt, const std::uint8_t* a_src, std::uint8_t* a_dst, std::uint32_t a_w)
		{
			switch (a_fmt) {
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				std::memcpy(a_dst, a_src, static_cast<std::size_t>(a_w) * 4);
				return true;
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8X8_UNORM:
			case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8X8_TYPELESS:
				for (std::uint32_t x = 0; x < a_w; ++x) {
					a_dst[x * 4 + 0] = a_src[x * 4 + 2];
					a_dst[x * 4 + 1] = a_src[x * 4 + 1];
					a_dst[x * 4 + 2] = a_src[x * 4 + 0];
					a_dst[x * 4 + 3] = 255;
				}
				return true;
			case DXGI_FORMAT_R10G10B10A2_UNORM:
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				for (std::uint32_t x = 0; x < a_w; ++x) {
					std::uint32_t p;
					std::memcpy(&p, a_src + x * 4, 4);
					a_dst[x * 4 + 0] = static_cast<std::uint8_t>(((p >> 0) & 0x3FFu) >> 2);
					a_dst[x * 4 + 1] = static_cast<std::uint8_t>(((p >> 10) & 0x3FFu) >> 2);
					a_dst[x * 4 + 2] = static_cast<std::uint8_t>(((p >> 20) & 0x3FFu) >> 2);
					a_dst[x * 4 + 3] = 255;
				}
				return true;
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				for (std::uint32_t x = 0; x < a_w; ++x) {
					std::uint16_t c[4];
					std::memcpy(c, a_src + x * 8, 8);
					a_dst[x * 4 + 0] = ToneMap(HalfToFloat(c[0]));
					a_dst[x * 4 + 1] = ToneMap(HalfToFloat(c[1]));
					a_dst[x * 4 + 2] = ToneMap(HalfToFloat(c[2]));
					a_dst[x * 4 + 3] = 255;
				}
				return true;
			case DXGI_FORMAT_R11G11B10_FLOAT:
				for (std::uint32_t x = 0; x < a_w; ++x) {
					std::uint32_t p;
					std::memcpy(&p, a_src + x * 4, 4);
					a_dst[x * 4 + 0] = ToneMap(SmallFloat(p & 0x7FFu, 6));
					a_dst[x * 4 + 1] = ToneMap(SmallFloat((p >> 11) & 0x7FFu, 6));
					a_dst[x * 4 + 2] = ToneMap(SmallFloat((p >> 22) & 0x3FFu, 5));
					a_dst[x * 4 + 3] = 255;
				}
				return true;
			default:
				return false;
			}
		}

		std::uint32_t BytesPerPixel(DXGI_FORMAT a_fmt)
		{
			switch (a_fmt) {
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				return 8;
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8X8_UNORM:
			case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8X8_TYPELESS:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			case DXGI_FORMAT_R11G11B10_FLOAT:
				return 4;
			default:
				return 0;
			}
		}

		// Box filter from the a_srcSide square to a_size x a_size.
		std::vector<std::uint8_t> Scale(const std::vector<std::uint8_t>& a_src, std::uint32_t a_srcSide, std::uint32_t a_size)
		{
			std::vector<std::uint8_t> out(static_cast<std::size_t>(a_size) * a_size * 4, 0);
			const float step = static_cast<float>(a_srcSide) / static_cast<float>(a_size);
			for (std::uint32_t oy = 0; oy < a_size; ++oy) {
				const auto sy0 = static_cast<std::uint32_t>(oy * step);
				const auto sy1 = (std::max)(sy0 + 1, (std::min)(a_srcSide, static_cast<std::uint32_t>((oy + 1) * step)));
				for (std::uint32_t ox = 0; ox < a_size; ++ox) {
					const auto sx0 = static_cast<std::uint32_t>(ox * step);
					const auto sx1 = (std::max)(sx0 + 1, (std::min)(a_srcSide, static_cast<std::uint32_t>((ox + 1) * step)));
					std::uint32_t acc[3] = { 0, 0, 0 };
					std::uint32_t n = 0;
					for (std::uint32_t sy = sy0; sy < sy1 && sy < a_srcSide; ++sy) {
						for (std::uint32_t sx = sx0; sx < sx1 && sx < a_srcSide; ++sx) {
							const auto* p = a_src.data() + (static_cast<std::size_t>(sy) * a_srcSide + sx) * 4;
							acc[0] += p[0];
							acc[1] += p[1];
							acc[2] += p[2];
							++n;
						}
					}
					auto* d = out.data() + (static_cast<std::size_t>(oy) * a_size + ox) * 4;
					if (n) {
						d[0] = static_cast<std::uint8_t>(acc[0] / n);
						d[1] = static_cast<std::uint8_t>(acc[1] / n);
						d[2] = static_cast<std::uint8_t>(acc[2] / n);
					}
					d[3] = 255;
				}
			}
			return out;
		}

		// RGBA8 to PNG with WIC, to a temporary name first, then renamed, so a
		// reader never sees a half-written file. The target name is new for
		// every capture: replacing a file a page has loaded fails (measured,
		// MoveFileEx error 5).
		std::string WritePng(const std::filesystem::path& a_path, const std::vector<std::uint8_t>& a_rgba, std::uint32_t a_w, std::uint32_t a_h)
		{
			std::error_code ec;
			std::filesystem::create_directories(a_path.parent_path(), ec);
			const auto tmp = std::filesystem::path(a_path).concat(L".tmp");

			std::vector<std::uint8_t> bgra(a_rgba.size());
			for (std::size_t i = 0; i + 3 < a_rgba.size(); i += 4) {
				bgra[i + 0] = a_rgba[i + 2];
				bgra[i + 1] = a_rgba[i + 1];
				bgra[i + 2] = a_rgba[i + 0];
				bgra[i + 3] = a_rgba[i + 3];
			}

			IWICImagingFactory*    factory = nullptr;
			IWICStream*            stream = nullptr;
			IWICBitmapEncoder*     encoder = nullptr;
			IWICBitmapFrameEncode* frame = nullptr;
			IPropertyBag2*         props = nullptr;
			std::string            err;
			auto step = [&](HRESULT a_hr, const char* a_what) {
				if (FAILED(a_hr) && err.empty()) {
					err = std::format("{} hr=0x{:08X}", a_what, static_cast<std::uint32_t>(a_hr));
				}
				return err.empty();
			};

			if (step(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "CoCreateInstance") &&
				step(factory->CreateStream(&stream), "CreateStream") &&
				step(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE), "InitializeFromFilename") &&
				step(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "CreateEncoder") &&
				step(encoder->Initialize(stream, WICBitmapEncoderNoCache), "Initialize") &&
				step(encoder->CreateNewFrame(&frame, &props), "CreateNewFrame") &&
				step(frame->Initialize(props), "Frame.Initialize") &&
				step(frame->SetSize(a_w, a_h), "SetSize")) {
				WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
				if (step(frame->SetPixelFormat(&pf), "SetPixelFormat")) {
					if (pf != GUID_WICPixelFormat32bppBGRA) {
						err = "encoder refused 32bppBGRA";
					}
				}
				if (err.empty() &&
					step(frame->WritePixels(a_h, a_w * 4, static_cast<UINT>(bgra.size()), bgra.data()), "WritePixels") &&
					step(frame->Commit(), "Frame.Commit")) {
					step(encoder->Commit(), "Encoder.Commit");
				}
			}
			if (props) props->Release();
			if (frame) frame->Release();
			if (encoder) encoder->Release();
			if (stream) stream->Release();
			if (factory) factory->Release();

			if (!err.empty()) {
				std::filesystem::remove(tmp, ec);
				return err;
			}
			if (!MoveFileExW(tmp.c_str(), a_path.c_str(), 0)) {
				const auto e = GetLastError();
				std::filesystem::remove(tmp, ec);
				return std::format("MoveFileEx error {}", e);
			}
			return {};
		}

		std::filesystem::path DataDir()
		{
			wchar_t buf[MAX_PATH * 2]{};
			const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
			return std::filesystem::path(std::wstring(buf, n)).parent_path() / L"Data";
		}

		// The folder is relative to both view roots: segments of letters,
		// digits, '_' and '-', separated by '/' or '\'. Returns the segments
		// joined by '/', or "" when the folder is not usable.
		std::string NormalizeFolder(std::string_view a_folder)
		{
			if (a_folder.empty() || a_folder.size() > kMaxFolder) {
				return {};
			}
			std::string out;
			bool segmentStart = true;
			for (char c : a_folder) {
				if (c == '/' || c == '\\') {
					if (segmentStart) {
						return {};  // empty segment, leading separator
					}
					out += '/';
					segmentStart = true;
					continue;
				}
				const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
				if (!ok) {
					return {};
				}
				out += c;
				segmentStart = false;
			}
			if (segmentStart) {
				return {};  // trailing separator
			}
			return out;
		}

		std::array<std::filesystem::path, 2> Roots(const std::string& a_folder)
		{
			const auto data = DataDir();
			const auto rel = std::filesystem::path(a_folder).make_preferred();
			return { data / L"PrismaUI" / L"views" / rel, data / L"Magelight" / rel };
		}

		// Files of earlier captures, per folder. A name stays listed until
		// both roots let it go; every capture to the folder tries again.
		struct FolderState
		{
			bool                     swept = false;
			std::int32_t             next = 1;
			std::vector<std::string> old;  // file names still to delete
		};
		std::mutex                          g_folderLock;
		std::map<std::string, FolderState>  g_folders;

		bool IsOurs(const std::string& a_name)
		{
			const std::string_view n(a_name);
			const std::string_view p(kPrefix);
			return n.size() > p.size() && n.substr(0, p.size()) == p && (n.ends_with(".png") || n.ends_with(".png.tmp"));
		}

		// First capture to a folder in this session: what earlier sessions
		// left there joins the delete list.
		void Sweep(const std::string& a_folder, FolderState& a_state)
		{
			a_state.swept = true;
			for (const auto& root : Roots(a_folder)) {
				std::error_code ec;
				for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
					const std::string name = it->path().filename().string();
					if (IsOurs(name) && std::find(a_state.old.begin(), a_state.old.end(), name) == a_state.old.end()) {
						a_state.old.push_back(name);
					}
				}
			}
		}

		// A name no root has.
		std::string NewName(const std::string& a_folder, FolderState& a_state)
		{
			const auto roots = Roots(a_folder);
			for (;;) {
				std::string name = std::format("{}{}.png", kPrefix, a_state.next++);
				std::error_code e0, e1;
				if (!std::filesystem::exists(roots[0] / name, e0) && !std::filesystem::exists(roots[1] / name, e1)) {
					return name;
				}
			}
		}

		// Deletes every listed name it can; returns how many were deleted and
		// leaves the rest listed.
		std::int32_t DeleteOld(const std::string& a_folder, FolderState& a_state, std::string& a_kept)
		{
			std::int32_t deleted = 0;
			const auto roots = Roots(a_folder);
			std::vector<std::string> keep;
			for (const auto& name : a_state.old) {
				bool gone = true;
				for (const auto& root : roots) {
					std::error_code ec;
					std::filesystem::remove(root / name, ec);
					if (ec) {
						gone = false;
					}
				}
				if (gone) {
					++deleted;
				} else {
					keep.push_back(name);
					a_kept += (a_kept.empty() ? "" : "|") + name;
				}
			}
			a_state.old = std::move(keep);
			return deleted;
		}

		// ---------------------------------------------------------------
		// the worker
		// ---------------------------------------------------------------

		// The head part of the result lines.
		std::string HeadText(const Job& a_job)
		{
			return std::format("settleFrames={} settleMs={:.2f} settle={} head=(take {:.1f} pose {:.1f} copy {:.1f} yaw {:.1f} pitch {:.1f} axis {:.1f}) track=[{}]",
				a_job.settleFrames, a_job.settleMs, a_job.settle, a_job.headTake, a_job.headArm, a_job.headCopy, a_job.yawCopy, a_job.pitchCopy,
				a_job.axisDeg, a_job.track);
		}

		void LogRefusal(const Job& a_job)
		{
			spdlog::warn("Portrait: #{} FAILED folder={} size={} totalMs={:.2f} armFrames={} poseFrames={} {} why=\"{}\" - the script gets \"\".",
				a_job.seq, a_job.folder, a_job.size, MsSince(a_job.t0), a_job.armFrames, a_job.poseFrames, HeadText(a_job), a_job.why);
		}

		void Finish(std::shared_ptr<Job> a_job)
		{
			std::string file;
			std::string write = "not written";
			std::int32_t deleted = 0;
			std::string kept;
			if (a_job->ok) {
				const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				try {
					const auto s = a_job->boxS;
					std::vector<std::uint8_t> rgba(static_cast<std::size_t>(s) * s * 4);
					bool converted = true;
					for (std::uint32_t y = 0; y < s && converted; ++y) {
						converted = ConvertRow(static_cast<DXGI_FORMAT>(a_job->format), a_job->raw.data() + static_cast<std::size_t>(y) * a_job->rowBytes,
							rgba.data() + static_cast<std::size_t>(y) * s * 4, s);
					}
					std::vector<std::uint8_t>().swap(a_job->raw);
					if (!converted) {
						a_job->ok = false;
						a_job->why = std::format("format {} not read", a_job->format);
					} else {
						const auto size = static_cast<std::uint32_t>(a_job->size);
						const auto px = Scale(rgba, s, size);
						std::scoped_lock l(g_folderLock);
						auto& state = g_folders[a_job->folder];
						if (!state.swept) {
							Sweep(a_job->folder, state);
						}
						const std::string name = NewName(a_job->folder, state);
						const auto roots = Roots(a_job->folder);
						std::string err = WritePng(roots[0] / name, px, size, size);
						if (err.empty()) {
							err = WritePng(roots[1] / name, px, size, size);
							if (!err.empty()) {
								err = "magelight root: " + err;
							}
						} else {
							err = "prisma root: " + err;
						}
						if (err.empty()) {
							write = "written";
							deleted = DeleteOld(a_job->folder, state, kept);
							state.old.push_back(name);  // deleted by the next capture to this folder
							file = name;
						} else {
							std::error_code ec;
							std::filesystem::remove(roots[0] / name, ec);
							std::filesystem::remove(roots[1] / name, ec);
							a_job->ok = false;
							a_job->why = err;
							write = "FAILED " + err;
						}
					}
				} catch (const std::exception& e) {
					a_job->ok = false;
					a_job->why = std::format("exception writing: {}", e.what());
				} catch (...) {
					a_job->ok = false;
					a_job->why = "exception writing";
				}
				if (SUCCEEDED(co)) {
					CoUninitialize();
				}
			}
			if (a_job->ok) {
				spdlog::info("Portrait: #{} ok folder={} file={} size={} totalMs={:.2f} box=({},{},{}) tex={}x{} fmt={} poseFrames={} armFrames={} shadowToPose={:.2f} camToPose={:.2f} rootToPose={:.2f} crop={} port=({}) matrixCalls={} mapTries={} hookMaxMs={:.2f} helmet=[{}] slots=[{}] {} write={} deleted={} kept=[{}]{}{}",
					a_job->seq, a_job->folder, file, a_job->size, MsSince(a_job->t0), a_job->boxX, a_job->boxY, a_job->boxS, a_job->texW, a_job->texH,
					a_job->format, a_job->poseFrames, a_job->armFrames, a_job->shadowToPose, a_job->camToPose, a_job->rootToPose, a_job->crop, a_job->port,
					a_job->matrixCalls, a_job->mapTries, a_job->hookMaxMs, a_job->helmet, a_job->slots, HeadText(*a_job), write, deleted, kept,
					a_job->diag.empty() ? "" : " ", a_job->diag);
			} else {
				LogRefusal(*a_job);
			}
			Answer(a_job->seq, file);
			g_busy.store(false, std::memory_order_release);
		}

		// ---------------------------------------------------------------
		// the frame hook
		// ---------------------------------------------------------------

		void EndActive()
		{
			ResumeTracking();
			if (g_staging) {
				g_staging->Release();
				g_staging = nullptr;
			}
			auto job = std::move(g_active);
			g_active.reset();
			g_phase = Phase::kIdle;
			if (job) {
				std::thread([job]() { Finish(job); }).detach();
			}
		}

		void FailActive(std::string a_why, bool a_device = false)
		{
			if (!g_active) {
				return;
			}
			if (a_device) {
				if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
					if (auto* device = reinterpret_cast<ID3D11Device*>(renderer->GetRuntimeData().forwarder)) {
						a_why += std::format(" deviceRemovedReason=0x{:08X}", static_cast<std::uint32_t>(device->GetDeviceRemovedReason()));
					}
				}
			}
			g_active->ok = false;
			g_active->why = std::move(a_why);
			EndActive();
		}

		// The refusals, at the take and again at the pose. "" = go on.
		std::string Refusal(RE::PlayerCharacter*& a_player, RE::NiAVObject*& a_head, RE::NiAVObject*& a_root)
		{
			auto* ui = RE::UI::GetSingleton();
			if (ui && ui->GameIsPaused()) {
				return "a menu pauses the game";
			}
			auto* cam = RE::PlayerCamera::GetSingleton();
			a_player = RE::PlayerCharacter::GetSingleton();
			if (!cam || !a_player || !cam->cameraRoot || !cam->IsInThirdPerson() ||
				!cam->IsCurrentState(RE::CameraStates::kThirdPerson, RE::CameraStates::kVRThirdPerson)) {
				return "camera not in the plain third person state";
			}
			a_head = HeadNode(a_player, &a_root);
			if (!a_head) {
				return "no head node in the third person 3D";
			}
			return {};
		}

		// Arms the pose in front of the head and culls the helmet. "" = armed.
		std::string ArmPose(Job& a_job)
		{
			RE::PlayerCharacter* player = nullptr;
			RE::NiAVObject*      head = nullptr;
			RE::NiAVObject*      root = nullptr;
			if (auto why = Refusal(player, head, root); !why.empty()) {
				return a_job.settleFrames > 0 ? why + std::format(" (after {} frames waiting for the head)", a_job.settleFrames) : why;
			}
			TrackingOff(player);
			const RE::NiPoint3 h = head->world.translate;
			g_frontPos = FrontOf(h, player->GetAngleZ());
			g_frontRot = LookAtMatrix(g_frontPos, h);
			g_frontQuat = QuatFromMatrix(g_frontRot);
			a_job.headArm = AimAt(head, nullptr, g_frontPos).deg;
			a_job.settleMs = MsSince(a_job.takeAt);
			a_job.helmet = HideHelmet(player);
			g_writes.store(0, std::memory_order_relaxed);
			g_matrixCalls.store(0, std::memory_order_relaxed);
			g_lastWrites = 0;
			g_armed.store(true, std::memory_order_release);
			g_phase = Phase::kArmed;
			return {};
		}

		// The take: refuse, or switch head tracking off and arm the pose at
		// once when the face is already in front; else wait for it. "" = on.
		std::string Take(Job& a_job)
		{
			RE::PlayerCharacter* player = nullptr;
			RE::NiAVObject*      head = nullptr;
			RE::NiAVObject*      root = nullptr;
			if (auto why = Refusal(player, head, root); !why.empty()) {
				return why;
			}
			try {
				a_job.slots = SlotOwners();
			} catch (...) {
				a_job.slots = "not read";
			}
			a_job.takeAt = Clock::now();
			a_job.track = SuspendTracking(player);
			const auto aim = AimAt(head, root, FrontOf(head->world.translate, player->GetAngleZ()));
			a_job.headTake = aim.deg;
			a_job.axisDeg = aim.axis;
			if (aim.deg >= 0.0f && aim.deg <= kHeadReady) {
				return ArmPose(a_job);
			}
			g_prevHead = aim.deg;
			g_stillFrames = 0;
			g_phase = Phase::kSettling;
			return {};
		}

		// One swap of the wait for the head. "" = still waiting or armed.
		std::string Settle(Job& a_job)
		{
			++a_job.armFrames;
			++a_job.settleFrames;
			RE::PlayerCharacter* player = nullptr;
			RE::NiAVObject*      head = nullptr;
			RE::NiAVObject*      root = nullptr;
			if (auto why = Refusal(player, head, root); !why.empty()) {
				return why + std::format(" (after {} frames waiting for the head)", a_job.settleFrames);
			}
			TrackingOff(player);
			const float deg = AimAt(head, nullptr, FrontOf(head->world.translate, player->GetAngleZ())).deg;
			g_stillFrames = std::fabs(deg - g_prevHead) < kHeadStill ? g_stillFrames + 1 : 0;
			g_prevHead = deg;
			if (deg >= 0.0f && deg <= kHeadReady) {
				a_job.settle = "front";
			} else if (a_job.settleFrames >= kMinSettleFrames && g_stillFrames >= kStillFrames) {
				a_job.settle = "still";
			} else if (a_job.settleFrames >= kMaxSettleFrames) {
				a_job.settle = "cap";
			} else {
				return {};
			}
			return ArmPose(a_job);
		}

		// The swap after the third camera update with the pose: the square
		// around the head from kMAIN to a staging texture. No Map here.
		void StartCopy()
		{
			auto& job = *g_active;
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* root = player ? player->Get3D(false) : nullptr;
			auto* head = root ? root->GetObjectByName(RE::BSFixedString("NPC Head [Head]")) : nullptr;
			auto* camera = RE::Main::WorldRootCamera();
			if (!head || !camera) {
				FailActive("no head node or scene camera at the copy");
				return;
			}
			const RE::NiPoint3 p = head->world.translate;

			// The head guard: the frame is the pose's, which follows the body;
			// a face turned away from it is a profile on either crop path.
			const auto aim = AimAt(head, nullptr, g_frontPos);
			job.headCopy = aim.deg;
			job.yawCopy = aim.yaw;
			job.pitchCopy = aim.pitch;
			if (aim.deg < 0.0f || aim.deg > kHeadLimit) {
				FailActive(std::format("head turned {:.1f} deg from the camera (yaw {:.1f} pitch {:.1f}, limit {:.0f}) with head tracking off, after {} frames waiting for it (settle={}, take {:.1f} deg, pose {:.1f} deg) track=[{}] - not a front portrait",
					aim.deg, aim.yaw, aim.pitch, kHeadLimit, job.settleFrames, job.settle, job.headTake, job.headArm, job.track));
				return;
			}

			job.camToPose = Dist3(camera->world.translate, g_frontPos);
			if (auto* pc = RE::PlayerCamera::GetSingleton(); pc && pc->cameraRoot) {
				job.rootToPose = Dist3(pc->cameraRoot->world.translate, g_frontPos);
			}
			float hx = 0, hy = 0, hz = 0, xa = 0, ya = 0, za = 0, xb = 0, yb = 0, zb = 0;
			const bool front = camera->WorldPtToScreenPt3(p, hx, hy, hz, 1e-5f);
			const bool a = camera->WorldPtToScreenPt3(RE::NiPoint3(p.x, p.y, p.z + kHeadSpan / 2.0f), xa, ya, za, 1e-5f);
			const bool b = camera->WorldPtToScreenPt3(RE::NiPoint3(p.x, p.y, p.z - kHeadSpan / 2.0f), xb, yb, zb, 1e-5f);
			const bool matrixOk = front && hz > 0.0f && hx >= 0.0f && hx <= 1.0f && hy >= 0.0f && hy <= 1.0f && a && b;
			job.port = std::format("{:.3f},{:.3f},{:.3f}{}", hx, hy, hz, front ? "" : " behind");

			// Where the head is in the frame. When the renderer's eye is at the
			// pose, the frame was drawn from the pose, which looks at the head:
			// the head is in the centre by construction, and its span follows
			// from the distance and the frustum. The scene camera's worldToCam
			// is used only when the eye is not at the pose: on a setup with
			// another render pipeline it was measured to place the head about
			// 7 units off the camera while the pose was 75 away.
			const bool poseOk = job.shadowToPose >= 0.0f && job.shadowToPose <= kPoseTol;
			float px = 0.5f, py = 0.5f, spanY = 1.0f;
			if (poseOk) {
				const auto& fr = camera->GetRuntimeData2().viewFrustum;
				const float ext = std::fabs(fr.fTop - fr.fBottom);  // view plane height at unit distance
				const float d = Dist3(g_frontPos, p);
				if (!fr.bOrtho && ext > 0.1f && ext < 5.0f && d > 1.0f) {
					spanY = kHeadSpan / (d * ext);
				}
				job.crop = "pose";
			} else if (matrixOk) {
				px = hx;
				py = hy;
				spanY = std::fabs(ya - yb);
				job.crop = "matrix";
			} else {
				FailActive(std::format("head not on screen at the copy (port {}) and the renderer camera is not at the pose (shadowToPose={:.2f} camToPose={:.2f} rootToPose={:.2f}) slots=[{}] {}",
					job.port, job.shadowToPose, job.camToPose, job.rootToPose, job.slots, CameraDiag(camera)));
				return;
			}
			if (!matrixOk) {
				job.diag = CameraDiag(camera);
			}

			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!renderer) {
				FailActive("no renderer");
				return;
			}
			auto& rd = renderer->GetRuntimeData();
			auto* device = reinterpret_cast<ID3D11Device*>(rd.forwarder);
			auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(rd.context);
			auto* tex = rd.renderTargets[RE::RENDER_TARGETS::kMAIN].texture;
			if (!device || !ctx || !tex) {
				FailActive("no device, context or kMAIN texture");
				return;
			}
			D3D11_TEXTURE2D_DESC desc{};
			tex->GetDesc(&desc);
			job.format = static_cast<std::uint32_t>(desc.Format);
			job.texW = desc.Width;
			job.texH = desc.Height;
			const std::uint32_t bpp = BytesPerPixel(desc.Format);
			if (desc.SampleDesc.Count > 1 || bpp == 0 || desc.Width < 8 || desc.Height < 8) {
				FailActive(std::format("kMAIN format {} samples {} size {}x{} not read", job.format, desc.SampleDesc.Count, desc.Width, desc.Height));
				return;
			}
			// Port y grows upwards; the texture's rows grow downwards.
			const float w = static_cast<float>(desc.Width);
			const float h = static_cast<float>(desc.Height);
			const float cx = px * w;
			const float cy = (1.0f - py) * h;
			const float side = (std::clamp)(spanY * h * kCrop, 8.0f, (std::min)(w, h));
			const auto s = static_cast<std::uint32_t>(side);
			job.boxX = static_cast<std::uint32_t>((std::clamp)(cx - side / 2.0f, 0.0f, w - static_cast<float>(s)));
			job.boxY = static_cast<std::uint32_t>((std::clamp)(cy - side / 2.0f, 0.0f, h - static_cast<float>(s)));
			job.boxS = s;
			job.rowBytes = s * bpp;

			D3D11_TEXTURE2D_DESC sd{};
			sd.Width = s;
			sd.Height = s;
			sd.MipLevels = 1;
			sd.ArraySize = 1;
			sd.Format = desc.Format;
			sd.SampleDesc.Count = 1;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			const HRESULT hr = device->CreateTexture2D(&sd, nullptr, &g_staging);
			if (FAILED(hr) || !g_staging) {
				g_staging = nullptr;
				FailActive(std::format("CreateTexture2D hr=0x{:08X}", static_cast<std::uint32_t>(hr)), true);
				return;
			}
			const D3D11_BOX box{ job.boxX, job.boxY, 0, job.boxX + s, job.boxY + s, 1 };
			ctx->CopySubresourceRegion(g_staging, 0, 0, 0, 0, tex, 0, &box);
			g_phase = Phase::kMapping;
		}

		// One Map attempt per frame that never waits.
		void TryMap()
		{
			auto& job = *g_active;
			++job.mapTries;
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			auto* ctx = renderer ? reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context) : nullptr;
			if (!ctx || !g_staging) {
				FailActive("no context or staging at Map");
				return;
			}
			D3D11_MAPPED_SUBRESOURCE m{};
			const HRESULT hr = ctx->Map(g_staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
			if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
				if (job.mapTries >= kMaxMapFrames) {
					FailActive(std::format("Map still drawing after {} frames", job.mapTries));
				}
				return;
			}
			if (FAILED(hr) || !m.pData) {
				FailActive(std::format("Map hr=0x{:08X} on try {}", static_cast<std::uint32_t>(hr), job.mapTries), true);
				return;
			}
			job.raw.resize(static_cast<std::size_t>(job.rowBytes) * job.boxS);
			for (std::uint32_t y = 0; y < job.boxS; ++y) {
				std::memcpy(job.raw.data() + static_cast<std::size_t>(y) * job.rowBytes,
					static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch, job.rowBytes);
			}
			ctx->Unmap(g_staging, 0);
			job.ok = true;
			EndActive();
		}

		// Mid hook at the entry of Main::Swap. SafetyHook saves and restores
		// every register around it; nothing here changes them.
		void OnSwap(safetyhook::Context&)
		{
			if (!g_active && !g_hasRequest.load(std::memory_order_acquire)) {
				return;
			}
			const auto t0 = Clock::now();
			try {
				if (!g_active) {
					{
						std::scoped_lock l(g_reqLock);
						g_active = std::move(g_request);
						g_request.reset();
						g_hasRequest.store(false, std::memory_order_release);
					}
					if (g_active) {
						if (auto why = Take(*g_active); !why.empty()) {
							Disarm();
							FailActive(std::move(why));
						}
					}
				} else if (g_phase == Phase::kSettling) {
					if (auto why = Settle(*g_active); !why.empty()) {
						Disarm();
						FailActive(std::move(why));
					}
				} else if (g_phase == Phase::kArmed) {
					auto& job = *g_active;
					++job.armFrames;
					if (auto* player = RE::PlayerCharacter::GetSingleton()) {
						TrackingOff(player);
					}
					const std::int32_t writes = g_writes.load(std::memory_order_relaxed);
					if (writes > g_lastWrites) {
						++job.poseFrames;  // this frame's camera update carried the pose
					}
					g_lastWrites = writes;
					if (job.poseFrames >= kHoldFrames) {
						RE::NiPoint3 s;
						if (ShadowEye(s)) {
							job.shadowToPose = Dist3(s, g_frontPos);
						}
						job.matrixCalls = g_matrixCalls.load(std::memory_order_relaxed);
						// The copy reads this frame's kMAIN; disarming only changes
						// what the next frame draws.
						StartCopy();
						Disarm();
					} else if (job.armFrames - job.settleFrames >= kHoldFrames + kMaxWaitFrames) {
						Disarm();
						FailActive(std::format("{} of {} camera updates in {} frames", job.poseFrames, kHoldFrames, job.armFrames - job.settleFrames));
					}
				} else {
					TryMap();
				}
			} catch (...) {
				Disarm();
				FailActive("exception in the frame hook");
			}
			if (g_active) {
				g_active->hookMaxMs = (std::max)(g_active->hookMaxMs, MsSince(t0));
			}
		}

		// ---------------------------------------------------------------
		// the native
		// ---------------------------------------------------------------

		// Every answer, the immediate refusals included, goes back through the
		// game thread and the callback always returns kStarted: the rules of
		// the latent natives in MenuPrompt.cpp, for the same reasons.
		RE::BSScript::LatentStatus CapturePlayerPortrait(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
			RE::StaticFunctionTag*, RE::BSFixedString a_folder, std::int32_t a_size)
		{
			std::string refused;
			bool claimed = false;
			try {
				const std::string raw = a_folder.c_str() ? a_folder.c_str() : "";
				const std::string folder = NormalizeFolder(raw);
				if (folder.empty() || a_size < kMinSize || a_size > kMaxSize) {
					refused = std::format("folder \"{}\" or size {} not accepted (folder: letters, digits, '_', '-', '/'; size {}-{})", raw,
						a_size, kMinSize, kMaxSize);
				} else if (!g_ready.load(std::memory_order_acquire)) {
					refused = "not installed this session (see the Portrait lines at load)";
				} else if (g_busy.exchange(true, std::memory_order_acq_rel)) {
					refused = "a capture is still in flight";
				} else {
					claimed = true;
					auto job = std::make_shared<Job>();
					job->seq = ++g_seq;
					job->folder = folder;
					job->size = a_size;
					job->t0 = Clock::now();
					{
						std::scoped_lock l(g_waitLock);
						g_waiter = Waiter{ job->seq, a_vm, a_stackID };
					}
					{
						std::scoped_lock l(g_reqLock);
						g_request = std::move(job);
					}
					g_hasRequest.store(true, std::memory_order_release);
					return RE::BSScript::LatentStatus::kStarted;
				}
			} catch (...) {
				if (claimed) {
					g_busy.store(false, std::memory_order_release);
				}
				refused = "exception filing the request";
			}
			spdlog::warn("Portrait: refused: {} - the script gets \"\".", refused);
			DispatchToGame([a_vm, a_stackID]() { ReturnToScript(a_vm, a_stackID, std::string()); });
			return RE::BSScript::LatentStatus::kStarted;
		}

		// The library's RegisterLatentFunction does not compile (see the same
		// class in MenuPrompt.cpp). This one returns a String to the script.
		template <class... Args>
		class LatentStringNative final :
			public RE::BSScript::NativeFunction<
				true,
				RE::BSScript::LatentStatus(RE::BSScript::Internal::VirtualMachine*, RE::VMStackID, RE::StaticFunctionTag*, Args...),
				std::underlying_type_t<RE::BSScript::LatentStatus>,
				RE::StaticFunctionTag*,
				Args...>
		{
			using super = RE::BSScript::NativeFunction<
				true,
				RE::BSScript::LatentStatus(RE::BSScript::Internal::VirtualMachine*, RE::VMStackID, RE::StaticFunctionTag*, Args...),
				std::underlying_type_t<RE::BSScript::LatentStatus>,
				RE::StaticFunctionTag*,
				Args...>;

		public:
			LatentStringNative(std::string_view a_fnName, std::string_view a_className, typename super::function_type* a_callback) :
				super(a_fnName, a_className, a_callback)
			{
				// Must match the type handed to ReturnLatentResult: a string.
				this->_retType = RE::BSScript::GetRawType<RE::BSFixedString>{}();
				this->_isLatent = true;
			}

			~LatentStringNative() override = default;
		};
	}

	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			return false;
		}
		a_vm->BindNativeMethod(new LatentStringNative<RE::BSFixedString, std::int32_t>("CapturePlayerPortrait", "Lodestone", CapturePlayerPortrait));
		spdlog::info("Portrait: native registered (CapturePlayerPortrait, latent - the caller waits a few frames).");
		return true;
	}

	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg || (a_msg->type != SKSE::MessagingInterface::kPreLoadGame && a_msg->type != SKSE::MessagingInterface::kNewGame)) {
			return;
		}
		std::optional<Waiter> w;
		{
			std::scoped_lock l(g_waitLock);
			w = g_waiter;
			g_waiter.reset();
		}
		if (w) {
			spdlog::info("Portrait: #{} answered \"\" at once - {}.", w->seq,
				a_msg->type == SKSE::MessagingInterface::kPreLoadGame ? "a save is being loaded" : "a new game is starting");
			ReturnToScript(w->vm, w->stack, std::string());
		}
	}

	void Install()
	{
		if (REL::Module::IsVR()) {
			spdlog::info("Portrait: NOT installed - VR runtime; the addresses are only known for SE and AE. CapturePlayerPortrait answers \"\".");
			return;
		}
		try {
			// Main::Swap, the engine's frame swap. The id is not in the pinned
			// CommonLib; it is the one Photo Mode (powerof3, GPL-3.0) and SKSE
			// Menu Framework hook call sites inside. A mid hook at the entry
			// needs no signature: SafetyHook saves and restores every register.
			REL::Relocation<std::uintptr_t> target{ REL::RelocationID(35556, 36555) };
			const auto* b = reinterpret_cast<const std::uint8_t*>(target.address());
			const std::string before = std::format("{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);

			// The worldToCam rebuild (SE 69271 / AE 70641, SmoothCam's offset
			// table). Without it the capture is refused: the pose alone gave an
			// off centre crop.
			REL::Relocation<std::uintptr_t> toScreen{ REL::RelocationID(69271, 70641) };
			const auto* t = reinterpret_cast<const std::uint8_t*>(toScreen.address());
			const std::string rebuild = std::format("{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}", t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
			g_toScreen = reinterpret_cast<ToScreenFn>(toScreen.address());

			g_swapHook = safetyhook::create_mid(reinterpret_cast<void*>(target.address()), OnSwap);
			if (!g_swapHook) {
				spdlog::error("Portrait: SafetyHook refused the frame hook at 0x{:X} (entry bytes {}); CapturePlayerPortrait answers \"\" this session.",
					target.address(), before);
				return;
			}

			// The plain third person state's Update (03), GetRotation (04) and
			// GetTranslation (05), SE/AE slots of TESCameraState.h. Each calls
			// the original first and changes nothing unless a capture is armed.
			REL::Relocation<std::uintptr_t> third{ RE::VTABLE_ThirdPersonState[0] };
			StateUpdateHook::func = third.write_vfunc(0x03, StateUpdateHook::thunk);
			StateRotationHook::func = third.write_vfunc(0x04, StateRotationHook::thunk);
			StateTranslationHook::func = third.write_vfunc(0x05, StateTranslationHook::thunk);

			g_ready.store(true, std::memory_order_release);
			spdlog::info("Portrait: installed ({} runtime): frame hook at the entry of Main::Swap 0x{:X} (entry bytes before {}), worldToCam rebuild at 0x{:X} (entry bytes {}), ThirdPersonState Update/GetRotation/GetTranslation. Idle until a capture is asked.",
				REL::Module::IsAE() ? "AE" : "SE", target.address(), before, toScreen.address(), rebuild);
		} catch (const std::exception& e) {
			spdlog::error("Portrait: NOT installed - {}. CapturePlayerPortrait answers \"\" this session.", e.what());
		} catch (...) {
			spdlog::error("Portrait: NOT installed (unknown exception). CapturePlayerPortrait answers \"\" this session.");
		}
	}
}
