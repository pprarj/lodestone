// Portrait.cpp
// Lodestone - Shared SKSE framework
//
// Lodestone.CapturePlayerPortrait(String asFolder, Int aiSize) -> String
// Lodestone.CapturePlayerPortraitCutout(String asFolder, Int aiSize, String asKey, Bool abTrimBackground) -> String
// Lodestone.GetPlayerPortrait(String asFolder, String asKey) -> String
// Lodestone.GetPlayerPortraitFailure() -> String
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
//   arm               a pose 75 units in front of the head, looking at it,
//                     along the body's heading, or along the face's when the
//                     head is turned more than 15 degrees from the body's front,
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
// THE CAMERA AT THE POSE. A frame counts as a pose frame only when, at its
// swap, the scene camera is where the pose put it (within 1 unit); the copy is
// the third such frame in a row, and a capture whose camera never gets there
// fails with code "camera" instead of returning the game camera's frame.
// SmoothCam installs its own Update on the camera states at the first load or
// new game, after this plugin's, so it runs around ours and moves the camera
// after the pose was written. When a frame shows the camera moved off the
// pose and SmoothCam's modder API is there, the camera is asked from it for
// the rest of the capture (while handed over, its update writes nothing) and
// given back at the disarm. Without SmoothCam nothing is asked.
//
// THE REASON. GetPlayerPortraitFailure returns a short code for the last
// capture that answered "": busy, menu, firstperson, camera, face, faded, load
// or other.
//
// THE HEAD. A head turned by head tracking shows in profile from a pose on
// the body's heading, and with head tracking switched off it was measured to
// come back too slowly for a key press (a third to less than half of the way
// to 30 degrees in 30 frames). So when the face is more than 15 degrees off
// the body's front at the take, the pose goes in front of the face instead:
// the heading of the face direction in the horizontal plane, the camera level
// as before. The torso then shows at an angle; the guard at the copy stays.
// The face direction is the head node's local +Y: in the vanilla skeleton
// (male and female) the head's bind rotation against NPC Root is the identity
// within 0.011, and the eye bone and the head magic node sit 8.1 and 11.5
// units along +Y from it.
//
// The camera state's own fields are never written, so the update after the
// copy builds the usual pose: the player sees three frames from the front.
// Nothing is unequipped, nothing is created, nothing goes into the save. With
// no request open the hook does one atomic load per frame.
//
// THE CONVERSION. kMAIN is a float target (HDR) on the measured setups; it is
// mapped with x / (1 + x) and a 1/2.2 gamma. 8 bit and 10 bit targets are
// copied as they are.
//
// THE CUTOUT (CapturePlayerPortraitCutout). The same sequence, with four
// differences:
//
//   framing   the camera goes level 3.2 units above the head node, at
//             33 / |fTop - fBottom| units from that point, so the frame shows
//             33 units of height whatever the FOV; the crop is the square of
//             that span around the point
//   hair      when an armour fills the hair or long hair slot, the hair's
//             dismember partitions under the face node are switched on for
//             the pose frames and off again at the disarm; the helmet's own
//             geometry is culled as above. Nothing is unequipped
//   light     a point light of its own, above and beside the camera, for the
//             pose frames only; removed at the disarm and at a load. It is a
//             scene object, not a form: nothing of it goes into the save
//   depth     the depth target of kMAIN is copied whole with the color box
//             (a depth resource is only copied whole) to staging textures
//             made in the armed frames before the copy; both boxes are read
//             back a slice per frame, so no frame pays for the whole read
//
// The worker keeps what is up to 45 units past the head node and, unless the
// caller asks for that reach alone, of it only what is joined to the face with
// no depth step over 5 units between neighbour pixels, so a wall or someone
// beside that only meets the player across a larger step is dropped. Alpha is
// 0 for everything else.
//
// THE PLAYER'S FADE. The game fades the player out when its own third person
// camera is pushed into the body, as with a wall close behind. The pose frames
// put the player's 3D back to opaque after each camera update, and the copy
// fails when the frame was still drawn with the player faded: the color would
// show what is behind the player where the depth has the player.
//
// FILE NAMES. lodestone-portrait-<n>.png, or lodestone-portrait-<key>-<n>.png
// with a key. A capture deletes only the earlier files of its own key, and
// GetPlayerPortrait returns the newest file of a key.

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
#include <cstring>
#include <filesystem>
#include <fstream>
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
		constexpr float        kHeadReady = 15.0f;   // degrees face to the body's pose at the take: past this the pose follows the face
		constexpr float        kHeadLimit = 30.0f;   // degrees face to camera at the copy: past this the capture fails
		constexpr float        kFaceLevelMin = 0.2f; // horizontal length of the face direction under which it has no heading
		constexpr std::int32_t kMinSize = 16;
		constexpr std::int32_t kMaxSize = 1024;
		constexpr std::size_t  kMaxFolder = 128;
		constexpr const char*  kPrefix = "lodestone-portrait-";


		// The cutout (CapturePlayerPortraitCutout).
		constexpr float         kCutSpan = 33.0f;       // visible height at the aim point, units
		constexpr float         kCutAimUp = 3.2f;       // aim point above the head node, units
		constexpr float         kFrameMinDist = 25.0f;  // the camera never closer than this to the aim point
		constexpr float         kCutReach = 45.0f;      // the cut keeps what is up to this past the head node
		constexpr float         kTrimStep = 5.0f;       // depth step between neighbour pixels that breaks the join
		constexpr float         kSeedBand = 20.0f;      // join seeds: within this of the head node's distance
		constexpr float         kConvBand = 30.0f;      // depth at the face this close to the head = the convention
		constexpr std::size_t   kMapBudget = 4u << 20;  // bytes read back per frame
		constexpr std::size_t   kMaxKey = 32;
		constexpr std::size_t   kMaxOld = 32;           // names listed per folder and key; the oldest past this is let go (the next session's sweep finds it)
		constexpr int           kRetryTries = 10;       // tries to delete a kept name after the script has its answer
		constexpr int           kRetryMs = 1500;        // between those tries
		constexpr float         kLightUp = 20.0f;       // the light: above the camera
		constexpr float         kLightSide = 15.0f;     // and to its right
		constexpr float         kLightRadius = 160.0f;
		constexpr float         kLightFade = 1.2f;
		constexpr float         kLightColor[3] = { 1.0f, 0.93f, 0.84f };
		constexpr std::uint16_t kHairSlots[] = { 31, 41 };  // hair, long hair (biped slot numbers)
		constexpr float         kFadeOpaque = 0.99f;  // a fade under this at the copy fails the capture

		struct Job
		{
			// the cutout
			bool         cutout = false;
			bool         trim = true;      // drop what is not joined to the face
			std::string  key;              // sanitized; "" = no key
			// framing: span 0 = the fixed pose of CapturePlayerPortrait
			float        span = 0.0f;      // visible height at the aim point, units
			float        aimUp = 0.0f;     // aim point above the head node, units
			float        extTake = -1.0f;  // |fTop - fBottom| of the scene camera at the take
			float        extCopy = -1.0f;  // and at the copy
			float        frameDist = -1.0f;  // camera to aim point, horizontal
			bool         frameClamped = false;  // frameDist raised to kFrameMinDist
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
			std::string  frames;                // per armed swap: n:camToPose/rootToPose, "-" without a camera update, "sc" with the camera handed over by SmoothCam
			std::string  smooth = "absent";     // SmoothCam's API: absent, or what it answered to the request and the release
			bool         smoothAsked = false;
			std::int32_t offFrames = 0;         // armed swaps after a camera update that left the camera off the pose
			std::string  code = "other";        // what GetPlayerPortraitFailure returns when this capture fails
			std::string  crop;                  // what placed the crop: "pose" or "matrix"
			std::string  port;                  // the head through the scene camera's worldToCam
			std::string  diag;                  // frustum, port rect and worldToCam, when the matrix was not used
			// head
			float        heading = 0.0f;     // radians: the heading the pose stands on
			std::string  poseBy = "body";    // whose heading: "body" (GetAngleZ) or "face" (the head's +Y)
			std::string  turn;               // headings against GetAngleZ at the take: the 3D root, the face
			float        headTake = -1.0f;   // degrees face to the body's pose at the take
			float        headArm = -1.0f;    // at the pose
			float        headCopy = -1.0f;   // at the copy
			float        yawCopy = 0.0f;     // camera in the head's frame at the copy: yaw to the head's right
			float        pitchCopy = 0.0f;   // and pitch above its forward axis
			float        axisDeg = -1.0f;    // face direction vs the head magic node, at the take
			std::string  track;              // head tracking before the take and what TDM answered
			// the player's fade (D-P5-B-12): currentFade of the 3D root fade
			// node, fadeAmount of the root, the actor alpha; -1 = not read
			std::array<float, 3> fadeTake{ -1.0f, -1.0f, -1.0f };
			std::array<float, 3> fadeCopy{ -1.0f, -1.0f, -1.0f };
			std::array<float, 2> fadeMin{ -1.0f, -1.0f };  // lowest seen after a camera update, before the hold
			std::int32_t fadeHeld = 0;       // camera updates and swaps where the hold wrote
			float        camTake = -1.0f;    // the game's camera root to the head node, at the take
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
			double        mapMs = 0.0;       // the longest single read-back step (Map, rows, Unmap)
			std::vector<std::uint8_t> raw;
			// cutout: depth and the read back
			float         nearP = 0.0f;
			float         farP = 0.0f;
			bool          ortho = false;
			float         dPose = -1.0f;     // camera to head node at the copy
			std::uint32_t depthFormat = 0;
			std::uint32_t depthBpp = 0;
			std::uint32_t dbx = 0;           // the color box in the depth target's pixels
			std::uint32_t dby = 0;
			std::uint32_t dbs = 0;
			std::uint32_t colorRows = 0;     // rows read back so far
			std::uint32_t depthRows = 0;
			std::int32_t  mapSteps = 0;
			double        stageMs = 0.0;     // the longest frame spent making the staging textures
			std::vector<std::uint8_t> depthRaw;
			// cutout: hair and light
			std::string   hair;
			std::int32_t  hairReapplied = 0;
			std::string   light;
			std::int32_t  litFrames = 0;
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
			kArmed,
			kMapping,
		};
		Phase                g_phase = Phase::kIdle;
		std::shared_ptr<Job> g_active;
		ID3D11Texture2D*     g_staging = nullptr;
		std::int32_t         g_lastWrites = 0;

		// The code of the last capture that answered "" (GetPlayerPortraitFailure).
		std::mutex  g_failLock;
		std::string g_lastFail;

		void SetLastFailure(std::string_view a_code)
		{
			std::scoped_lock l(g_failLock);
			g_lastFail = a_code;
		}

		// Read by the camera hooks.
		std::atomic<bool>         g_armed{ false };
		std::atomic<std::int32_t> g_writes{ 0 };
		std::atomic<std::int32_t> g_matrixCalls{ 0 };
		RE::NiPoint3              g_frontPos;
		RE::NiMatrix3             g_frontRot;
		RE::NiQuaternion          g_frontQuat{ 1, 0, 0, 0 };
		std::vector<RE::NiPointer<RE::NiAVObject>> g_hidden;

		// The player's fade while armed (camera update and swap, both on the
		// main thread).
		std::atomic<std::int32_t> g_fadeHeld{ 0 };
		float                     g_fadeMinCf = 2.0f;
		float                     g_fadeMinFa = 2.0f;

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
		// one SmoothCam (mwilsnd) calls after it moves the camera. Its
		// repository has no license file; only its ABI is declared here.
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

		// The player's fade (D-P5-B-12). The game fades the player out when
		// its own third person camera is pushed into the body (a wall close
		// behind); the pose camera is elsewhere, but the fade stays and the
		// frame shows what is behind the player while the depth keeps the
		// player. Read: currentFade of the 3D root when it is a fade node,
		// fadeAmount of the root, the actor alpha. -1 = not read.
		std::array<float, 3> FadeOf(RE::PlayerCharacter* a_player)
		{
			std::array<float, 3> f{ -1.0f, -1.0f, -1.0f };
			if (!a_player) {
				return f;
			}
			if (auto* root = a_player->Get3D(false)) {
				f[1] = root->fadeAmount;
				if (auto* fn = root->AsFadeNode()) {
					f[0] = fn->GetRuntimeData().currentFade;
				}
			}
			f[2] = a_player->GetAlpha();
			return f;
		}

		// Faded = a fade value read and under kFadeOpaque.
		bool Faded(const std::array<float, 3>& a_f)
		{
			return (a_f[0] >= 0.0f && a_f[0] < kFadeOpaque) || (a_f[1] >= 0.0f && a_f[1] < kFadeOpaque);
		}

		// Records the lowest fade the engine left, then puts the player's 3D
		// back to opaque for the frame about to be drawn. Nothing to put back
		// at the disarm: the engine writes the fade from its own camera every
		// frame. True if it wrote.
		bool HoldOpaque()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* root = player ? player->Get3D(false) : nullptr;
			if (!root) {
				return false;
			}
			bool wrote = false;
			g_fadeMinFa = (std::min)(g_fadeMinFa, root->fadeAmount);
			if (root->fadeAmount < 1.0f) {
				root->fadeAmount = 1.0f;
				wrote = true;
			}
			if (auto* fn = root->AsFadeNode()) {
				auto& rd = fn->GetRuntimeData();
				g_fadeMinCf = (std::min)(g_fadeMinCf, rd.currentFade);
				if (rd.currentFade < 1.0f) {
					rd.currentFade = 1.0f;
					wrote = true;
				}
			}
			if (wrote) {
				g_fadeHeld.fetch_add(1, std::memory_order_relaxed);
			}
			return wrote;
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
						HoldOpaque();
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
		// the cutout: hair and light
		// ---------------------------------------------------------------

		// The hair partitions the capture in flight switched on: the skin
		// instance and the partition's own slot value. Switched off again at
		// the disarm, which is the state the helmet in the slot gave them.
		struct HairPart
		{
			RE::NiPointer<RE::NiSkinInstance> skin;
			std::uint16_t                     slot = 0;
		};
		std::vector<HairPart>         g_hair;
		RE::NiPointer<RE::NiAVObject> g_hairCulled;  // the hair head part, when it was culled

		// Partition slots of head parts carry 100 or 200 over the biped slot.
		std::uint16_t BipedSlot(std::uint16_t a_raw)
		{
			if (a_raw >= 230 && a_raw <= 261) {
				return static_cast<std::uint16_t>(a_raw - 200);
			}
			if (a_raw >= 130 && a_raw <= 161) {
				return static_cast<std::uint16_t>(a_raw - 100);
			}
			return a_raw;
		}

		// The hair slots an armour fills now.
		std::vector<std::uint16_t> CoveredHairSlots(RE::PlayerCharacter* a_player)
		{
			std::vector<std::uint16_t> out;
			const auto& biped = a_player->GetBiped(false);
			if (!biped) {
				return out;
			}
			for (const auto slot : kHairSlots) {
				const auto& obj = biped->objects[slot - 30];
				if (obj.item && obj.item->GetFormType() == RE::FormType::Armor) {
					out.push_back(slot);
				}
			}
			return out;
		}

		// Switches on the hair partitions of the slots an armour fills, and
		// unculls the hair head part when it is culled. Returns what it did.
		std::string ShowHair(RE::PlayerCharacter* a_player)
		{
			g_hair.clear();
			g_hairCulled.reset();
			const auto covered = CoveredHairSlots(a_player);
			std::string cov;
			for (const auto s : covered) {
				cov += std::format("{}{}", cov.empty() ? "" : ",", s);
			}
			if (covered.empty()) {
				return "slots free";
			}
			auto* face = a_player->GetFaceNodeSkinned();
			std::int32_t geoms = 0;
			std::int32_t hairParts = 0;
			std::string  on;
			if (face) {
				RE::BSVisit::TraverseScenegraphGeometries(face, [&](RE::BSGeometry* a_geo) -> RE::BSVisit::BSVisitControl {
					++geoms;
					auto& skin = a_geo->GetGeometryRuntimeData().skinInstance;
					auto* dis = skin ? netimmerse_cast<RE::BSDismemberSkinInstance*>(skin.get()) : nullptr;
					if (!dis) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					auto& rd = dis->GetRuntimeData();
					std::vector<std::uint16_t> done;
					for (std::int32_t i = 0; rd.partitions && i < rd.numPartitions; ++i) {
						const std::uint16_t raw = rd.partitions[i].slot;
						const auto biped = BipedSlot(raw);
						if (std::find(covered.begin(), covered.end(), biped) == covered.end() ||
							std::find(done.begin(), done.end(), raw) != done.end()) {
							continue;
						}
						++hairParts;
						const bool before = rd.partitions[i].editorVisible;
						dis->UpdateDismemberPartion(raw, true);
						done.push_back(raw);
						g_hair.push_back(HairPart{ skin, raw });
						on += std::format("{}{}:{}:{}->{}", on.empty() ? "" : "|", a_geo->name.c_str() ? a_geo->name.c_str() : "", raw, before ? 1 : 0,
							rd.partitions[i].editorVisible ? 1 : 0);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
			std::string part = "none";
			if (auto* hairObj = a_player->GetHeadPartObject(RE::BGSHeadPart::HeadPartType::kHair)) {
				part = hairObj->GetAppCulled() ? "unculled" : "shown";
				if (hairObj->GetAppCulled()) {
					hairObj->SetAppCulled(false);
					g_hairCulled = RE::NiPointer<RE::NiAVObject>(hairObj);
				}
			}
			return std::format("covered={} faceNode={} geoms={} hairParts={} on=[{}] headPart={}", cov, face ? "yes" : "no", geoms, hairParts, on, part);
		}

		// Armed frames: a partition the engine switched off again goes back
		// on. Returns how many.
		std::int32_t KeepHair()
		{
			std::int32_t n = 0;
			for (auto& h : g_hair) {
				auto* dis = h.skin ? netimmerse_cast<RE::BSDismemberSkinInstance*>(h.skin.get()) : nullptr;
				if (!dis) {
					continue;
				}
				auto& rd = dis->GetRuntimeData();
				for (std::int32_t i = 0; rd.partitions && i < rd.numPartitions; ++i) {
					if (rd.partitions[i].slot == h.slot && !rd.partitions[i].editorVisible) {
						dis->UpdateDismemberPartion(h.slot, true);
						++n;
						break;
					}
				}
			}
			return n;
		}

		// At the disarm and at a load. Idempotent.
		void RestoreHair()
		{
			for (auto& h : g_hair) {
				if (auto* dis = h.skin ? netimmerse_cast<RE::BSDismemberSkinInstance*>(h.skin.get()) : nullptr) {
					dis->UpdateDismemberPartion(h.slot, false);
				}
			}
			g_hair.clear();
			if (g_hairCulled) {
				g_hairCulled->SetAppCulled(true);
				g_hairCulled.reset();
			}
		}

		RE::NiPointer<RE::NiPointLight> g_light;
		RE::NiPointer<RE::BSLight>      g_bsLight;
		RE::NiPointer<RE::NiNode>       g_lightParent;

		// The light of the cutout, hung on the player's 3D root, above and to
		// the right of the camera pose, and added to the world's shadow scene
		// node the way light placing plugins do. Returns what was made.
		std::string LightOn(RE::NiAVObject* a_root, const RE::NiPoint3& a_head)
		{
			auto* parent = a_root ? a_root->AsNode() : nullptr;
			auto* ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			if (!parent || !ssn) {
				return "NOT made: no 3D root node or shadow scene node";
			}
			auto* light = RE::NiPointLight::Create();
			if (!light) {
				return "NOT made: create failed";
			}
			g_light = RE::NiPointer<RE::NiPointLight>(light);
			const auto& r = g_frontRot.entry;  // columns: right, forward, up
			RE::NiTransform w;
			w.translate = RE::NiPoint3(g_frontPos.x + r[0][0] * kLightSide + r[0][2] * kLightUp, g_frontPos.y + r[1][0] * kLightSide + r[1][2] * kLightUp,
				g_frontPos.z + r[2][0] * kLightSide + r[2][2] * kLightUp);
			w.scale = 1.0f;
			light->local = parent->world.Invert() * w;
			auto& ld = light->GetLightRuntimeData();
			ld.ambient.red = 0.0f;
			ld.ambient.green = 0.0f;
			ld.ambient.blue = 0.0f;
			ld.diffuse.red = kLightColor[0];
			ld.diffuse.green = kLightColor[1];
			ld.diffuse.blue = kLightColor[2];
			ld.radius = RE::NiPoint3(kLightRadius, kLightRadius, kLightRadius);
			ld.fade = kLightFade;
			light->SetLightAttenuation(kLightRadius);
			parent->AttachChild(light, true);
			g_lightParent = RE::NiPointer<RE::NiNode>(parent);
			RE::NiUpdateData ud{};
			light->Update(ud);
			RE::ShadowSceneNode::LIGHT_CREATE_PARAMS p{};
			p.dynamic = true;
			p.shadowLight = false;
			p.portalStrict = false;
			p.affectLand = true;
			p.affectWater = false;
			p.neverFades = true;
			p.fov = 0.0f;
			p.falloff = 1.0f;
			p.nearDistance = 5.0f;
			p.depthBias = 0.0f;
			p.sceneGraphIndex = 0;
			p.restrictedNode = nullptr;
			p.lensFlareData = nullptr;
			g_bsLight = RE::NiPointer<RE::BSLight>(ssn->AddLight(light, p));
			return std::format("on pos=({:.1f},{:.1f},{:.1f}) toHead={:.1f} radius={:.0f} fade={:.2f} color=({:.2f},{:.2f},{:.2f}) bsLight={}", w.translate.x,
				w.translate.y, w.translate.z, Dist3(w.translate, a_head), kLightRadius, kLightFade, kLightColor[0], kLightColor[1], kLightColor[2],
				g_bsLight ? "set" : "null");
		}

		// At the disarm and at a load. Idempotent. Returns what it did.
		std::string LightOff()
		{
			if (!g_light) {
				return "off";
			}
			std::string out = "removed";
			if (g_bsLight) {
				if (auto* ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
					ssn->RemoveLight(g_bsLight);
				} else {
					out = "NOT removed from the scene: no shadow scene node";
				}
			}
			if (g_lightParent) {
				g_lightParent->DetachChild(g_light.get());
			}
			out += std::format(" parentAfter={}", g_light->parent ? "set" : "none");
			g_bsLight.reset();
			g_light.reset();
			g_lightParent.reset();
			return out;
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

		// SmoothCam (mwilsnd) writes the camera in its own Update of the camera
		// states, after the state's, and has a modder API that hands the camera
		// to a plugin: while it is handed over, that update writes nothing. The
		// interface comes by an SKSE message: a request to "SmoothCam" at
		// kPostPostLoad, the answer to a listener registered at kPostLoad. Only
		// the message layout and the order of the version 1 interface's virtual
		// functions are declared here; two of them are called. Not loaded: no
		// answer, nothing asked.
		namespace Sc
		{
			// The same values as Tdm::Result.
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
				virtual Result             RequestCameraControl(SKSE::PluginHandle) noexcept = 0;
				virtual Result             RequestCrosshairControl(SKSE::PluginHandle, bool) noexcept = 0;
				virtual Result             RequestStealthMeterControl(SKSE::PluginHandle, bool) noexcept = 0;
				virtual SKSE::PluginHandle GetCameraOwner() const noexcept = 0;
				virtual SKSE::PluginHandle GetCrosshairOwner() const noexcept = 0;
				virtual SKSE::PluginHandle GetStealthMeterOwner() const noexcept = 0;
				virtual Result             ReleaseCameraControl(SKSE::PluginHandle) noexcept = 0;
			};

			constexpr const char* kSender = "SmoothCam";

			// The request: a tagged command pointing to the interface version.
			struct Command
			{
				std::uint32_t header = 0x9007CA50;
				std::uint8_t  type = 0;  // interface request
				void*         data = nullptr;
			};
			struct Request
			{
				std::uint8_t version = 0;  // version 1
			};
			// The answer: a type and, for an interface, a container.
			struct Response
			{
				std::uint8_t type = 0;  // 0 error, 1 interface
				void*        data = nullptr;
			};
			struct Container
			{
				void*        instance = nullptr;
				std::uint8_t version = 0;
			};
			static_assert(sizeof(Command) == 16 && sizeof(Response) == 16);

			std::atomic<IV1*> g_api{ nullptr };
			bool              g_listening = false;  // SKSE message thread at load
			bool              g_owned = false;      // main thread: the camera is handed over to this plugin

			const char* Name(Result a_r)
			{
				return Tdm::Name(static_cast<Tdm::Result>(a_r));
			}

			void OnMessage(SKSE::MessagingInterface::Message* a_msg)
			{
				if (!a_msg || a_msg->type != 0 || a_msg->dataLen != sizeof(Response) || !a_msg->data) {
					return;
				}
				if (a_msg->sender && std::strcmp(a_msg->sender, kSender) != 0) {
					return;
				}
				const auto* r = static_cast<const Response*>(a_msg->data);
				if (r->type != 1 || !r->data) {
					spdlog::warn("Portrait: SmoothCam answered the interface request with an error; its camera is never asked for.");
					return;
				}
				const auto* c = static_cast<const Container*>(r->data);
				g_api.store(static_cast<IV1*>(c->instance), std::memory_order_release);
				spdlog::info("Portrait: SmoothCam interface obtained (version {}); the camera is asked from it only when it moves the camera off a pose.",
					static_cast<int>(c->version) + 1);
			}

			// kPostLoad: the listener. kPostPostLoad and kDataLoaded: the request,
			// until an interface came back.
			void Handle(std::uint32_t a_type)
			{
				auto* messaging = SKSE::GetMessagingInterface();
				if (!messaging) {
					return;
				}
				if (a_type == SKSE::MessagingInterface::kPostLoad) {
					g_listening = messaging->RegisterListener(kSender, OnMessage);
					return;
				}
				if (g_api.load(std::memory_order_acquire)) {
					return;
				}
				bool heard = false;
				if (g_listening) {
					Request req{};
					Command cmd{};
					cmd.data = &req;
					heard = messaging->Dispatch(0, &cmd, sizeof(Command), kSender);
				}
				if (a_type == SKSE::MessagingInterface::kDataLoaded && !g_api.load(std::memory_order_acquire)) {
					spdlog::info("Portrait: no SmoothCam interface (listener {}, request heard {}) - its camera is never asked for.", g_listening ? "yes" : "no",
						heard ? "yes" : "no");
				}
			}
		}

		// Main thread (the frame hook): asks SmoothCam for the camera, once per
		// capture.
		void TakeCamera(Job& a_job)
		{
			auto* api = Sc::g_api.load(std::memory_order_acquire);
			if (!api || Sc::g_owned || a_job.smoothAsked) {
				return;
			}
			a_job.smoothAsked = true;
			const auto res = api->RequestCameraControl(SKSE::GetPluginHandle());
			Sc::g_owned = res == Sc::Result::kOK;
			a_job.smooth = std::format("asked at frame {}: {}", a_job.armFrames, Sc::Name(res));
		}

		// Main thread: gives the camera back. Safe to call more than once.
		void GiveCameraBack()
		{
			if (!Sc::g_owned) {
				return;
			}
			Sc::g_owned = false;
			auto* api = Sc::g_api.load(std::memory_order_acquire);
			if (!api) {
				return;
			}
			const auto res = api->ReleaseCameraControl(SKSE::GetPluginHandle());
			if (g_active) {
				g_active->smooth += std::format(", given back: {}", Sc::Name(res));
			}
			if (res != Sc::Result::kOK) {
				spdlog::warn("Portrait: SmoothCam did not take its camera back: {}.", Sc::Name(res));
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

		// What the camera looks at: the head node, or with a framing the
		// point a_job.aimUp above it.
		RE::NiPoint3 AimPoint(const Job& a_job, const RE::NiPoint3& a_head)
		{
			return a_job.span > 0.0f ? RE::NiPoint3(a_head.x, a_head.y, a_head.z + a_job.aimUp) : a_head;
		}

		// Where the camera goes. With a framing it stands level with the aim
		// point, frameDist in front of it along the body's heading.
		RE::NiPoint3 PoseFor(const Job& a_job, const RE::NiPoint3& a_head, float a_heading)
		{
			if (a_job.span <= 0.0f) {
				return FrontOf(a_head, a_heading);
			}
			const auto a = AimPoint(a_job, a_head);
			return RE::NiPoint3(a.x + std::sin(a_heading) * a_job.frameDist, a.y + std::cos(a_heading) * a_job.frameDist, a.z);
		}

		// The view plane height at unit distance of the scene camera, or -1.
		float FrustumExt()
		{
			auto* camera = RE::Main::WorldRootCamera();
			if (!camera) {
				return -1.0f;
			}
			const auto& fr = camera->GetRuntimeData2().viewFrustum;
			const float ext = std::fabs(fr.fTop - fr.fBottom);
			return (!fr.bOrtho && ext > 0.1f && ext < 5.0f) ? ext : -1.0f;
		}

		// The framing: the distance that shows a_job.span units of height at the aim
		// point, from the frustum read now. "" = set.
		std::string SetFrameDist(Job& a_job)
		{
			if (a_job.span <= 0.0f) {
				return {};
			}
			a_job.extTake = FrustumExt();
			if (a_job.extTake <= 0.0f) {
				return "framing asked and the scene camera frustum is not readable (ortho or out of 0.1-5)";
			}
			a_job.frameDist = a_job.span / a_job.extTake;
			if (a_job.frameDist < kFrameMinDist) {
				a_job.frameDist = kFrameMinDist;
				a_job.frameClamped = true;
			}
			return {};
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

		// The heading (radians, the GetAngleZ convention: front = (sin, cos))
		// of a node's local +Y in the horizontal plane. False when that axis is
		// too close to vertical to have one.
		bool HeadingOf(const RE::NiAVObject* a_node, float& a_out)
		{
			const auto& r = a_node->world.rotate.entry;
			const float x = r[0][1], y = r[1][1];
			if (std::sqrt(x * x + y * y) < kFaceLevelMin) {
				return false;
			}
			a_out = std::atan2(x, y);
			return true;
		}

		// a_heading - a_ref in degrees, in -180..180: positive is turned to the
		// right (clockwise seen from above).
		float TurnDeg(float a_heading, float a_ref)
		{
			float d = std::fmod((a_heading - a_ref) * kRadToDeg, 360.0f);
			if (d > 180.0f) {
				d -= 360.0f;
			} else if (d < -180.0f) {
				d += 360.0f;
			}
			return d;
		}

		// The 3D root and the face against GetAngleZ, for the result line:
		// the root off says the body is not where the pose of the body goes.
		std::string TurnText(RE::NiAVObject* a_root, RE::NiAVObject* a_head, float a_angleZ)
		{
			float      hr = 0.0f, hf = 0.0f;
			const bool r = a_root && HeadingOf(a_root, hr);
			const bool f = a_head && HeadingOf(a_head, hf);
			return std::format("root {} face {}", r ? std::format("{:.1f}", TurnDeg(hr, a_angleZ)) : std::string("-"),
				f ? std::format("{:.1f}", TurnDeg(hf, a_angleZ)) : std::string("-"));
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
			RestoreHair();
			const auto light = LightOff();
			if (g_active && g_active->cutout && light != "off") {
				g_active->light += " -> " + light;
			}
			ResumeTracking();
			GiveCameraBack();
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

		// ---------------------------------------------------------------
		// depth (the cutout)
		// ---------------------------------------------------------------

		std::uint32_t DepthBpp(DXGI_FORMAT a_fmt)
		{
			switch (a_fmt) {
			case DXGI_FORMAT_R24G8_TYPELESS:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
			case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT:
			case DXGI_FORMAT_R32_FLOAT:
				return 4;
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
				return 8;
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_R16_UNORM:
				return 2;
			default:
				return 0;
			}
		}

		// Depth in [0, 1] and the stencil (-1 when the format has none).
		void DepthAt(DXGI_FORMAT a_fmt, const std::uint8_t* a_p, float& a_z, std::int32_t& a_stencil)
		{
			a_stencil = -1;
			switch (a_fmt) {
			case DXGI_FORMAT_R24G8_TYPELESS:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
			case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
				{
					std::uint32_t v;
					std::memcpy(&v, a_p, 4);
					a_z = static_cast<float>(v & 0xFFFFFFu) / 16777215.0f;
					a_stencil = static_cast<std::int32_t>(v >> 24);
					return;
				}
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
				std::memcpy(&a_z, a_p, 4);
				a_stencil = a_p[4];
				return;
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT:
			case DXGI_FORMAT_R32_FLOAT:
				std::memcpy(&a_z, a_p, 4);
				return;
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_R16_UNORM:
				{
					std::uint16_t v;
					std::memcpy(&v, a_p, 2);
					a_z = static_cast<float>(v) / 65535.0f;
					return;
				}
			default:
				a_z = 0.0f;
				return;
			}
		}

		// Eye distance from a depth value: the standard projection
		// (z = 0 at near) and the inverted one (z = 0 at far).
		float DistStd(float a_z, float a_n, float a_f)
		{
			const float den = a_f - a_z * (a_f - a_n);
			return den > 1e-6f ? a_n * a_f / den : -1.0f;
		}

		float DistInv(float a_z, float a_n, float a_f)
		{
			const float den = a_n + a_z * (a_f - a_n);
			return den > 1e-6f ? a_n * a_f / den : -1.0f;
		}

		// Box filter weighted by alpha: a kept pixel next to a dropped one
		// keeps its own color and the edge goes soft in alpha only.
		std::vector<std::uint8_t> ScaleAlpha(const std::vector<std::uint8_t>& a_src, std::uint32_t a_srcSide, std::uint32_t a_size)
		{
			std::vector<std::uint8_t> out(static_cast<std::size_t>(a_size) * a_size * 4, 0);
			const float step = static_cast<float>(a_srcSide) / static_cast<float>(a_size);
			for (std::uint32_t oy = 0; oy < a_size; ++oy) {
				const auto sy0 = static_cast<std::uint32_t>(oy * step);
				const auto sy1 = (std::max)(sy0 + 1, (std::min)(a_srcSide, static_cast<std::uint32_t>((oy + 1) * step)));
				for (std::uint32_t ox = 0; ox < a_size; ++ox) {
					const auto sx0 = static_cast<std::uint32_t>(ox * step);
					const auto sx1 = (std::max)(sx0 + 1, (std::min)(a_srcSide, static_cast<std::uint32_t>((ox + 1) * step)));
					std::uint64_t acc[3] = { 0, 0, 0 };
					std::uint64_t accA = 0;
					std::uint32_t n = 0;
					for (std::uint32_t sy = sy0; sy < sy1 && sy < a_srcSide; ++sy) {
						for (std::uint32_t sx = sx0; sx < sx1 && sx < a_srcSide; ++sx) {
							const auto* p = a_src.data() + (static_cast<std::size_t>(sy) * a_srcSide + sx) * 4;
							acc[0] += static_cast<std::uint64_t>(p[0]) * p[3];
							acc[1] += static_cast<std::uint64_t>(p[1]) * p[3];
							acc[2] += static_cast<std::uint64_t>(p[2]) * p[3];
							accA += p[3];
							++n;
						}
					}
					auto* d = out.data() + (static_cast<std::size_t>(oy) * a_size + ox) * 4;
					if (accA) {
						d[0] = static_cast<std::uint8_t>(acc[0] / accA);
						d[1] = static_cast<std::uint8_t>(acc[1] / accA);
						d[2] = static_cast<std::uint8_t>(acc[2] / accA);
					}
					d[3] = n ? static_cast<std::uint8_t>(accA / n) : 0;
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

		// A file of an earlier capture and the roots that still have it
		// (index 0 = PrismaUI, 1 = Magelight).
		struct OldFile
		{
			std::string         name;
			std::array<bool, 2> has{ false, false };
		};

		constexpr std::array<const char*, 2> kRootNames{ "prisma", "magelight" };

		// Files of earlier captures, per folder and key. A name stays listed
		// until both roots let it go; every capture to the folder and key
		// tries again, and only the roots that still have it.
		struct FolderState
		{
			bool                 swept = false;
			std::int64_t         next = 1;
			std::vector<OldFile> old;  // files still to delete
		};
		std::mutex                          g_folderLock;
		std::map<std::string, FolderState>  g_folders;  // folder + '|' + key

		// The key as it goes into a file name: letters, digits, '_' and '-',
		// lowercased (the file system does not tell case apart); any other
		// byte becomes '_'; at most kMaxKey bytes.
		std::string SanitizeKey(std::string_view a_key)
		{
			std::string out;
			for (const char c : a_key) {
				if (out.size() >= kMaxKey) {
					break;
				}
				const auto u = static_cast<unsigned char>(c);
				if (u >= 'A' && u <= 'Z') {
					out += static_cast<char>(u - 'A' + 'a');
				} else if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || c == '_' || c == '-') {
					out += c;
				} else {
					out += '_';
				}
			}
			return out;
		}

		// The capture number in a_name when it is a file of a_key ("" = the
		// names without a key: the prefix, digits, ".png"), else -1. A
		// ".png.tmp" counts when a_tmp.
		std::int64_t NumberOf(std::string_view a_name, const std::string& a_key, bool a_tmp)
		{
			const std::string_view p(kPrefix);
			if (!a_name.starts_with(p)) {
				return -1;
			}
			a_name.remove_prefix(p.size());
			if (!a_key.empty()) {
				if (a_name.size() <= a_key.size() || a_name.substr(0, a_key.size()) != a_key || a_name[a_key.size()] != '-') {
					return -1;
				}
				a_name.remove_prefix(a_key.size() + 1);
			}
			if (a_name.ends_with(".png")) {
				a_name.remove_suffix(4);
			} else if (a_tmp && a_name.ends_with(".png.tmp")) {
				a_name.remove_suffix(8);
			} else {
				return -1;
			}
			if (a_name.empty() || a_name.size() > 9) {
				return -1;
			}
			std::int64_t v = 0;
			for (const char c : a_name) {
				if (c < '0' || c > '9') {
					return -1;
				}
				v = v * 10 + (c - '0');
			}
			return v;
		}

		// First capture to a folder and key in this session: what earlier
		// sessions left there joins the delete list, and the numbering goes on
		// past the highest left, so the newest file has the highest number.
		void Sweep(const std::string& a_folder, const std::string& a_key, FolderState& a_state)
		{
			a_state.swept = true;
			const auto roots = Roots(a_folder);
			for (std::size_t i = 0; i < roots.size(); ++i) {
				std::error_code ec;
				for (std::filesystem::directory_iterator it(roots[i], ec), end; !ec && it != end; it.increment(ec)) {
					const std::string name = it->path().filename().string();
					const auto n = NumberOf(name, a_key, true);
					if (n < 0) {
						continue;
					}
					a_state.next = (std::max)(a_state.next, n + 1);
					auto found = std::find_if(a_state.old.begin(), a_state.old.end(), [&](const OldFile& f) { return f.name == name; });
					if (found == a_state.old.end()) {
						a_state.old.push_back({ name, { false, false } });
						found = a_state.old.end() - 1;
					}
					found->has[i] = true;
				}
			}
		}

		// A name no root has.
		std::string NewName(const std::string& a_folder, const std::string& a_key, FolderState& a_state)
		{
			const auto roots = Roots(a_folder);
			for (;;) {
				const auto n = a_state.next++;
				std::string name = a_key.empty() ? std::format("{}{}.png", kPrefix, n) : std::format("{}{}-{}.png", kPrefix, a_key, n);
				std::error_code e0, e1;
				if (!std::filesystem::exists(roots[0] / name, e0) && !std::filesystem::exists(roots[1] / name, e1)) {
					return name;
				}
			}
		}

		// What a round of deletes left, for the result line.
		struct RemoveReport
		{
			std::int32_t             deleted = 0;
			std::int32_t             dropped = 0;  // names let go past kMaxOld
			std::vector<std::string> keptNames;
			std::string              kept;           // names some root still has
			std::string              keptPrisma;     // names the PrismaUI root still has
			std::string              keptMagelight;  // names the Magelight root still has
			std::string              errors;         // root:name:value:message of each delete that failed
		};

		void Append(std::string& a_list, const std::string& a_item)
		{
			a_list += (a_list.empty() ? "" : "|") + a_item;
		}

		// Deletes a_file from every root that still has it; true when none does.
		// std::filesystem::remove sets the error code only when the file is
		// there and cannot go.
		bool TryRemove(const std::array<std::filesystem::path, 2>& a_roots, OldFile& a_file, RemoveReport& a_report)
		{
			for (std::size_t i = 0; i < a_roots.size(); ++i) {
				if (!a_file.has[i]) {
					continue;
				}
				std::error_code ec;
				std::filesystem::remove(a_roots[i] / a_file.name, ec);
				if (!ec) {
					a_file.has[i] = false;
					continue;
				}
				std::string text = ec.message();
				for (auto& c : text) {
					const auto u = static_cast<unsigned char>(c);
					if (u < 32 || u > 126 || c == '|' || c == ']' || c == '[') {
						c = '?';
					}
				}
				Append(a_report.errors, std::format("{}:{}:{}:{}", kRootNames[i], a_file.name, ec.value(), text));
			}
			return !a_file.has[0] && !a_file.has[1];
		}

		void NoteKept(const OldFile& a_file, RemoveReport& a_report)
		{
			a_report.keptNames.push_back(a_file.name);
			Append(a_report.kept, a_file.name);
			if (a_file.has[0]) {
				Append(a_report.keptPrisma, a_file.name);
			}
			if (a_file.has[1]) {
				Append(a_report.keptMagelight, a_file.name);
			}
		}

		// Deletes every listed name it can and leaves the rest listed, with the
		// roots that still have each.
		RemoveReport DeleteOld(const std::string& a_folder, FolderState& a_state)
		{
			RemoveReport         report;
			const auto           roots = Roots(a_folder);
			std::vector<OldFile> keep;
			for (auto& file : a_state.old) {
				if (TryRemove(roots, file, report)) {
					++report.deleted;
				} else {
					keep.push_back(file);
					NoteKept(file, report);
				}
			}
			while (keep.size() > kMaxOld) {
				keep.erase(keep.begin());
				++report.dropped;
			}
			a_state.old = std::move(keep);
			return report;
		}

		// The names a capture could not delete, tried again in its own worker
		// thread after the script has its answer: a page that shows the previous
		// capture lets it go when it shows the new one. One line says how it
		// ended.
		void RetryKept(std::int32_t a_seq, const std::string& a_folder, const std::string& a_key, std::vector<std::string> a_names)
		{
			RemoveReport last;
			int          tries = 0;
			std::int32_t deleted = 0;
			while (tries < kRetryTries && !a_names.empty()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(kRetryMs));
				++tries;
				RemoveReport round;
				std::scoped_lock l(g_folderLock);
				auto&            state = g_folders[a_folder + "|" + a_key];
				const auto       roots = Roots(a_folder);
				std::vector<std::string> left;
				std::vector<OldFile>     keep;
				for (auto& file : state.old) {
					if (std::find(a_names.begin(), a_names.end(), file.name) == a_names.end()) {
						keep.push_back(file);  // not one of ours to retry
					} else if (TryRemove(roots, file, round)) {
						++deleted;
					} else {
						keep.push_back(file);
						left.push_back(file.name);
						NoteKept(file, round);
					}
				}
				state.old = std::move(keep);
				a_names = std::move(left);  // a name a later capture deleted is no longer in state.old
				last = std::move(round);
			}
			spdlog::info("Portrait: #{} cleanup folder={} tries={} deleted={} left=[{}] leftPrisma=[{}] leftMagelight=[{}] removeErr=[{}]",
				a_seq, a_folder, tries, deleted, last.kept, last.keptPrisma, last.keptMagelight, last.errors);
		}

		// The cutout part of the result lines.
		std::string CutText(const Job& a_job)
		{
			if (!a_job.cutout) {
				return {};
			}
			return std::format(" key={} trim={} hair=[{}] hairReapplied={} light=[{}] litFrames={} mapSteps={} stageMs={:.3f}", a_job.key.empty() ? "(none)" : a_job.key,
				a_job.trim ? "asked" : "off", a_job.hair, a_job.hairReapplied, a_job.light, a_job.litFrames, a_job.mapSteps, a_job.stageMs);
		}

		// ---------------------------------------------------------------
		// the worker
		// ---------------------------------------------------------------

		// The head part of the result lines.
		std::string HeadText(const Job& a_job)
		{
			return std::format("poseBy={} head=(take {:.1f} pose {:.1f} copy {:.1f} yaw {:.1f} pitch {:.1f} axis {:.1f}) turn=[{}] track=[{}]",
				a_job.poseBy, a_job.headTake, a_job.headArm, a_job.headCopy, a_job.yawCopy, a_job.pitchCopy, a_job.axisDeg, a_job.turn, a_job.track) +
			       std::format(" fade=[take=({:.3f},{:.3f},{:.3f}) min=({:.3f},{:.3f}) held={} copy=({:.3f},{:.3f},{:.3f}) camTake={:.1f}]", a_job.fadeTake[0],
					   a_job.fadeTake[1], a_job.fadeTake[2], a_job.fadeMin[0], a_job.fadeMin[1], a_job.fadeHeld, a_job.fadeCopy[0], a_job.fadeCopy[1],
					   a_job.fadeCopy[2], a_job.camTake) +
			       (a_job.span > 0.0f ? std::format(" frame=[span={:.1f} aimUp={:.1f} extTake={:.4f} extCopy={:.4f} dist={:.2f}{}]", a_job.span, a_job.aimUp,
												 a_job.extTake, a_job.extCopy, a_job.frameDist, a_job.frameClamped ? " clamped" : "") :
										std::string());
		}

		// The camera part of the result lines.
		std::string CameraText(const Job& a_job)
		{
			return std::format("frames=[{}] smoothcam=[{}]", a_job.frames, a_job.smooth);
		}

		void LogRefusal(const Job& a_job)
		{
			spdlog::warn("Portrait: #{} FAILED code={} {} folder={} size={} totalMs={:.2f} armFrames={} poseFrames={} mapTries={} mapMs={:.3f} hookMaxMs={:.3f} helmet=[{}] {} {}{} why=\"{}\" - the script gets \"\".",
				a_job.seq, a_job.code, a_job.cutout ? "cutout" : "plain", a_job.folder, a_job.size, MsSince(a_job.t0), a_job.armFrames, a_job.poseFrames, a_job.mapTries,
				a_job.mapMs, a_job.hookMaxMs, a_job.helmet, CameraText(a_job), HeadText(a_job), CutText(a_job), a_job.why);
		}

		// Luminance (Rec. 709 weights on the 8 bit values) and mean color of
		// the opaque pixels, and their share of the box.
		void ToneOf(const std::vector<std::uint8_t>& a_rgba, double& a_lum, std::array<double, 3>& a_tone, double& a_opaque)
		{
			std::uint64_t n = 0, total = 0;
			std::array<std::uint64_t, 3> acc{};
			for (std::size_t i = 0; i + 3 < a_rgba.size(); i += 4) {
				++total;
				if (a_rgba[i + 3] < 255) {
					continue;
				}
				++n;
				acc[0] += a_rgba[i + 0];
				acc[1] += a_rgba[i + 1];
				acc[2] += a_rgba[i + 2];
			}
			a_opaque = total ? 100.0 * static_cast<double>(n) / static_cast<double>(total) : 0.0;
			for (std::size_t k = 0; k < 3; ++k) {
				a_tone[k] = n ? static_cast<double>(acc[k]) / static_cast<double>(n) : 0.0;
			}
			a_lum = 0.2126 * a_tone[0] + 0.7152 * a_tone[1] + 0.0722 * a_tone[2];
		}

		// The cutout, on the worker: alpha from the depth box into a_rgba (the
		// color box, side boxS). "" = done, else why it failed; a_log gets
		// what was measured.
		std::string Cutout(Job& a_job, std::vector<std::uint8_t>& a_rgba, std::string& a_log)
		{
			const std::uint32_t ds = a_job.dbs;
			const std::uint32_t s = a_job.boxS;
			const std::size_t   n = static_cast<std::size_t>(ds) * ds;
			if (ds < 8 || a_job.depthRaw.size() < n * a_job.depthBpp) {
				return "depth box not read";
			}
			if (a_job.ortho) {
				return "orthographic scene camera, depth not read";
			}
			const auto fmt = static_cast<DXGI_FORMAT>(a_job.depthFormat);
			std::vector<float> z(n);
			for (std::size_t i = 0; i < n; ++i) {
				std::int32_t st = -1;
				DepthAt(fmt, a_job.depthRaw.data() + i * a_job.depthBpp, z[i], st);
			}
			std::vector<std::uint8_t>().swap(a_job.depthRaw);

			// The convention, from the 5 x 5 median at the box centre: the aim
			// point, on the face.
			std::vector<float> mid;
			const auto c = static_cast<std::int32_t>(ds / 2);
			for (std::int32_t y = c - 2; y <= c + 2; ++y) {
				for (std::int32_t x = c - 2; x <= c + 2; ++x) {
					mid.push_back(z[static_cast<std::size_t>(y) * ds + static_cast<std::size_t>(x)]);
				}
			}
			std::nth_element(mid.begin(), mid.begin() + mid.size() / 2, mid.end());
			const float zc = mid[mid.size() / 2];
			const float dStd = DistStd(zc, a_job.nearP, a_job.farP);
			const float dInv = DistInv(zc, a_job.nearP, a_job.farP);
			bool inv = false;
			if (dStd > 0.0f && std::fabs(dStd - a_job.dPose) <= kConvBand) {
				inv = false;
			} else if (dInv > 0.0f && std::fabs(dInv - a_job.dPose) <= kConvBand) {
				inv = true;
			} else {
				return std::format("depth at the face matches no convention (z {:.7f} std {:.2f} inv {:.2f} head {:.2f} near {:.3f} far {:.1f})", zc, dStd, dInv,
					a_job.dPose, a_job.nearP, a_job.farP);
			}

			// The reach: up to kCutReach past the head node.
			std::vector<float>        dist(n);
			std::vector<std::uint8_t> reach(n, 0);
			std::size_t               nReach = 0;
			const float               limit = a_job.dPose + kCutReach;
			for (std::size_t i = 0; i < n; ++i) {
				dist[i] = inv ? DistInv(z[i], a_job.nearP, a_job.farP) : DistStd(z[i], a_job.nearP, a_job.farP);
				if (dist[i] > 0.0f && dist[i] <= limit) {
					reach[i] = 1;
					++nReach;
				}
			}

			// The join: from seeds at the centre, over 8-neighbours within the
			// reach whose depth steps by no more than kTrimStep.
			std::vector<std::uint8_t>  joined(n, 0);
			std::vector<std::uint32_t> queue;
			const std::int32_t         half = (std::max)(1, static_cast<std::int32_t>(ds / 20));
			std::size_t                seeds = 0;
			for (std::int32_t y = c - half; y <= c + half; ++y) {
				for (std::int32_t x = c - half; x <= c + half; ++x) {
					if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(ds) || y >= static_cast<std::int32_t>(ds)) {
						continue;
					}
					const auto i = static_cast<std::uint32_t>(y) * ds + static_cast<std::uint32_t>(x);
					if (reach[i] && !joined[i] && std::fabs(dist[i] - a_job.dPose) <= kSeedBand) {
						joined[i] = 1;
						queue.push_back(i);
						++seeds;
					}
				}
			}
			for (std::size_t q = 0; q < queue.size(); ++q) {
				const std::uint32_t i = queue[q];
				const auto          x = static_cast<std::int32_t>(i % ds);
				const auto          y = static_cast<std::int32_t>(i / ds);
				for (std::int32_t dy = -1; dy <= 1; ++dy) {
					for (std::int32_t dx = -1; dx <= 1; ++dx) {
						const std::int32_t nx = x + dx, ny = y + dy;
						if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= static_cast<std::int32_t>(ds) || ny >= static_cast<std::int32_t>(ds)) {
							continue;
						}
						const auto j = static_cast<std::uint32_t>(ny) * ds + static_cast<std::uint32_t>(nx);
						if (!joined[j] && reach[j] && std::fabs(dist[j] - dist[i]) <= kTrimStep) {
							joined[j] = 1;
							queue.push_back(j);
						}
					}
				}
			}
			const std::size_t nJoined = queue.size();
			const bool        useJoin = a_job.trim && seeds > 0;
			const auto&       keep = useJoin ? joined : reach;
			for (std::uint32_t y = 0; y < s; ++y) {
				const std::uint32_t dy = (std::min)(ds - 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * ds / s));
				for (std::uint32_t x = 0; x < s; ++x) {
					const std::uint32_t dx = (std::min)(ds - 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * ds / s));
					a_rgba[(static_cast<std::size_t>(y) * s + x) * 4 + 3] = keep[static_cast<std::size_t>(dy) * ds + dx] ? 255 : 0;
				}
			}
			a_log = std::format("conv={} zCentre={:.7f} dCentre={:.2f} dPose={:.2f} near={:.3f} far={:.1f} depthFmt={} depthBox=({},{},{}) reach={:.1f}% joined={:.1f}% seeds={} trimmed={} dropsOfReach={:.1f}%",
				inv ? "inv" : "std", zc, inv ? dInv : dStd, a_job.dPose, a_job.nearP, a_job.farP, a_job.depthFormat, a_job.dbx, a_job.dby, ds,
				100.0 * static_cast<double>(nReach) / static_cast<double>(n), 100.0 * static_cast<double>(nJoined) / static_cast<double>(n), seeds,
				useJoin ? "yes" : (a_job.trim ? "no (no seed)" : "no (off)"),
				nReach ? 100.0 * static_cast<double>(nReach - (std::min)(nReach, nJoined)) / static_cast<double>(nReach) : 0.0);
			return {};
		}

		void Finish(std::shared_ptr<Job> a_job)
		{
			std::string  file;
			std::string  write = "not written";
			RemoveReport removed;
			std::string  cut;
			double       lum = -1.0;
			double       opaque = -1.0;
			std::array<double, 3> tone{};
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
					std::string err;
					if (!converted) {
						err = std::format("format {} not read", a_job->format);
					} else if (a_job->cutout) {
						err = Cutout(*a_job, rgba, cut);
						if (err.empty()) {
							ToneOf(rgba, lum, tone, opaque);
						}
					}
					if (!err.empty()) {
						a_job->ok = false;
						a_job->why = err;
					} else {
						const auto size = static_cast<std::uint32_t>(a_job->size);
						const auto px = a_job->cutout ? ScaleAlpha(rgba, s, size) : Scale(rgba, s, size);
						std::scoped_lock l(g_folderLock);
						auto& state = g_folders[a_job->folder + "|" + a_job->key];
						if (!state.swept) {
							Sweep(a_job->folder, a_job->key, state);
						}
						const std::string name = NewName(a_job->folder, a_job->key, state);
						const auto roots = Roots(a_job->folder);
						err = WritePng(roots[0] / name, px, size, size);
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
							removed = DeleteOld(a_job->folder, state);
							state.old.push_back({ name, { true, true } });  // deleted by the next capture to this folder and key
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
				const std::string cutLine = a_job->cutout ? std::format(" cut=[{}] lum={:.1f} tone=({:.1f},{:.1f},{:.1f}) opaque={:.1f}%", cut, lum, tone[0], tone[1], tone[2], opaque) :
				                                            std::string();
				spdlog::info("Portrait: #{} ok {} folder={} file={} size={} totalMs={:.2f} box=({},{},{}) tex={}x{} fmt={} poseFrames={} armFrames={} shadowToPose={:.2f} camToPose={:.2f} rootToPose={:.2f} crop={} port=({}) matrixCalls={} mapTries={} mapMs={:.3f} hookMaxMs={:.3f} helmet=[{}] slots=[{}] {} {}{}{} write={} deleted={} kept=[{}] keptPrisma=[{}] keptMagelight=[{}] removeErr=[{}] dropped={}{}{}",
					a_job->seq, a_job->cutout ? "cutout" : "plain", a_job->folder, file, a_job->size, MsSince(a_job->t0), a_job->boxX, a_job->boxY, a_job->boxS,
					a_job->texW, a_job->texH, a_job->format, a_job->poseFrames, a_job->armFrames, a_job->shadowToPose, a_job->camToPose, a_job->rootToPose,
					a_job->crop, a_job->port, a_job->matrixCalls, a_job->mapTries, a_job->mapMs, a_job->hookMaxMs, a_job->helmet, a_job->slots, CameraText(*a_job), HeadText(*a_job),
					CutText(*a_job), cutLine, write, removed.deleted, removed.kept, removed.keptPrisma, removed.keptMagelight, removed.errors, removed.dropped, a_job->diag.empty() ? "" : " ", a_job->diag);
			} else {
				LogRefusal(*a_job);
				SetLastFailure(a_job->code);
			}
			Answer(a_job->seq, file);
			g_busy.store(false, std::memory_order_release);
			if (!removed.keptNames.empty()) {
				RetryKept(a_job->seq, a_job->folder, a_job->key, std::move(removed.keptNames));
			}
		}

		// ---------------------------------------------------------------
		// the frame hook
		// ---------------------------------------------------------------

		ID3D11Texture2D* g_depthStaging = nullptr;
		std::uint32_t    g_stageSide = 0;  // side of the color staging of a cutout
		std::uint32_t    g_stageFormat = 0;

		void EndActive()
		{
			ResumeTracking();
			GiveCameraBack();
			if (g_staging) {
				g_staging->Release();
				g_staging = nullptr;
			}
			if (g_depthStaging) {
				g_depthStaging->Release();
				g_depthStaging = nullptr;
			}
			g_stageSide = 0;
			g_stageFormat = 0;
			auto job = std::move(g_active);
			g_active.reset();
			g_phase = Phase::kIdle;
			if (job) {
				std::thread([job]() { Finish(job); }).detach();
			}
		}

		// The two staging textures of a cutout, made in the armed frames
		// before the copy, one per frame; a_both makes what is missing now.
		// The color one is the largest square kMAIN can give; the depth one
		// has the depth target's description, since a depth resource is only
		// copied whole. "" = ok.
		std::string PrepareStaging(bool a_both)
		{
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!renderer) {
				return "no renderer for the staging";
			}
			auto& rd = renderer->GetRuntimeData();
			auto* device = reinterpret_cast<ID3D11Device*>(rd.forwarder);
			auto* tex = rd.renderTargets[RE::RENDER_TARGETS::kMAIN].texture;
			auto* dtex = renderer->GetDepthStencilData().depthStencils[0].texture;  // kMAIN's depth, index 0 on SE and AE
			if (!device || !tex || !dtex) {
				return "no device, kMAIN or its depth texture for the staging";
			}
			if (!g_staging) {
				D3D11_TEXTURE2D_DESC desc{};
				tex->GetDesc(&desc);
				const std::uint32_t side = (std::min)(desc.Width, desc.Height);
				D3D11_TEXTURE2D_DESC sd{};
				sd.Width = side;
				sd.Height = side;
				sd.MipLevels = 1;
				sd.ArraySize = 1;
				sd.Format = desc.Format;
				sd.SampleDesc.Count = 1;
				sd.Usage = D3D11_USAGE_STAGING;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				const HRESULT hr = device->CreateTexture2D(&sd, nullptr, &g_staging);
				if (FAILED(hr) || !g_staging) {
					g_staging = nullptr;
					return std::format("CreateTexture2D color staging hr=0x{:08X}", static_cast<std::uint32_t>(hr));
				}
				g_stageSide = side;
				g_stageFormat = static_cast<std::uint32_t>(desc.Format);
				if (!a_both) {
					return {};
				}
			}
			if (!g_depthStaging) {
				D3D11_TEXTURE2D_DESC dd{};
				dtex->GetDesc(&dd);
				dd.Usage = D3D11_USAGE_STAGING;
				dd.BindFlags = 0;
				dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				dd.MiscFlags = 0;
				const HRESULT hr = device->CreateTexture2D(&dd, nullptr, &g_depthStaging);
				if (FAILED(hr) || !g_depthStaging) {
					g_depthStaging = nullptr;
					return std::format("CreateTexture2D depth staging hr=0x{:08X}", static_cast<std::uint32_t>(hr));
				}
			}
			return {};
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

		// The refusals, at the take and again at the pose. "" = go on; a_code
		// gets the reason code of a refusal.
		std::string Refusal(RE::PlayerCharacter*& a_player, RE::NiAVObject*& a_head, RE::NiAVObject*& a_root, std::string& a_code)
		{
			auto* ui = RE::UI::GetSingleton();
			if (ui && ui->GameIsPaused()) {
				a_code = "menu";
				return "a menu pauses the game";
			}
			auto* cam = RE::PlayerCamera::GetSingleton();
			a_player = RE::PlayerCharacter::GetSingleton();
			if (!cam || !a_player || !cam->cameraRoot || !cam->IsInThirdPerson() ||
				!cam->IsCurrentState(RE::CameraStates::kThirdPerson, RE::CameraStates::kVRThirdPerson)) {
				a_code = "firstperson";
				return "camera not in the plain third person state";
			}
			a_head = HeadNode(a_player, &a_root);
			if (!a_head) {
				a_code = "other";
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
			if (auto why = Refusal(player, head, root, a_job.code); !why.empty()) {
				return why;
			}
			TrackingOff(player);
			const RE::NiPoint3 h = head->world.translate;
			g_frontPos = PoseFor(a_job, h, a_job.heading);
			g_frontRot = LookAtMatrix(g_frontPos, AimPoint(a_job, h));
			g_frontQuat = QuatFromMatrix(g_frontRot);
			a_job.headArm = AimAt(head, nullptr, g_frontPos).deg;
			a_job.helmet = HideHelmet(player);
			if (a_job.cutout) {
				a_job.hair = ShowHair(player);
				a_job.light = LightOn(root, h);
			}
			g_writes.store(0, std::memory_order_relaxed);
			g_matrixCalls.store(0, std::memory_order_relaxed);
			g_fadeHeld.store(0, std::memory_order_relaxed);
			g_fadeMinCf = 2.0f;
			g_fadeMinFa = 2.0f;
			g_lastWrites = 0;
			g_armed.store(true, std::memory_order_release);
			g_phase = Phase::kArmed;
			return {};
		}

		// The take: refuse, or switch head tracking off and arm the pose at
		// once - on the body's heading when the face is in front of it, else
		// on the face's (D-P5-B-15). "" = on.
		std::string Take(Job& a_job)
		{
			RE::PlayerCharacter* player = nullptr;
			RE::NiAVObject*      head = nullptr;
			RE::NiAVObject*      root = nullptr;
			if (auto why = Refusal(player, head, root, a_job.code); !why.empty()) {
				return why;
			}
			try {
				a_job.slots = SlotOwners();
			} catch (...) {
				a_job.slots = "not read";
			}
			a_job.smooth = Sc::g_api.load(std::memory_order_acquire) ? "not asked" : "absent";
			if (auto why = SetFrameDist(a_job); !why.empty()) {
				a_job.code = "other";
				return why;
			}
			a_job.fadeTake = FadeOf(player);
			if (auto* pc = RE::PlayerCamera::GetSingleton(); pc && pc->cameraRoot) {
				a_job.camTake = Dist3(pc->cameraRoot->world.translate, head->world.translate);
			}
			a_job.track = SuspendTracking(player);
			const float angleZ = player->GetAngleZ();
			a_job.turn = TurnText(root, head, angleZ);
			a_job.heading = angleZ;
			const auto aim = AimAt(head, root, PoseFor(a_job, head->world.translate, angleZ));
			a_job.headTake = aim.deg;
			a_job.axisDeg = aim.axis;
			// A face more than kHeadReady off the body's pose: the pose goes in
			// front of the face. A face with no horizontal heading keeps the
			// body's, and the guard at the copy decides.
			if (aim.deg > kHeadReady) {
				if (float hf = 0.0f; HeadingOf(head, hf)) {
					a_job.heading = hf;
					a_job.poseBy = "face";
				}
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

			// The fade guard (D-P5-B-12): this swap ends the frame being copied,
			// drawn with the fade read now. A faded player is the wall behind
			// in the color and the player in the depth: refuse, never return it.
			job.fadeCopy = FadeOf(player);
			job.fadeHeld = g_fadeHeld.load(std::memory_order_relaxed);
			job.fadeMin = { g_fadeMinCf > 1.5f ? -1.0f : g_fadeMinCf, g_fadeMinFa > 1.5f ? -1.0f : g_fadeMinFa };
			if (Faded(job.fadeCopy)) {
				job.code = "faded";
				FailActive(std::format("the player is faded at the copy (currentFade {:.3f} fadeAmount {:.3f} alpha {:.3f}, lowest {:.3f} {:.3f}, held {}, game camera {:.1f} from the head at the take) - the frame shows what is behind the player, not a portrait",
					job.fadeCopy[0], job.fadeCopy[1], job.fadeCopy[2], job.fadeMin[0], job.fadeMin[1], job.fadeHeld, job.camTake));
				return;
			}

			// The head guard: the frame is the pose's, which follows the body;
			// a face turned away from it is a profile on either crop path.
			const auto aim = AimAt(head, nullptr, g_frontPos);
			job.headCopy = aim.deg;
			job.yawCopy = aim.yaw;
			job.pitchCopy = aim.pitch;
			if (aim.deg < 0.0f || aim.deg > kHeadLimit) {
				job.code = "face";
				FailActive(std::format("head turned {:.1f} deg from the camera (yaw {:.1f} pitch {:.1f}, limit {:.0f}) with head tracking off, the pose on the {}'s heading (take {:.1f} deg, pose {:.1f} deg) turn=[{}] track=[{}] - not a front portrait",
					aim.deg, aim.yaw, aim.pitch, kHeadLimit, job.poseBy, job.headTake, job.headArm, job.turn, job.track));
				return;
			}

			job.camToPose = Dist3(camera->world.translate, g_frontPos);
			if (auto* pc = RE::PlayerCamera::GetSingleton(); pc && pc->cameraRoot) {
				job.rootToPose = Dist3(pc->cameraRoot->world.translate, g_frontPos);
			}
			// The camera guard: the frame being copied was drawn from where the
			// scene camera is now. Off the pose it is the game camera's frame,
			// whatever the crop would place on it.
			if (job.camToPose > kPoseTol) {
				job.code = "camera";
				FailActive(std::format("the scene camera is {:.2f} from the pose at the copy (rootToPose={:.2f} shadowToPose={:.2f}) {} slots=[{}] - the frame is the game camera's, not a portrait",
					job.camToPose, job.rootToPose, job.shadowToPose, CameraText(job), job.slots));
				return;
			}
			// The crop centre and side: the head and 2.4 head spans, or with a
			// framing (the cutout) the aim point and the framing span.
			const RE::NiPoint3 c = AimPoint(job, p);
			const float cropScale = job.span > 0.0f ? job.span / kHeadSpan : kCrop;  // crop side in head spans
			job.extCopy = FrustumExt();
			float hx = 0, hy = 0, hz = 0, xa = 0, ya = 0, za = 0, xb = 0, yb = 0, zb = 0;
			const bool front = camera->WorldPtToScreenPt3(c, hx, hy, hz, 1e-5f);
			const bool a = camera->WorldPtToScreenPt3(RE::NiPoint3(c.x, c.y, c.z + kHeadSpan / 2.0f), xa, ya, za, 1e-5f);
			const bool b = camera->WorldPtToScreenPt3(RE::NiPoint3(c.x, c.y, c.z - kHeadSpan / 2.0f), xb, yb, zb, 1e-5f);
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
				const float d = Dist3(g_frontPos, c);
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
				job.code = "camera";
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
			const float side = (std::clamp)(spanY * h * cropScale, 8.0f, (std::min)(w, h));
			const auto s = static_cast<std::uint32_t>(side);
			job.boxX = static_cast<std::uint32_t>((std::clamp)(cx - side / 2.0f, 0.0f, w - static_cast<float>(s)));
			job.boxY = static_cast<std::uint32_t>((std::clamp)(cy - side / 2.0f, 0.0f, h - static_cast<float>(s)));
			job.boxS = s;
			job.rowBytes = s * bpp;

			if (job.cutout) {
				// The depth target of kMAIN, copied whole with the color box,
				// to the stagings the armed frames made.
				auto* dtex = renderer->GetDepthStencilData().depthStencils[0].texture;  // kMAIN's depth, index 0 on SE and AE
				if (!dtex) {
					FailActive("no depth texture for kMAIN");
					return;
				}
				D3D11_TEXTURE2D_DESC dd{};
				dtex->GetDesc(&dd);
				job.depthFormat = static_cast<std::uint32_t>(dd.Format);
				job.depthBpp = DepthBpp(dd.Format);
				if (dd.SampleDesc.Count > 1 || job.depthBpp == 0 || dd.Width < 8 || dd.Height < 8) {
					FailActive(std::format("depth format {} samples {} size {}x{} not read", job.depthFormat, dd.SampleDesc.Count, dd.Width, dd.Height));
					return;
				}
				const double dsx = static_cast<double>(dd.Width) / desc.Width;
				const double dsy = static_cast<double>(dd.Height) / desc.Height;
				job.dbs = (std::max)(1u, static_cast<std::uint32_t>(s * dsy));
				job.dbs = (std::min)({ job.dbs, dd.Width, dd.Height });
				job.dbx = (std::min)(static_cast<std::uint32_t>(job.boxX * dsx), dd.Width - job.dbs);
				job.dby = (std::min)(static_cast<std::uint32_t>(job.boxY * dsy), dd.Height - job.dbs);
				const auto& fr = camera->GetRuntimeData2().viewFrustum;
				job.nearP = fr.fNear;
				job.farP = fr.fFar;
				job.ortho = fr.bOrtho;
				job.dPose = Dist3(g_frontPos, p);
				// A staging that no longer fits (the target changed since the
				// armed frames) is made again.
				if (g_staging && (s > g_stageSide || g_stageFormat != job.format)) {
					g_staging->Release();
					g_staging = nullptr;
				}
				if (g_depthStaging) {
					D3D11_TEXTURE2D_DESC have{};
					g_depthStaging->GetDesc(&have);
					if (have.Width != dd.Width || have.Height != dd.Height || have.Format != dd.Format) {
						g_depthStaging->Release();
						g_depthStaging = nullptr;
					}
				}
				const auto ts = Clock::now();
				if (auto why = PrepareStaging(true); !why.empty()) {
					FailActive(std::move(why), true);
					return;
				}
				job.stageMs = (std::max)(job.stageMs, MsSince(ts));
				const D3D11_BOX cbox{ job.boxX, job.boxY, 0, job.boxX + s, job.boxY + s, 1 };
				ctx->CopySubresourceRegion(g_staging, 0, 0, 0, 0, tex, 0, &cbox);
				ctx->CopyResource(g_depthStaging, dtex);
				g_phase = Phase::kMapping;
				return;
			}

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

		// One read-back step per frame that never waits. The plain capture
		// reads its box in one step; the cutout reads a slice of at most
		// kMapBudget bytes, the color box first, then the depth box.
		void MapStep()
		{
			auto& job = *g_active;
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			auto* ctx = renderer ? reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context) : nullptr;
			if (!ctx) {
				FailActive("no context at Map");
				return;
			}
			const bool          color = !job.cutout || job.colorRows < job.boxS;
			ID3D11Texture2D*    tex = color ? g_staging : g_depthStaging;
			const std::uint32_t total = color ? job.boxS : job.dbs;
			const std::uint32_t rowBytes = color ? job.rowBytes : job.dbs * job.depthBpp;
			const std::size_t   x0 = color ? 0 : static_cast<std::size_t>(job.dbx) * job.depthBpp;
			const std::uint32_t y0 = color ? 0 : job.dby;
			std::uint32_t&      done = color ? job.colorRows : job.depthRows;
			auto&               out = color ? job.raw : job.depthRaw;
			if (!tex || rowBytes == 0) {
				FailActive(std::format("no {} staging at Map", color ? "color" : "depth"));
				return;
			}
			const auto t0 = Clock::now();
			++job.mapTries;
			D3D11_MAPPED_SUBRESOURCE m{};
			const HRESULT hr = ctx->Map(tex, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
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
			if (out.size() != static_cast<std::size_t>(rowBytes) * total) {
				out.resize(static_cast<std::size_t>(rowBytes) * total);
			}
			const std::uint32_t rows = job.cutout ? (std::min)(total - done, (std::max)(1u, static_cast<std::uint32_t>(kMapBudget / rowBytes))) : total;
			const auto*         base = static_cast<const std::uint8_t*>(m.pData);
			for (std::uint32_t y = done; y < done + rows; ++y) {
				std::memcpy(out.data() + static_cast<std::size_t>(y) * rowBytes, base + static_cast<std::size_t>(y0 + y) * m.RowPitch + x0, rowBytes);
			}
			ctx->Unmap(tex, 0);
			done += rows;
			++job.mapSteps;
			job.mapMs = (std::max)(job.mapMs, MsSince(t0));
			if (job.colorRows >= job.boxS && (!job.cutout || job.depthRows >= job.dbs)) {
				job.ok = true;
				EndActive();
			}
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
				} else if (g_phase == Phase::kArmed) {
					auto& job = *g_active;
					++job.armFrames;
					if (auto* player = RE::PlayerCharacter::GetSingleton()) {
						TrackingOff(player);
					}
					if (job.cutout) {
						if (g_light) {
							++job.litFrames;  // the frame this swap ends was drawn with the light
						}
						job.hairReapplied += KeepHair();
					}
					// This frame's camera update carried the pose when our hook wrote;
					// the frame is the pose's when the scene camera is still there at
					// this swap. Off it, something wrote the camera after the pose:
					// SmoothCam is asked for its camera, and the count starts again.
					const std::int32_t writes = g_writes.load(std::memory_order_relaxed);
					const bool         carried = writes > g_lastWrites;
					g_lastWrites = writes;
					float camD = -1.0f, rootD = -1.0f;
					if (auto* nc = RE::Main::WorldRootCamera()) {
						camD = Dist3(nc->world.translate, g_frontPos);
					}
					if (auto* pc = RE::PlayerCamera::GetSingleton(); pc && pc->cameraRoot) {
						rootD = Dist3(pc->cameraRoot->world.translate, g_frontPos);
					}
					const bool atPose = carried && camD >= 0.0f && camD <= kPoseTol;
					job.frames += std::format("{}{}:{}{}", job.frames.empty() ? "" : " ", job.armFrames,
						carried ? std::format("{:.1f}/{:.1f}", camD, rootD) : std::string("-"), Sc::g_owned ? "sc" : "");
					if (carried && !atPose) {
						++job.offFrames;
						job.poseFrames = 0;
						TakeCamera(job);
					}
					if (job.poseFrames + (atPose ? 1 : 0) < kHoldFrames) {
						HoldOpaque();  // for the next frame; the copy frame is read as drawn
					}
					if (atPose) {
						++job.poseFrames;
					}
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
					} else if (job.armFrames >= kHoldFrames + kMaxWaitFrames) {
						Disarm();
						job.code = job.offFrames > 0 ? "camera" : "menu";
						FailActive(std::format("{} of {} camera updates in a row at the pose in {} frames ({} left the camera off it) {} slots=[{}]", job.poseFrames, kHoldFrames,
							job.armFrames, job.offFrames, CameraText(job), job.slots));
					} else if (job.cutout && (!g_staging || !g_depthStaging)) {
						// The stagings, one per armed frame, ahead of the copy.
						const auto ts = Clock::now();
						auto       why = PrepareStaging(false);
						job.stageMs = (std::max)(job.stageMs, MsSince(ts));
						if (!why.empty()) {
							Disarm();
							FailActive(std::move(why), true);
						}
					}
				} else {
					MapStep();
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
		// the natives
		// ---------------------------------------------------------------

		// Every answer, the immediate refusals included, goes back through the
		// game thread and the callback always returns kStarted: the rules of
		// the latent natives in MenuPrompt.cpp, for the same reasons.
		RE::BSScript::LatentStatus FileCapture(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
			RE::BSFixedString a_folder, std::int32_t a_size, bool a_cutout, const std::string& a_key, bool a_trim)
		{
			std::string refused;
			const char* code = "other";
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
					code = "busy";
				} else {
					claimed = true;
					auto job = std::make_shared<Job>();
					job->seq = ++g_seq;
					job->folder = folder;
					job->size = a_size;
					job->t0 = Clock::now();
					if (a_cutout) {
						job->cutout = true;
						job->trim = a_trim;
						job->key = a_key;
						job->span = kCutSpan;
						job->aimUp = kCutAimUp;
					}
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
			spdlog::warn("Portrait: refused: code={} {} - the script gets \"\".", code, refused);
			SetLastFailure(code);
			DispatchToGame([a_vm, a_stackID]() { ReturnToScript(a_vm, a_stackID, std::string()); });
			return RE::BSScript::LatentStatus::kStarted;
		}

		RE::BSScript::LatentStatus CapturePlayerPortrait(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
			RE::StaticFunctionTag*, RE::BSFixedString a_folder, std::int32_t a_size)
		{
			return FileCapture(a_vm, a_stackID, a_folder, a_size, false, std::string(), false);
		}

		RE::BSScript::LatentStatus CapturePlayerPortraitCutout(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
			RE::StaticFunctionTag*, RE::BSFixedString a_folder, std::int32_t a_size, RE::BSFixedString a_key, bool a_trim)
		{
			std::string key;
			try {
				key = SanitizeKey(a_key.c_str() ? a_key.c_str() : "");
			} catch (...) {
				key.clear();
			}
			return FileCapture(a_vm, a_stackID, a_folder, a_size, true, key, a_trim);
		}

		// Not latent: the newest file of the key in the folder, "" when there
		// is none or the folder is not accepted. Reads the two roots; writes
		// nothing.
		RE::BSFixedString GetPlayerPortrait(RE::StaticFunctionTag*, RE::BSFixedString a_folder, RE::BSFixedString a_key)
		{
			try {
				const std::string raw = a_folder.c_str() ? a_folder.c_str() : "";
				const std::string folder = NormalizeFolder(raw);
				if (folder.empty()) {
					spdlog::warn("Portrait: GetPlayerPortrait folder \"{}\" not accepted - \"\".", raw);
					return RE::BSFixedString("");
				}
				const std::string key = SanitizeKey(a_key.c_str() ? a_key.c_str() : "");
				std::int64_t best = -1;
				std::string  name;
				for (const auto& root : Roots(folder)) {
					std::error_code ec;
					for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
						const std::string f = it->path().filename().string();
						const auto        v = NumberOf(f, key, false);
						if (v > best) {
							best = v;
							name = f;
						}
					}
				}
				spdlog::info("Portrait: GetPlayerPortrait folder={} key={} -> \"{}\"", folder, key.empty() ? "(none)" : key, name);
				return RE::BSFixedString(name.c_str());
			} catch (...) {
				spdlog::warn("Portrait: GetPlayerPortrait threw - \"\".");
				return RE::BSFixedString("");
			}
		}

		// Not latent: the code of the last capture that answered "", "" when none
		// has since the game started. Reads one string under a lock; callable
		// from tasklets.
		RE::BSFixedString GetPlayerPortraitFailure(RE::StaticFunctionTag*)
		{
			try {
				std::string code;
				{
					std::scoped_lock l(g_failLock);
					code = g_lastFail;
				}
				return RE::BSFixedString(code.c_str());
			} catch (...) {
				return RE::BSFixedString("");
			}
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
		a_vm->BindNativeMethod(new LatentStringNative<RE::BSFixedString, std::int32_t, RE::BSFixedString, bool>("CapturePlayerPortraitCutout", "Lodestone",
			CapturePlayerPortraitCutout));
		a_vm->RegisterFunction("GetPlayerPortrait", "Lodestone", GetPlayerPortrait);
		a_vm->RegisterFunction("GetPlayerPortraitFailure", "Lodestone", GetPlayerPortraitFailure, true);
		spdlog::info("Portrait: natives registered (CapturePlayerPortrait and CapturePlayerPortraitCutout, latent - the caller waits a few frames; GetPlayerPortrait; GetPlayerPortraitFailure).");
		return true;
	}

	void HandleSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg && (a_msg->type == SKSE::MessagingInterface::kPostLoad || a_msg->type == SKSE::MessagingInterface::kPostPostLoad ||
						 a_msg->type == SKSE::MessagingInterface::kDataLoaded)) {
			if (!REL::Module::IsVR()) {
				Sc::Handle(a_msg->type);
			}
			return;
		}
		if (!a_msg || (a_msg->type != SKSE::MessagingInterface::kPreLoadGame && a_msg->type != SKSE::MessagingInterface::kNewGame)) {
			return;
		}
		// The cutout's light and hair and SmoothCam's camera never cross a load.
		RestoreHair();
		LightOff();
		GiveCameraBack();
		std::optional<Waiter> w;
		{
			std::scoped_lock l(g_waitLock);
			w = g_waiter;
			g_waiter.reset();
		}
		if (w) {
			SetLastFailure("load");
			spdlog::info("Portrait: #{} answered \"\" at once (code=load) - {}.", w->seq,
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
