#pragma once
#include "VROpenXR.h"
#include "VRHaptics.h"
#include "VRInput.h"

#undef GetUserName

class CWeaponClass;
class CXGame;
struct IDirect3DDevice9Ex;
struct IDirect3DTexture9;

// OpenXR (like OpenVR): x = right, y = up, -z = forward; FarCry: x = left, -y = forward, z = up
Matrix34 XrPoseToFarCry(const XrPosef& pose);
XrPosef FarCryToXrPose(const Matrix34& mat);

class VRManager : private IQueueLock
{
public:
	VRManager();
	~VRManager();

	bool Init(CXGame *game);
	void Shutdown();

	void Update();

	void AwaitFrame();
	void HandleEvents();

	void CaptureEye(int eye);
	void CaptureStereo(int eye);
	void CaptureHUD();

	void MirrorEyeToBackBuffer();

	void SetDevice(IDirect3DDevice9Ex *device);
	// Releases all D3DPOOL_DEFAULT resources owned by the VR manager (eye, HUD and stereo textures).
	// Must be called before the device is reset; the textures are recreated lazily on next use.
	void ReleaseDeviceResources();
	void FinishFrame();

	vector2di GetRenderSize() const;

	void ModifyViewCamera(int eye, CCamera& cam);
	void Modify2DCamera(CCamera& cam);
	void Modify3DCamera(int eye, CCamera& cam);
	void ModifyBinocularCamera(IEntityCamera* cam);

	void GetEffectiveRenderLimits(int eye, float* left, float* right, float* top, float* bottom);

	void ProcessInput();
	void ProcessMenuInput();

	bool MousePressed() const { return m_mousePressed; }
	bool MouseReleased() const { return m_mouseReleased; }

	bool UseMotionControllers() const;
	Matrix34 GetControllerTransform(int hand);
	Matrix34 GetHmdTransform() { return m_hmdTransform; }

	void UpdatePlayerTurnOffset(float yawDeltaDeg);
	void UpdatePlayerMoveOffset(const Vec3& offset, const Ang3& hmdAnglesDeg);

	void OnPostPlayerCameraUpdate();
	void CommitYawAndOffsetChanges();

	VRHaptics* GetHaptics() { return &m_vrHaptics; }

	const Vec3& GetBinocularAngles() const { return m_curBinocularAngles; }
	const Vec3& GetBinocularPos() const { return m_curBinocularPos; }

	bool IsDrivingVehicleInCinemaMode();

private:
	struct D3DResources;

	// where the HUD quad (menu, ingame HUD, binoculars, weapon scope) is shown this frame
	struct HudPlacement
	{
		bool headLocked = true;      // pose relative to the head instead of the tracking space
		XrPosef pose;                // quad center, +Z towards the viewer
		float width = 2.0f;          // meters
		bool opaque = false;         // ignore the texture's alpha
	};

	CXGame* m_pGame;
	bool m_initialized = false;
	bool m_inputReady = false;
	D3DResources* m_d3d = nullptr;
	VROpenXR m_xr;
	XrPosef m_headPoseXr;          // raw head pose in the OpenXR stage space, from the last AwaitFrame
	bool m_headPoseValid = false;
	bool m_recalibratePending = false;
	float m_verticalFov;           // tangents of the half angles the eyes are rendered with (symmetric)
	float m_horizontalFov;
	float m_vertRenderScale;
	float m_horzRenderScale;
	bool m_fovKnown = false;
	float m_prevViewYaw = 0;

	int m_curWindowWidth = 0;
	int m_curWindowHeight = 0;

	HudPlacement m_hud;
	bool m_stereoVisible = false;  // show the side-by-side 3D cinema quads (same placement as the HUD)

	// menu pointer: a ray from the pointing hand hits the menu quad and moves the mouse cursor there
	int m_pointerHand = 1;
	bool m_pointerVisible = false;
	bool m_pointerValid = false;
	float m_pointerU = 0.5f;       // cursor on the menu quad, 0..1 from the left / from the top
	float m_pointerV = 0.5f;
	XrVector3f m_pointerHit;       // where the ray meets the menu quad (stage space)
	bool m_menuTriggerDown[2] = {};
	bool m_menuClickDown = false;
	bool m_menuAnyButtonDown = false;
	float m_lastStandaloneFrameTime = 0;   // last frame submitted without VRRenderer (loading screens)

	void SetHudAttachedToHead();
	void SetHudInFrontOfPlayer();
	void SetHudAsBinoculars();
	void SetHudAsWeaponZoom();

	void InitDevice(IDirect3DDevice9Ex* device);
	void CreateEyeTexture(int eye);
	void CreateHUDTexture();
	void CreateStereoTexture();

	// Creates a render target on the game's D3D9 device, in the channel order of the OpenXR swapchains so the
	// frames can be copied over 1:1. The pointer is owned by the caller.
	bool CreateRenderTarget(int width, int height, const char* name, IDirect3DTexture9** ppTexture);

	// Asks dxvk for the Vulkan objects behind its D3D9 device (the OpenXR session is created on them).
	bool QueryVulkanDevice(IDirect3DDevice9Ex* device, VROpenXR::VulkanDevice& vulkanDevice);

	// A texture on its way to OpenXR: the Vulkan image dxvk keeps behind it, moved into the copy source layout
	// (dxvk records the transition into its command stream, so it has to happen before the stream is flushed).
	struct SubmissionImage
	{
		VROpenXR::SourceImage source;
		VkImageLayout originalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		IDirect3DTexture9* texture = nullptr;   // not owned
	};
	bool PrepareTextureForSubmission(IDirect3DTexture9* texture, SubmissionImage& image);
	void PostSubmissionTransitionTexture(SubmissionImage& image);

	// IQueueLock: keeps dxvk off its graphics queue while OpenXR (and our copies) use it
	virtual void LockQueue(bool flushPending);
	virtual void UnlockQueue();

	void UpdateFovFromRuntime();
	void UpdateMenuPointer();
	bool BuildBeamQuad(const XrVector3f& from, const XrVector3f& to, float width, const XrVector3f& eye, VROpenXR::QuadLayer& quad);
	bool BuildFaceQuad(const XrVector3f& center, const XrVector3f& x, const XrVector3f& y, const XrVector3f& normal, float w, float h, const XrVector3f& eye, VROpenXR::QuadLayer& quad);
	int BuildPointerQuads(VROpenXR::QuadLayer* quads, int maxQuads);

public:
	// VR-specific cvars
	float vr_yaw_deadzone_angle;
	int vr_render_force_max_terrain_detail;
	int vr_render_force_obj_draw_dist;
	int vr_enable_motion_controllers;
	int vr_window_width;
	int vr_window_height;
	int vr_mirrored_eye;
	int vr_debug_draw_grip;
	int vr_debug_override_grip;
	float vr_melee_swing_threshold;
	int vr_snap_turn_amount;
	float vr_smooth_turn_speed;
	float vr_button_long_press_time;
	float vr_haptics_effect_strength;
	float vr_weapon_pitch_offset;
	float vr_weapon_yaw_offset;
	int vr_crosshair;
	int vr_movement_dir;
	int vr_show_empty_hands;
	int vr_immersive_ladders;
	int vr_render_world_while_zoomed;
	float vr_binocular_size;
	float vr_scope_size;
	int vr_seated_mode;
	int vr_cutscenes_cinema_mode;
	int vr_vehicles_cinema_mode;
	float vr_hud_distance;
	float vr_hud_width;
	float vr_menu_distance;
	float vr_menu_width;
	int vr_skip_vehicle_transitions;
	int vr_decouple_vehicle_rotations;
	int vr_vehicle_alt_controls;
	int vr_menu_pointer;
	ICVar* vr_debug_override_rh_offset = nullptr;
	ICVar* vr_debug_override_rh_angles = nullptr;
	ICVar* vr_debug_override_lh_offset = nullptr;
	ICVar* e_terrain_lod_ratio = nullptr;
	ICVar* e_detail_texture_min_fov = nullptr;
	ICVar* e_obj_view_dist_ratio = nullptr;

private:
	void RegisterCVars();

	VRInput m_input;
	VRHaptics m_vrHaptics;

	Vec3 m_referencePosition;
	Vec3 m_uncommittedReferencePosition;
	float m_referenceYaw = 0;
	float m_uncommittedReferenceYaw = 0;
	float m_referenceHeight = -1;
	Matrix34 m_hmdTransform;
	bool m_skippedRoomscaleMovement = false;
	bool m_wasInMenu = false;
	bool m_mousePressed = false;
	bool m_mouseReleased = false;
	float m_lastTimeButtonPressed = 0;
	bool m_buttonPressed = false;

	Matrix34 m_fixedHudTransform;
	bool m_fixedPositionInitialized = false;

	Ang3 m_curBinocularAngles;
	Vec3 m_curBinocularPos;
	CCamera m_binocularOriginalPlayerCam;
	bool m_wasBinocular = false;

	void UpdateHmdTransform();
	void ProcessRoomscale();

	void RecalibrateView();
};

extern VRManager* gVR;
