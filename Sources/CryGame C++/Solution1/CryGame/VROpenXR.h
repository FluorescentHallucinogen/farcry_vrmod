#pragma once

// OpenXR runtime access for the Far Cry VR mod.
//
// Far Cry renders with Direct3D 9, which dxvk translates to Vulkan. OpenXR has no D3D9 binding, but it
// has a Vulkan one, so the frames travel like this:
//   D3D9 render target (eye / HUD / stereo texture, filled by the game)
//     -> its Vulkan image, borrowed from dxvk (VRManager does that through dxvk's interop exports)
//     -> copied with a small command buffer, on dxvk's own graphics queue, into an OpenXR swapchain
//        image (XR_KHR_vulkan_enable, the session runs on dxvk's VkInstance / VkDevice / VkQueue)
//     -> submitted as a projection layer (eyes) or quad layers (HUD, menu, 3D cinema, pointer ray).
//
// The queue belongs to dxvk, which submits to it from its own thread. Everything here that may touch the
// queue - our copies, but also the runtime inside xrBeginFrame / xrAcquire- / xrWait- / xrReleaseSwapchainImage
// / xrEndFrame - therefore runs while dxvk's submission queue is locked (IQueueLock, implemented by VRManager
// on top of dxvk's exports).
//
// This class owns the instance, session, spaces, actions and swapchains and knows nothing about the game or
// about D3D9; VRManager and VRInput sit on top of it.

#include <windows.h>
#include <string>
#include <vector>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES   // the entry points are fetched from vulkan-1.dll at runtime (see VROpenXR::LoadVulkan)
#endif
#include <vulkan/vulkan.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_VULKAN
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

// The Vulkan queue the OpenXR session runs on is dxvk's, and dxvk submits to it from its own thread. Whoever
// hands the queue to VROpenXR has to be able to keep dxvk off it for a while: LockQueue blocks until dxvk has
// submitted everything it had queued up (and, with flushPending, first makes dxvk queue up all the D3D9 work
// recorded so far) and then holds dxvk's submission lock until UnlockQueue. Not reentrant, VROpenXR takes care
// of that.
class IQueueLock
{
public:
	virtual ~IQueueLock() {}
	virtual void LockQueue(bool flushPending) = 0;
	virtual void UnlockQueue() = 0;
};

class VROpenXR
{
public:
	enum { Hand_Left = 0, Hand_Right = 1 };

	// the Vulkan objects the game renders with; the OpenXR session is created on them
	struct VulkanDevice
	{
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		unsigned queueFamilyIndex = 0;
		unsigned queueIndex = 0;
	};

	// an image rendered by the game, ready to be copied into a swapchain: single mip and layer, in
	// VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, with all writes to it already submitted to the queue
	struct SourceImage
	{
		VkImage image = VK_NULL_HANDLE;
		VkFormat format = VK_FORMAT_UNDEFINED;
		unsigned width = 0;
		unsigned height = 0;
	};

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
		std::vector<VkImage> images;   // owned by the runtime, valid while the swapchain lives
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

	// device: dxvk's Vulkan objects; queueLock: keeps dxvk off the queue while OpenXR uses it (see IQueueLock).
	// Both have to stay valid until Shutdown.
	bool Init(const VulkanDevice& device, IQueueLock* queueLock);
	void Shutdown();
	bool IsInitialized() const { return m_session != XR_NULL_HANDLE; }

	// the format the swapchains are created with; the game's render targets should use the same channel order
	// so the frames can be copied over 1:1 (sRGB vs UNORM does not matter for the copy)
	VkFormat GetSwapchainFormat() const { return m_swapchainFormat; }

	// --- queue ownership --------------------------------------------------------------------------
	// Every call below that may touch the Vulkan queue (BeginFrame, CopyToSwapchain, FillSwapchain, EndFrame,
	// Create-/DestroySwapchain) locks it by itself. A caller that makes several of them in a row (VRManager's
	// FinishFrame) wraps them in one LockQueue/UnlockQueue pair instead, so dxvk is stopped only once per frame;
	// the calls are reentrant and only the outermost lock really takes dxvk's submission lock.
	void LockQueue(bool flushPending);
	void UnlockQueue();

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
	// copies the whole source image into the swapchain's next image; the source has to match the swapchain's
	// size and be in TRANSFER_SRC_OPTIMAL layout (see SourceImage)
	bool CopyToSwapchain(Swapchain& swapchain, const SourceImage& source);
	// fills the swapchain's next image with one color, given as B8G8R8A8 (0xAARRGGBB) in sRGB encoding
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
	// the Vulkan entry points this class uses, resolved against dxvk's instance and device
	struct VulkanFunctions
	{
		PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
		PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
		PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
		PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
		PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
		PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
		PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
		PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
		PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
		PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
		PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
		PFN_vkCmdCopyImage vkCmdCopyImage = nullptr;
		PFN_vkCmdBlitImage vkCmdBlitImage = nullptr;
		PFN_vkCmdClearColorImage vkCmdClearColorImage = nullptr;
		PFN_vkQueueSubmit vkQueueSubmit = nullptr;
		PFN_vkCreateFence vkCreateFence = nullptr;
		PFN_vkDestroyFence vkDestroyFence = nullptr;
		PFN_vkWaitForFences vkWaitForFences = nullptr;
		PFN_vkResetFences vkResetFences = nullptr;
	};

	// a command buffer with the fence that tells when the GPU is done with it; reused round robin
	struct CommandSlot
	{
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		bool pending = false;
	};
	enum { kCommandSlots = 8 };

	bool Check(XrResult result, const char* what) const;
	XrPath Path(const char* path) const;
	bool HasExtension(const char* name) const;
	bool LoadVulkan();
	bool CheckVulkanRequirements();
	bool CreateCommandBuffers();
	void DestroyCommandBuffers();
	bool CreateActions();
	void SuggestBindings(const char* profile, const BindingDef* defs, int count);
	void SyncInput();
	void LocateFrame();
	bool LocateHand(XrSpace space, XrTime time, XrPosef& pose) const;
	bool WarmUp();
	bool AcquireImage(Swapchain& swapchain, unsigned& index);
	void ReleaseImage(Swapchain& swapchain);
	// takes a free command slot (waiting for the GPU if all are in flight) and starts recording into it
	CommandSlot* BeginCommands();
	// ends recording and submits the slot to the queue; the queue has to be locked
	bool SubmitCommands(CommandSlot* slot);
	void RecordImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
		VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);
	static bool SameChannelOrder(VkFormat a, VkFormat b);
	static bool IsSrgbFormat(VkFormat format);

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

	VulkanDevice m_vk;
	VulkanFunctions m_fn;
	HMODULE m_vulkanLibrary = nullptr;
	IQueueLock* m_queueLock = nullptr;
	int m_queueLockDepth = 0;
	VkCommandPool m_commandPool = VK_NULL_HANDLE;
	CommandSlot m_commandSlots[kCommandSlots];
	int m_nextCommandSlot = 0;
	VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
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
