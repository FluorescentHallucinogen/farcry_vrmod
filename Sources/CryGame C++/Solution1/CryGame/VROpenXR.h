#pragma once

// OpenXR runtime access for the Far Cry VR mod.
//
// Far Cry renders with Direct3D 9 (through the Far Cry VR d3d9.dll proxy it is a D3D9Ex device),
// OpenXR has no D3D9 binding, so the frames travel like this:
//   D3D9Ex shared render target (eye / HUD / stereo texture, filled by the game)
//     -> opened on the D3D11 device created here (OpenSharedResource)
//     -> copied into an OpenXR swapchain (XR_KHR_D3D11_enable)
//     -> submitted as a projection layer (eyes) or quad layers (HUD, menu, 3D cinema, pointer ray).
//
// This class owns the instance, session, spaces, actions and swapchains and knows nothing about
// the game; VRManager and VRInput sit on top of it.

#include <windows.h>
#include <d3d11.h>
#include <string>
#include <vector>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#undef GetUserName

namespace xrmath
{
	XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b);
	XrQuaternionf Conjugate(const XrQuaternionf& q);
	XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v);
	XrVector3f Forward(const XrQuaternionf& q);   // q * (0, 0, -1)
	XrVector3f Right(const XrQuaternionf& q);     // q * (1, 0, 0)
	XrVector3f Up(const XrQuaternionf& q);        // q * (0, 1, 0)
	XrPosef Identity();
	XrPosef Inverse(const XrPosef& p);
	XrPosef Compose(const XrPosef& parent, const XrPosef& child);   // parent * child
	// column basis (x, y, z axes of the local frame expressed in the parent frame) -> quaternion
	XrQuaternionf FromAxes(const XrVector3f& x, const XrVector3f& y, const XrVector3f& z);
	void ToAxes(const XrQuaternionf& q, XrVector3f& x, XrVector3f& y, XrVector3f& z);
	float Length(const XrVector3f& v);
	XrVector3f Normalize(const XrVector3f& v);
	XrVector3f Cross(const XrVector3f& a, const XrVector3f& b);
	float Dot(const XrVector3f& a, const XrVector3f& b);
	XrVector3f Add(const XrVector3f& a, const XrVector3f& b);
	XrVector3f Sub(const XrVector3f& a, const XrVector3f& b);
	XrVector3f Scale(const XrVector3f& v, float s);
}

class VROpenXR
{
public:
	enum { Hand_Left = 0, Hand_Right = 1 };

	// Controller state after the last BeginFrame. All controllers are reduced to this common
	// vocabulary; the per-profile bindings in VROpenXR.cpp decide which physical control feeds what.
	struct HandState
	{
		bool active = false;       // the controller is connected and delivered input this frame
		float trigger = 0;         // 0..1
		float squeeze = 0;         // 0..1 (grip)
		float stickX = 0;          // thumbstick (trackpad on controllers without a stick), -1..1
		float stickY = 0;
		bool stickClick = false;
		bool padClick = false;     // trackpad click/force on controllers that have both a stick and a pad (Index)
		bool primary = false;      // X / A
		bool secondary = false;    // Y / B (menu button on controllers without face buttons)
	};

	struct Swapchain
	{
		XrSwapchain handle = XR_NULL_HANDLE;
		std::vector<ID3D11Texture2D*> images;   // owned by the runtime, valid while the swapchain lives
		unsigned width = 0;
		unsigned height = 0;
	};

	struct ProjectionView
	{
		const Swapchain* swapchain = nullptr;
		XrRect2Di rect = {};       // the part of the image that covers the eye's real (asymmetric) fov
		XrFovf fov = {};           // the field of view that rect covers (angles, radians)
	};

	struct QuadLayer
	{
		const Swapchain* swapchain = nullptr;
		XrRect2Di rect = {};       // the part of the image to show
		bool headLocked = false;   // pose relative to the head (VIEW space) instead of the stage
		XrPosef pose = {};         // quad center; +Z of the pose faces the viewer
		float width = 1;           // meters
		float height = 1;
		bool alphaBlend = false;   // blend with the texture's (premultiplied) alpha, else opaque
		XrEyeVisibility eye = XR_EYE_VISIBILITY_BOTH;
	};

	VROpenXR();
	// does not shut down: VRManager is a static object and by the time it dies the runtime may be gone
	~VROpenXR() {}

	bool Init();
	void Shutdown();
	bool IsInitialized() const { return m_session != XR_NULL_HANDLE; }

	ID3D11Device* GetDevice11() const { return m_device11; }
	ID3D11DeviceContext* GetContext11() const { return m_context11; }

	// --- session events ---------------------------------------------------------------------------
	void PollEvents();
	bool IsSessionRunning() const { return m_sessionRunning; }
	bool TakeQuitRequest();     // true once when the runtime wants the application to exit
	bool TakeFocusLost();       // true once when the session went from focused to merely visible (system menu)
	bool TakeRecenter();        // true once when the runtime re-anchored the tracking space

	// --- frame loop -------------------------------------------------------------------------------
	// Waits for the runtime's frame timing, syncs input and locates head, eyes and hands. Returns false
	// when the session is not running (nothing must be submitted then).
	bool BeginFrame();
	bool IsFrameOpen() const { return m_frameOpen; }
	bool ShouldRender() const { return m_frameOpen && m_shouldRender; }
	// Ends the frame started by BeginFrame. projection: both eyes or nullptr; quads: drawn in order.
	void EndFrame(const ProjectionView* projection, const QuadLayer* quads, int quadCount);

	// --- tracking (stage space, meters; valid after BeginFrame) -----------------------------------
	bool GetHeadPose(XrPosef& pose) const;
	bool GetEyePose(int eye, XrPosef& pose) const;
	bool GetEyeFov(int eye, XrFovf& fov) const;   // false until the runtime told us the fov
	bool GetHandPose(int hand, bool aim, XrPosef& pose) const;
	void GetRecommendedEyeSize(unsigned& width, unsigned& height) const;

	// --- input (valid after BeginFrame) -----------------------------------------------------------
	bool HasInput() const { return m_inputAttached; }   // controller actions are set up
	const HandState& GetHand(int hand) const { return m_hands[hand < 0 ? 0 : (hand > 1 ? 1 : hand)]; }
	// amplitude 0..1 (<= 0 stops the motor), frequency in Hz (0: whatever the controller likes),
	// seconds: how long the vibration lasts
	void ApplyHaptic(int hand, float amplitude, float frequency, float seconds);

	// --- swapchains -------------------------------------------------------------------------------
	bool CreateSwapchain(Swapchain& swapchain, unsigned width, unsigned height);
	void DestroySwapchain(Swapchain& swapchain);
	bool CopyToSwapchain(Swapchain& swapchain, ID3D11Texture2D* source);
	bool FillSwapchain(Swapchain& swapchain, unsigned colorBGRA);

	// the abstract actions every controller profile is mapped onto (tables in VROpenXR.cpp)
	enum ActionId
	{
		Action_GripPose,
		Action_AimPose,
		Action_Haptic,
		Action_Trigger,
		Action_Squeeze,
		Action_Stick,
		Action_StickClick,
		Action_PadClick,
		Action_Primary,
		Action_Secondary,
		Action_Count
	};

	struct BindingDef
	{
		ActionId action;
		const char* path;
	};

private:
	bool Check(XrResult result, const char* what) const;
	XrPath Path(const char* path) const;
	bool HasExtension(const char* name) const;
	bool CreateDevice11();
	bool CreateActions();
	void SuggestBindings(const char* profile, const BindingDef* defs, int count);
	void SyncInput();
	void LocateFrame();
	bool LocateHand(XrSpace space, XrTime time, XrPosef& pose) const;
	bool WarmUp();
	bool AcquireImage(Swapchain& swapchain, unsigned& index);
	void ReleaseImage(Swapchain& swapchain);

	XrInstance m_instance = XR_NULL_HANDLE;
	XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
	XrSession m_session = XR_NULL_HANDLE;
	XrSpace m_stageSpace = XR_NULL_HANDLE;
	XrSpace m_viewSpace = XR_NULL_HANDLE;
	XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;
	bool m_sessionRunning = false;
	bool m_quitRequested = false;
	bool m_focusLost = false;
	bool m_recenter = false;

	std::vector<std::string> m_enabledExtensions;

	ID3D11Device* m_device11 = nullptr;
	ID3D11DeviceContext* m_context11 = nullptr;
	LUID m_adapterLuid = {};
	DXGI_FORMAT m_swapchainFormat = DXGI_FORMAT_UNKNOWN;
	XrViewConfigurationView m_configViews[2] = {};

	XrFrameState m_frameState = {};
	bool m_frameOpen = false;
	bool m_shouldRender = false;
	XrView m_views[2] = {};
	bool m_viewsValid = false;
	bool m_fovKnown = false;
	XrPosef m_headPose = {};
	bool m_headValid = false;

	XrActionSet m_actionSet = XR_NULL_HANDLE;
	XrAction m_actions[Action_Count] = {};
	XrPath m_handPath[2] = {};
	XrSpace m_gripSpace[2] = {};
	XrSpace m_aimSpace[2] = {};
	XrPosef m_gripPose[2] = {};
	XrPosef m_aimPose[2] = {};
	bool m_gripValid[2] = {};
	bool m_aimValid[2] = {};
	HandState m_hands[2];
	bool m_inputAttached = false;
	float m_lastHapticAmplitude[2] = {};
};
