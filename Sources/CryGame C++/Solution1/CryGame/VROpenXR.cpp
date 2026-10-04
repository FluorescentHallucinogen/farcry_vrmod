#include "StdAfx.h"
#include "VROpenXR.h"

#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------------------------
// small pose math (OpenXR convention: x right, y up, -z forward, meters)
// ---------------------------------------------------------------------------------------------

namespace xrmath
{
	XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b)
	{
		XrQuaternionf q;
		q.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
		q.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
		q.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
		q.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
		return q;
	}

	XrQuaternionf Conjugate(const XrQuaternionf& q)
	{
		XrQuaternionf c = { -q.x, -q.y, -q.z, q.w };
		return c;
	}

	XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v)
	{
		// v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
		XrVector3f u = { q.x, q.y, q.z };
		XrVector3f t = Scale(Cross(u, Add(Cross(u, v), Scale(v, q.w))), 2.0f);
		return Add(v, t);
	}

	XrVector3f Forward(const XrQuaternionf& q)
	{
		XrVector3f f = { 0, 0, -1 };
		return Rotate(q, f);
	}

	XrVector3f Right(const XrQuaternionf& q)
	{
		XrVector3f r = { 1, 0, 0 };
		return Rotate(q, r);
	}

	XrVector3f Up(const XrQuaternionf& q)
	{
		XrVector3f u = { 0, 1, 0 };
		return Rotate(q, u);
	}

	XrPosef Identity()
	{
		XrPosef p;
		p.orientation.x = p.orientation.y = p.orientation.z = 0;
		p.orientation.w = 1;
		p.position.x = p.position.y = p.position.z = 0;
		return p;
	}

	XrPosef Inverse(const XrPosef& p)
	{
		XrPosef r;
		r.orientation = Conjugate(p.orientation);
		r.position = Scale(Rotate(r.orientation, p.position), -1.0f);
		return r;
	}

	XrPosef Compose(const XrPosef& parent, const XrPosef& child)
	{
		XrPosef r;
		r.orientation = Multiply(parent.orientation, child.orientation);
		r.position = Add(parent.position, Rotate(parent.orientation, child.position));
		return r;
	}

	XrQuaternionf FromAxes(const XrVector3f& x, const XrVector3f& y, const XrVector3f& z)
	{
		// rotation matrix with the axes as columns (Shepperd's method)
		const float m00 = x.x, m01 = y.x, m02 = z.x;
		const float m10 = x.y, m11 = y.y, m12 = z.y;
		const float m20 = x.z, m21 = y.z, m22 = z.z;

		XrQuaternionf q;
		const float trace = m00 + m11 + m22;
		if (trace > 0.0f)
		{
			float s = sqrtf(trace + 1.0f) * 2.0f;
			q.w = 0.25f * s;
			q.x = (m21 - m12) / s;
			q.y = (m02 - m20) / s;
			q.z = (m10 - m01) / s;
		}
		else if (m00 > m11 && m00 > m22)
		{
			float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
			q.w = (m21 - m12) / s;
			q.x = 0.25f * s;
			q.y = (m01 + m10) / s;
			q.z = (m02 + m20) / s;
		}
		else if (m11 > m22)
		{
			float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
			q.w = (m02 - m20) / s;
			q.x = (m01 + m10) / s;
			q.y = 0.25f * s;
			q.z = (m12 + m21) / s;
		}
		else
		{
			float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
			q.w = (m10 - m01) / s;
			q.x = (m02 + m20) / s;
			q.y = (m12 + m21) / s;
			q.z = 0.25f * s;
		}
		return q;
	}

	void ToAxes(const XrQuaternionf& q, XrVector3f& x, XrVector3f& y, XrVector3f& z)
	{
		x = Right(q);
		y = Up(q);
		XrVector3f back = { 0, 0, 1 };
		z = Rotate(q, back);
	}

	float Length(const XrVector3f& v)
	{
		return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	}

	XrVector3f Normalize(const XrVector3f& v)
	{
		float len = Length(v);
		if (len < 1e-6f)
		{
			XrVector3f zero = { 0, 0, 0 };
			return zero;
		}
		return Scale(v, 1.0f / len);
	}

	XrVector3f Cross(const XrVector3f& a, const XrVector3f& b)
	{
		XrVector3f r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
		return r;
	}

	float Dot(const XrVector3f& a, const XrVector3f& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	XrVector3f Add(const XrVector3f& a, const XrVector3f& b)
	{
		XrVector3f r = { a.x + b.x, a.y + b.y, a.z + b.z };
		return r;
	}

	XrVector3f Sub(const XrVector3f& a, const XrVector3f& b)
	{
		XrVector3f r = { a.x - b.x, a.y - b.y, a.z - b.z };
		return r;
	}

	XrVector3f Scale(const XrVector3f& v, float s)
	{
		XrVector3f r = { v.x * s, v.y * s, v.z * s };
		return r;
	}
}

// ---------------------------------------------------------------------------------------------

namespace
{
	// controller profiles that only exist behind an extension; used when the runtime offers them
	const char* const kOptionalExtensions[] =
	{
		XR_EXT_LOCAL_FLOOR_EXTENSION_NAME,
		"XR_EXT_hp_mixed_reality_controller",
		"XR_HTC_vive_cosmos_controller_interaction",
		"XR_HTC_vive_focus3_controller_interaction",
		"XR_BD_controller_interaction",
	};

	const int kMaxLayers = 16;

	const char* SessionStateName(XrSessionState state)
	{
		switch (state)
		{
		case XR_SESSION_STATE_IDLE: return "idle";
		case XR_SESSION_STATE_READY: return "ready";
		case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
		case XR_SESSION_STATE_VISIBLE: return "visible";
		case XR_SESSION_STATE_FOCUSED: return "focused";
		case XR_SESSION_STATE_STOPPING: return "stopping";
		case XR_SESSION_STATE_LOSS_PENDING: return "loss pending";
		case XR_SESSION_STATE_EXITING: return "exiting";
		default: return "unknown";
		}
	}

	template <typename T>
	void Zero(T& value, XrStructureType type)
	{
		memset(&value, 0, sizeof(T));
		value.type = type;
	}

	// splits the space separated extension list OpenXR hands out
	void SplitExtensionList(const std::string& list, std::vector<std::string>& names)
	{
		size_t start = 0;
		while (start < list.size())
		{
			size_t end = list.find(' ', start);
			if (end == std::string::npos)
				end = list.size();
			if (end > start)
				names.push_back(list.substr(start, end - start));
			start = end + 1;
		}
	}

	// sRGB encoded 8 bit channel -> linear float
	float SrgbToLinear(unsigned byte)
	{
		float c = byte / 255.0f;
		return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
	}

	// holds VROpenXR's queue lock for the duration of a scope
	class ScopedQueueLock
	{
	public:
		ScopedQueueLock(VROpenXR& xr, bool flushPending) : m_xr(xr) { m_xr.LockQueue(flushPending); }
		~ScopedQueueLock() { m_xr.UnlockQueue(); }
	private:
		VROpenXR& m_xr;
	};
}

VROpenXR::VROpenXR()
{
	m_headPose = xrmath::Identity();
	for (int i = 0; i < 2; ++i)
	{
		m_gripPose[i] = xrmath::Identity();
		m_aimPose[i] = xrmath::Identity();
	}
}

// ---------------------------------------------------------------------------------------------
// queue ownership
// ---------------------------------------------------------------------------------------------

void VROpenXR::LockQueue(bool flushPending)
{
	if (m_queueLockDepth++ == 0 && m_queueLock)
		m_queueLock->LockQueue(flushPending);
}

void VROpenXR::UnlockQueue()
{
	if (m_queueLockDepth <= 0)
		return;
	if (--m_queueLockDepth == 0 && m_queueLock)
		m_queueLock->UnlockQueue();
}

bool VROpenXR::Check(XrResult result, const char* what) const
{
	if (XR_SUCCEEDED(result))
		return true;

	char name[XR_MAX_RESULT_STRING_SIZE] = {};
	if (m_instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(m_instance, result, name)))
		CryLogAlways("OpenXR: %s failed: %s", what, name);
	else
		CryLogAlways("OpenXR: %s failed: %d", what, (int)result);
	return false;
}

XrPath VROpenXR::Path(const char* path) const
{
	XrPath result = XR_NULL_PATH;
	if (m_instance != XR_NULL_HANDLE)
		xrStringToPath(m_instance, path, &result);
	return result;
}

bool VROpenXR::HasExtension(const char* name) const
{
	for (size_t i = 0; i < m_enabledExtensions.size(); ++i)
	{
		if (m_enabledExtensions[i] == name)
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------------------------

bool VROpenXR::Init(const VulkanDevice& device, IQueueLock* queueLock)
{
	if (m_instance != XR_NULL_HANDLE)
		return true;

	if (device.instance == VK_NULL_HANDLE || device.physicalDevice == VK_NULL_HANDLE || device.device == VK_NULL_HANDLE || device.queue == VK_NULL_HANDLE)
	{
		CryLogAlways("OpenXR: no Vulkan device to run the session on (is the game rendering through dxvk?)");
		return false;
	}
	m_vk = device;
	m_queueLock = queueLock;

	if (!LoadVulkan())
	{
		Shutdown();
		return false;
	}

	// --- extensions ------------------------------------------------------------------------------

	unsigned extensionCount = 0;
	std::vector<XrExtensionProperties> available;
	if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr)) && extensionCount > 0)
	{
		available.resize(extensionCount);
		for (unsigned i = 0; i < extensionCount; ++i)
			Zero(available[i], XR_TYPE_EXTENSION_PROPERTIES);
		if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, &available[0])))
			available.clear();
	}

	bool haveVulkan = false;
	for (size_t i = 0; i < available.size(); ++i)
	{
		if (strcmp(available[i].extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0)
			haveVulkan = true;
		for (size_t j = 0; j < sizeof(kOptionalExtensions) / sizeof(kOptionalExtensions[0]); ++j)
		{
			if (strcmp(available[i].extensionName, kOptionalExtensions[j]) == 0)
				m_enabledExtensions.push_back(kOptionalExtensions[j]);
		}
	}
	if (!haveVulkan)
	{
		CryLogAlways("OpenXR: the runtime does not support XR_KHR_vulkan_enable (or no runtime is installed); dxvk renders with Vulkan, so a Vulkan capable runtime is required");
		Shutdown();
		return false;
	}
	m_enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);

	std::vector<const char*> extensionNames;
	for (size_t i = 0; i < m_enabledExtensions.size(); ++i)
		extensionNames.push_back(m_enabledExtensions[i].c_str());

	// --- instance --------------------------------------------------------------------------------

	XrInstanceCreateInfo instanceInfo;
	Zero(instanceInfo, XR_TYPE_INSTANCE_CREATE_INFO);
	strcpy(instanceInfo.applicationInfo.applicationName, "Far Cry VR");
	instanceInfo.applicationInfo.applicationVersion = 1;
	strcpy(instanceInfo.applicationInfo.engineName, "CryEngine 1");
	instanceInfo.applicationInfo.engineVersion = 1;
	// ask for OpenXR 1.0: some runtimes still reject a 1.1 instance
	instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	instanceInfo.enabledExtensionCount = (uint32_t)extensionNames.size();
	instanceInfo.enabledExtensionNames = &extensionNames[0];

	if (!Check(xrCreateInstance(&instanceInfo, &m_instance), "xrCreateInstance"))
	{
		m_instance = XR_NULL_HANDLE;
		Shutdown();
		return false;
	}

	XrInstanceProperties instanceProps;
	Zero(instanceProps, XR_TYPE_INSTANCE_PROPERTIES);
	if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &instanceProps)))
	{
		CryLogAlways("OpenXR: runtime %s %u.%u.%u", instanceProps.runtimeName,
			XR_VERSION_MAJOR(instanceProps.runtimeVersion), XR_VERSION_MINOR(instanceProps.runtimeVersion), XR_VERSION_PATCH(instanceProps.runtimeVersion));
	}

	// --- system ----------------------------------------------------------------------------------

	XrSystemGetInfo systemInfo;
	Zero(systemInfo, XR_TYPE_SYSTEM_GET_INFO);
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!Check(xrGetSystem(m_instance, &systemInfo, &m_systemId), "xrGetSystem (is the headset connected?)"))
	{
		Shutdown();
		return false;
	}

	XrSystemProperties systemProps;
	Zero(systemProps, XR_TYPE_SYSTEM_PROPERTIES);
	if (XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_systemId, &systemProps)))
		CryLogAlways("OpenXR: system %s, max layers %u", systemProps.systemName, systemProps.graphicsProperties.maxLayerCount);

	unsigned viewCount = 0;
	for (int i = 0; i < 2; ++i)
		Zero(m_configViews[i], XR_TYPE_VIEW_CONFIGURATION_VIEW);
	if (!Check(xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, m_configViews), "xrEnumerateViewConfigurationViews")
		|| viewCount != 2)
	{
		CryLogAlways("OpenXR: the system does not offer a stereo view configuration");
		Shutdown();
		return false;
	}
	CryLogAlways("OpenXR: recommended eye size %u x %u (max %u x %u)",
		m_configViews[0].recommendedImageRectWidth, m_configViews[0].recommendedImageRectHeight,
		m_configViews[0].maxImageRectWidth, m_configViews[0].maxImageRectHeight);

	// --- the Vulkan device: dxvk's, checked against what the runtime wants --------------------------

	if (!CheckVulkanRequirements())
	{
		Shutdown();
		return false;
	}
	if (!CreateCommandBuffers())
	{
		Shutdown();
		return false;
	}

	// --- session ---------------------------------------------------------------------------------

	XrGraphicsBindingVulkanKHR binding;
	Zero(binding, XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR);
	binding.instance = m_vk.instance;
	binding.physicalDevice = m_vk.physicalDevice;
	binding.device = m_vk.device;
	binding.queueFamilyIndex = m_vk.queueFamilyIndex;
	binding.queueIndex = m_vk.queueIndex;

	XrSessionCreateInfo sessionInfo;
	Zero(sessionInfo, XR_TYPE_SESSION_CREATE_INFO);
	sessionInfo.next = &binding;
	sessionInfo.systemId = m_systemId;
	XrResult sessionResult;
	{
		// the runtime may well touch the queue while it sets the session up
		ScopedQueueLock lock(*this, false);
		sessionResult = xrCreateSession(m_instance, &sessionInfo, &m_session);
	}
	if (!Check(sessionResult, "xrCreateSession"))
	{
		m_session = XR_NULL_HANDLE;
		CryLogAlways("OpenXR: the session could not be created on dxvk's Vulkan device; if the runtime logged missing device extensions, dxvk did not enable them (see the extension lists above)");
		Shutdown();
		return false;
	}

	// --- swapchain format: the D3D9 render targets are A8R8G8B8 (B8G8R8A8 in Vulkan), and a plain image copy
	// needs the same channel order, so prefer BGRA; with an RGBA-only runtime VRManager creates its render
	// targets as A8B8G8R8 instead (see GetSwapchainFormat) ----------------------------------------

	unsigned formatCount = 0;
	std::vector<int64_t> formats;
	if (XR_SUCCEEDED(xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr)) && formatCount > 0)
	{
		formats.resize(formatCount);
		if (XR_FAILED(xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, &formats[0])))
			formats.clear();
	}
	std::string formatList;
	for (size_t f = 0; f < formats.size(); ++f)
	{
		char buf[16];
		sprintf(buf, "%s%d", f ? " " : "", (int)formats[f]);
		formatList += buf;
	}
	CryLogAlways("OpenXR: swapchain formats offered (VkFormat): %s", formatList.c_str());

	const VkFormat preferred[] = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM };
	m_swapchainFormat = VK_FORMAT_UNDEFINED;
	for (int p = 0; p < 4 && m_swapchainFormat == VK_FORMAT_UNDEFINED; ++p)
	{
		for (size_t f = 0; f < formats.size(); ++f)
		{
			if (formats[f] == (int64_t)preferred[p])
			{
				m_swapchainFormat = preferred[p];
				break;
			}
		}
	}
	if (m_swapchainFormat == VK_FORMAT_UNDEFINED)
	{
		CryLogAlways("OpenXR: the runtime offers no 8 bit RGBA/BGRA swapchain format, the D3D9 frames cannot be handed over");
		Shutdown();
		return false;
	}
	if (!IsSrgbFormat(m_swapchainFormat))
		CryLogAlways("OpenXR: WARNING: no sRGB swapchain format offered, the picture may look washed out");

	// --- reference spaces ------------------------------------------------------------------------

	unsigned spaceCount = 0;
	std::vector<XrReferenceSpaceType> spaces;
	if (XR_SUCCEEDED(xrEnumerateReferenceSpaces(m_session, 0, &spaceCount, nullptr)) && spaceCount > 0)
	{
		spaces.resize(spaceCount);
		if (XR_FAILED(xrEnumerateReferenceSpaces(m_session, spaceCount, &spaceCount, &spaces[0])))
			spaces.clear();
	}
	bool haveStage = false;
	bool haveLocalFloor = false;
	for (size_t i = 0; i < spaces.size(); ++i)
	{
		if (spaces[i] == XR_REFERENCE_SPACE_TYPE_STAGE)
			haveStage = true;
		if (spaces[i] == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT && HasExtension(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME))
			haveLocalFloor = true;
	}

	// the mod measures the player's height as the head's height above the space's origin, so the origin
	// has to be on the floor: STAGE, else LOCAL_FLOOR, else LOCAL (origin at eye level) moved down by a
	// typical eye height
	XrReferenceSpaceCreateInfo spaceInfo;
	Zero(spaceInfo, XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
	spaceInfo.poseInReferenceSpace = xrmath::Identity();
	const char* spaceName = "stage";
	if (haveStage)
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	else if (haveLocalFloor)
	{
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
		spaceName = "local floor (no stage space offered)";
	}
	else
	{
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		spaceInfo.poseInReferenceSpace.position.y = -1.65f;
		spaceName = "local (no stage space offered, the floor is assumed 1.65 m below the head)";
	}
	if (!Check(xrCreateReferenceSpace(m_session, &spaceInfo, &m_stageSpace), "xrCreateReferenceSpace(stage)"))
	{
		m_stageSpace = XR_NULL_HANDLE;
		Shutdown();
		return false;
	}
	CryLogAlways("OpenXR: tracking space: %s", spaceName);

	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (!Check(xrCreateReferenceSpace(m_session, &spaceInfo, &m_viewSpace), "xrCreateReferenceSpace(view)"))
	{
		m_viewSpace = XR_NULL_HANDLE;
		Shutdown();
		return false;
	}

	// --- controllers (not fatal: the game stays playable with keyboard and mouse) ----------------

	if (!CreateActions())
	{
		CryLogAlways("OpenXR: controller input could not be set up, motion controls are disabled");
		if (m_actionSet != XR_NULL_HANDLE)
		{
			xrDestroyActionSet(m_actionSet);
			m_actionSet = XR_NULL_HANDLE;
		}
		for (int i = 0; i < Action_Count; ++i)
			m_actions[i] = XR_NULL_HANDLE;
		m_inputAttached = false;
	}

	// --- get the session going and learn the field of view ---------------------------------------

	WarmUp();

	CryLogAlways("OpenXR: initialized (swapchain format %d)", (int)m_swapchainFormat);
	return true;
}

// ---------------------------------------------------------------------------------------------
// Vulkan: the device is dxvk's, we only borrow it
// ---------------------------------------------------------------------------------------------

bool VROpenXR::LoadVulkan()
{
	// dxvk talks to vulkan-1.dll as well, so the handles it gave us are the loader's
	if (!m_vulkanLibrary)
		m_vulkanLibrary = LoadLibraryA("vulkan-1.dll");
	if (!m_vulkanLibrary)
	{
		CryLogAlways("OpenXR: vulkan-1.dll could not be loaded");
		return false;
	}

	m_fn.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(m_vulkanLibrary, "vkGetInstanceProcAddr");
	if (!m_fn.vkGetInstanceProcAddr)
	{
		CryLogAlways("OpenXR: vkGetInstanceProcAddr not found in vulkan-1.dll");
		return false;
	}

#define LOAD_INSTANCE_FN(name) m_fn.name = (PFN_##name)m_fn.vkGetInstanceProcAddr(m_vk.instance, #name)
#define LOAD_DEVICE_FN(name) m_fn.name = (PFN_##name)m_fn.vkGetDeviceProcAddr(m_vk.device, #name)
	LOAD_INSTANCE_FN(vkGetDeviceProcAddr);
	LOAD_INSTANCE_FN(vkGetPhysicalDeviceProperties);
	LOAD_INSTANCE_FN(vkEnumerateDeviceExtensionProperties);
	if (!m_fn.vkGetDeviceProcAddr || !m_fn.vkGetPhysicalDeviceProperties || !m_fn.vkEnumerateDeviceExtensionProperties)
	{
		CryLogAlways("OpenXR: Vulkan instance functions could not be resolved");
		return false;
	}
	LOAD_DEVICE_FN(vkCreateCommandPool);
	LOAD_DEVICE_FN(vkDestroyCommandPool);
	LOAD_DEVICE_FN(vkAllocateCommandBuffers);
	LOAD_DEVICE_FN(vkResetCommandBuffer);
	LOAD_DEVICE_FN(vkBeginCommandBuffer);
	LOAD_DEVICE_FN(vkEndCommandBuffer);
	LOAD_DEVICE_FN(vkCmdPipelineBarrier);
	LOAD_DEVICE_FN(vkCmdCopyImage);
	LOAD_DEVICE_FN(vkCmdBlitImage);
	LOAD_DEVICE_FN(vkCmdClearColorImage);
	LOAD_DEVICE_FN(vkQueueSubmit);
	LOAD_DEVICE_FN(vkCreateFence);
	LOAD_DEVICE_FN(vkDestroyFence);
	LOAD_DEVICE_FN(vkWaitForFences);
	LOAD_DEVICE_FN(vkResetFences);
#undef LOAD_INSTANCE_FN
#undef LOAD_DEVICE_FN
	if (!m_fn.vkCreateCommandPool || !m_fn.vkDestroyCommandPool || !m_fn.vkAllocateCommandBuffers || !m_fn.vkResetCommandBuffer
		|| !m_fn.vkBeginCommandBuffer || !m_fn.vkEndCommandBuffer || !m_fn.vkCmdPipelineBarrier || !m_fn.vkCmdCopyImage
		|| !m_fn.vkCmdBlitImage || !m_fn.vkCmdClearColorImage || !m_fn.vkQueueSubmit || !m_fn.vkCreateFence
		|| !m_fn.vkDestroyFence || !m_fn.vkWaitForFences || !m_fn.vkResetFences)
	{
		CryLogAlways("OpenXR: Vulkan device functions could not be resolved");
		return false;
	}

	VkPhysicalDeviceProperties props;
	m_fn.vkGetPhysicalDeviceProperties(m_vk.physicalDevice, &props);
	CryLogAlways("OpenXR: rendering on %s (Vulkan %u.%u driver, queue family %u index %u)", props.deviceName,
		VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), m_vk.queueFamilyIndex, m_vk.queueIndex);
	return true;
}

bool VROpenXR::CheckVulkanRequirements()
{
	// XR_KHR_vulkan_enable wants the application to create its Vulkan instance and device with the extensions the
	// runtime names. Ours were created by dxvk long before this code runs, so all we can do is log what the runtime
	// wants and check that the GPU at least offers it; whether dxvk enabled it shows in xrCreateSession.

	PFN_xrGetVulkanGraphicsRequirementsKHR getRequirements = nullptr;
	PFN_xrGetVulkanInstanceExtensionsKHR getInstanceExtensions = nullptr;
	PFN_xrGetVulkanDeviceExtensionsKHR getDeviceExtensions = nullptr;
	PFN_xrGetVulkanGraphicsDeviceKHR getGraphicsDevice = nullptr;
	if (!Check(xrGetInstanceProcAddr(m_instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getRequirements), "xrGetInstanceProcAddr(xrGetVulkanGraphicsRequirementsKHR)")
		|| !Check(xrGetInstanceProcAddr(m_instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&getInstanceExtensions), "xrGetInstanceProcAddr(xrGetVulkanInstanceExtensionsKHR)")
		|| !Check(xrGetInstanceProcAddr(m_instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&getDeviceExtensions), "xrGetInstanceProcAddr(xrGetVulkanDeviceExtensionsKHR)")
		|| !Check(xrGetInstanceProcAddr(m_instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&getGraphicsDevice), "xrGetInstanceProcAddr(xrGetVulkanGraphicsDeviceKHR)"))
		return false;

	// mandatory before xrCreateSession
	XrGraphicsRequirementsVulkanKHR requirements;
	Zero(requirements, XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR);
	if (!Check(getRequirements(m_instance, m_systemId, &requirements), "xrGetVulkanGraphicsRequirementsKHR"))
		return false;
	CryLogAlways("OpenXR: the runtime supports Vulkan %u.%u - %u.%u (dxvk uses a 1.3 instance)",
		XR_VERSION_MAJOR(requirements.minApiVersionSupported), XR_VERSION_MINOR(requirements.minApiVersionSupported),
		XR_VERSION_MAJOR(requirements.maxApiVersionSupported), XR_VERSION_MINOR(requirements.maxApiVersionSupported));

	// instance extensions: informational (dxvk creates a Vulkan 1.3 instance, which has the usual ones built in)
	unsigned length = 0;
	std::string buffer;
	if (XR_SUCCEEDED(getInstanceExtensions(m_instance, m_systemId, 0, &length, nullptr)) && length > 0)
	{
		buffer.resize(length);
		if (XR_SUCCEEDED(getInstanceExtensions(m_instance, m_systemId, length, &length, &buffer[0])))
		{
			buffer.resize(strlen(buffer.c_str()));
			CryLogAlways("OpenXR: the runtime asks for these Vulkan instance extensions: %s", buffer.c_str());
		}
	}

	// device extensions: check them against what the GPU offers
	std::vector<std::string> required;
	length = 0;
	buffer.clear();
	if (XR_SUCCEEDED(getDeviceExtensions(m_instance, m_systemId, 0, &length, nullptr)) && length > 0)
	{
		buffer.resize(length);
		if (XR_SUCCEEDED(getDeviceExtensions(m_instance, m_systemId, length, &length, &buffer[0])))
		{
			buffer.resize(strlen(buffer.c_str()));
			CryLogAlways("OpenXR: the runtime asks for these Vulkan device extensions: %s", buffer.c_str());
			SplitExtensionList(buffer, required);
		}
	}
	if (!required.empty())
	{
		unsigned count = 0;
		std::vector<VkExtensionProperties> supported;
		if (m_fn.vkEnumerateDeviceExtensionProperties(m_vk.physicalDevice, nullptr, &count, nullptr) == VK_SUCCESS && count > 0)
		{
			supported.resize(count);
			if (m_fn.vkEnumerateDeviceExtensionProperties(m_vk.physicalDevice, nullptr, &count, &supported[0]) != VK_SUCCESS)
				supported.clear();
		}
		for (size_t r = 0; r < required.size(); ++r)
		{
			bool found = false;
			for (size_t s = 0; s < supported.size() && !found; ++s)
				found = required[r] == supported[s].extensionName;
			if (!found)
				CryLogAlways("OpenXR: WARNING: the GPU does not offer %s, which the runtime asks for", required[r].c_str());
		}
		CryLogAlways("OpenXR: note: dxvk 2.7 enables VK_KHR_external_memory_win32 and VK_KHR_external_semaphore_win32 when the GPU has them; anything else the runtime needs beyond Vulkan 1.3 core is not enabled on its device");
	}

	// the GPU the runtime renders on has to be the one dxvk picked
	VkPhysicalDevice wanted = VK_NULL_HANDLE;
	if (Check(getGraphicsDevice(m_instance, m_systemId, m_vk.instance, &wanted), "xrGetVulkanGraphicsDeviceKHR") && wanted != m_vk.physicalDevice)
	{
		VkPhysicalDeviceProperties props;
		m_fn.vkGetPhysicalDeviceProperties(wanted, &props);
		CryLogAlways("OpenXR: WARNING: the runtime renders on %s, but dxvk picked a different GPU; the session may fail or run slowly (dxvk.conf: dxvk.deviceFilter can select the GPU)", props.deviceName);
	}
	return true;
}

bool VROpenXR::CreateCommandBuffers()
{
	VkCommandPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = m_vk.queueFamilyIndex;
	if (m_fn.vkCreateCommandPool(m_vk.device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
	{
		CryLogAlways("OpenXR: vkCreateCommandPool failed");
		m_commandPool = VK_NULL_HANDLE;
		return false;
	}

	VkCommandBuffer buffers[kCommandSlots] = {};
	VkCommandBufferAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = m_commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = kCommandSlots;
	if (m_fn.vkAllocateCommandBuffers(m_vk.device, &allocInfo, buffers) != VK_SUCCESS)
	{
		CryLogAlways("OpenXR: vkAllocateCommandBuffers failed");
		return false;
	}

	VkFenceCreateInfo fenceInfo = {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	for (int i = 0; i < kCommandSlots; ++i)
	{
		m_commandSlots[i].commandBuffer = buffers[i];
		m_commandSlots[i].pending = false;
		if (m_fn.vkCreateFence(m_vk.device, &fenceInfo, nullptr, &m_commandSlots[i].fence) != VK_SUCCESS)
		{
			CryLogAlways("OpenXR: vkCreateFence failed");
			m_commandSlots[i].fence = VK_NULL_HANDLE;
			return false;
		}
	}
	m_nextCommandSlot = 0;
	return true;
}

void VROpenXR::DestroyCommandBuffers()
{
	if (m_vk.device == VK_NULL_HANDLE || !m_fn.vkDestroyFence)
		return;

	for (int i = 0; i < kCommandSlots; ++i)
	{
		CommandSlot& slot = m_commandSlots[i];
		if (slot.fence != VK_NULL_HANDLE)
		{
			if (slot.pending)
				m_fn.vkWaitForFences(m_vk.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
			m_fn.vkDestroyFence(m_vk.device, slot.fence, nullptr);
		}
		slot.fence = VK_NULL_HANDLE;
		slot.commandBuffer = VK_NULL_HANDLE;
		slot.pending = false;
	}
	if (m_commandPool != VK_NULL_HANDLE)
		m_fn.vkDestroyCommandPool(m_vk.device, m_commandPool, nullptr);   // frees the command buffers as well
	m_commandPool = VK_NULL_HANDLE;
}

VROpenXR::CommandSlot* VROpenXR::BeginCommands()
{
	if (m_commandPool == VK_NULL_HANDLE)
		return nullptr;

	CommandSlot& slot = m_commandSlots[m_nextCommandSlot];
	m_nextCommandSlot = (m_nextCommandSlot + 1) % kCommandSlots;
	if (slot.pending)
	{
		// the GPU is normally long done with a buffer by the time it comes around again
		if (m_fn.vkWaitForFences(m_vk.device, 1, &slot.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
			return nullptr;
		slot.pending = false;
	}
	m_fn.vkResetFences(m_vk.device, 1, &slot.fence);
	m_fn.vkResetCommandBuffer(slot.commandBuffer, 0);

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (m_fn.vkBeginCommandBuffer(slot.commandBuffer, &beginInfo) != VK_SUCCESS)
		return nullptr;
	return &slot;
}

bool VROpenXR::SubmitCommands(CommandSlot* slot)
{
	if (!slot)
		return false;
	if (m_fn.vkEndCommandBuffer(slot->commandBuffer) != VK_SUCCESS)
		return false;

	// no semaphores: the queue is dxvk's and everything on it is ordered by submission; the barriers inside
	// the command buffer wait for the game's rendering of the source image to finish
	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &slot->commandBuffer;
	VkResult result = m_fn.vkQueueSubmit(m_vk.queue, 1, &submitInfo, slot->fence);
	if (result != VK_SUCCESS)
	{
		CryLogAlways("OpenXR: vkQueueSubmit failed: %d", (int)result);
		return false;
	}
	slot->pending = true;
	return true;
}

void VROpenXR::RecordImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
	VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;
	m_fn.vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool VROpenXR::SameChannelOrder(VkFormat a, VkFormat b)
{
	const bool aBgra = a == VK_FORMAT_B8G8R8A8_UNORM || a == VK_FORMAT_B8G8R8A8_SRGB;
	const bool bBgra = b == VK_FORMAT_B8G8R8A8_UNORM || b == VK_FORMAT_B8G8R8A8_SRGB;
	const bool aRgba = a == VK_FORMAT_R8G8B8A8_UNORM || a == VK_FORMAT_R8G8B8A8_SRGB;
	const bool bRgba = b == VK_FORMAT_R8G8B8A8_UNORM || b == VK_FORMAT_R8G8B8A8_SRGB;
	return (aBgra && bBgra) || (aRgba && bRgba);
}

bool VROpenXR::IsSrgbFormat(VkFormat format)
{
	return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
}

// ---------------------------------------------------------------------------------------------
// actions and bindings
// ---------------------------------------------------------------------------------------------

namespace
{
	// every controller is mapped onto the same set of actions; hands are subaction paths
	#define LH "/user/hand/left/"
	#define RH "/user/hand/right/"

	const VROpenXR::BindingDef kTouchBindings[] =
	{
		{ VROpenXR::Action_GripPose,   LH "input/grip/pose" },        { VROpenXR::Action_GripPose,   RH "input/grip/pose" },
		{ VROpenXR::Action_AimPose,    LH "input/aim/pose" },         { VROpenXR::Action_AimPose,    RH "input/aim/pose" },
		{ VROpenXR::Action_Haptic,     LH "output/haptic" },          { VROpenXR::Action_Haptic,     RH "output/haptic" },
		{ VROpenXR::Action_Trigger,    LH "input/trigger/value" },    { VROpenXR::Action_Trigger,    RH "input/trigger/value" },
		{ VROpenXR::Action_Squeeze,    LH "input/squeeze/value" },    { VROpenXR::Action_Squeeze,    RH "input/squeeze/value" },
		{ VROpenXR::Action_Stick,      LH "input/thumbstick" },       { VROpenXR::Action_Stick,      RH "input/thumbstick" },
		{ VROpenXR::Action_StickClick, LH "input/thumbstick/click" }, { VROpenXR::Action_StickClick, RH "input/thumbstick/click" },
		{ VROpenXR::Action_Primary,    LH "input/x/click" },          { VROpenXR::Action_Primary,    RH "input/a/click" },
		{ VROpenXR::Action_Secondary,  LH "input/y/click" },          { VROpenXR::Action_Secondary,  RH "input/b/click" },
	};

	const VROpenXR::BindingDef kIndexBindings[] =
	{
		{ VROpenXR::Action_GripPose,   LH "input/grip/pose" },        { VROpenXR::Action_GripPose,   RH "input/grip/pose" },
		{ VROpenXR::Action_AimPose,    LH "input/aim/pose" },         { VROpenXR::Action_AimPose,    RH "input/aim/pose" },
		{ VROpenXR::Action_Haptic,     LH "output/haptic" },          { VROpenXR::Action_Haptic,     RH "output/haptic" },
		{ VROpenXR::Action_Trigger,    LH "input/trigger/value" },    { VROpenXR::Action_Trigger,    RH "input/trigger/value" },
		// squeeze/value on the Index is the finger curl, which a relaxed hand already registers; the force
		// sensor behaves like the grip "click" the SteamVR bindings used
		{ VROpenXR::Action_Squeeze,    LH "input/squeeze/force" },    { VROpenXR::Action_Squeeze,    RH "input/squeeze/force" },
		{ VROpenXR::Action_Stick,      LH "input/thumbstick" },       { VROpenXR::Action_Stick,      RH "input/thumbstick" },
		{ VROpenXR::Action_StickClick, LH "input/thumbstick/click" }, { VROpenXR::Action_StickClick, RH "input/thumbstick/click" },
		{ VROpenXR::Action_PadClick,   LH "input/trackpad/force" },   { VROpenXR::Action_PadClick,   RH "input/trackpad/force" },
		{ VROpenXR::Action_Primary,    LH "input/a/click" },          { VROpenXR::Action_Primary,    RH "input/a/click" },
		{ VROpenXR::Action_Secondary,  LH "input/b/click" },          { VROpenXR::Action_Secondary,  RH "input/b/click" },
	};

	// Windows Mixed Reality motion controllers (no face buttons: menu = secondary, trackpad click = primary)
	const VROpenXR::BindingDef kWmrBindings[] =
	{
		{ VROpenXR::Action_GripPose,   LH "input/grip/pose" },        { VROpenXR::Action_GripPose,   RH "input/grip/pose" },
		{ VROpenXR::Action_AimPose,    LH "input/aim/pose" },         { VROpenXR::Action_AimPose,    RH "input/aim/pose" },
		{ VROpenXR::Action_Haptic,     LH "output/haptic" },          { VROpenXR::Action_Haptic,     RH "output/haptic" },
		{ VROpenXR::Action_Trigger,    LH "input/trigger/value" },    { VROpenXR::Action_Trigger,    RH "input/trigger/value" },
		{ VROpenXR::Action_Squeeze,    LH "input/squeeze/click" },    { VROpenXR::Action_Squeeze,    RH "input/squeeze/click" },
		{ VROpenXR::Action_Stick,      LH "input/thumbstick" },       { VROpenXR::Action_Stick,      RH "input/thumbstick" },
		{ VROpenXR::Action_StickClick, LH "input/thumbstick/click" }, { VROpenXR::Action_StickClick, RH "input/thumbstick/click" },
		{ VROpenXR::Action_Primary,    LH "input/trackpad/click" },   { VROpenXR::Action_Primary,    RH "input/trackpad/click" },
		{ VROpenXR::Action_Secondary,  LH "input/menu/click" },       { VROpenXR::Action_Secondary,  RH "input/menu/click" },
	};

	// HTC Vive wands (trackpad stands in for the thumbstick)
	const VROpenXR::BindingDef kViveBindings[] =
	{
		{ VROpenXR::Action_GripPose,   LH "input/grip/pose" },        { VROpenXR::Action_GripPose,   RH "input/grip/pose" },
		{ VROpenXR::Action_AimPose,    LH "input/aim/pose" },         { VROpenXR::Action_AimPose,    RH "input/aim/pose" },
		{ VROpenXR::Action_Haptic,     LH "output/haptic" },          { VROpenXR::Action_Haptic,     RH "output/haptic" },
		{ VROpenXR::Action_Trigger,    LH "input/trigger/value" },    { VROpenXR::Action_Trigger,    RH "input/trigger/value" },
		{ VROpenXR::Action_Squeeze,    LH "input/squeeze/click" },    { VROpenXR::Action_Squeeze,    RH "input/squeeze/click" },
		{ VROpenXR::Action_Stick,      LH "input/trackpad" },         { VROpenXR::Action_Stick,      RH "input/trackpad" },
		{ VROpenXR::Action_StickClick, LH "input/trackpad/click" },   { VROpenXR::Action_StickClick, RH "input/trackpad/click" },
		{ VROpenXR::Action_Secondary,  LH "input/menu/click" },       { VROpenXR::Action_Secondary,  RH "input/menu/click" },
	};

	// the minimal profile every runtime supports: at least aiming, clicking and the menu work
	const VROpenXR::BindingDef kSimpleBindings[] =
	{
		{ VROpenXR::Action_GripPose,   LH "input/grip/pose" },        { VROpenXR::Action_GripPose,   RH "input/grip/pose" },
		{ VROpenXR::Action_AimPose,    LH "input/aim/pose" },         { VROpenXR::Action_AimPose,    RH "input/aim/pose" },
		{ VROpenXR::Action_Haptic,     LH "output/haptic" },          { VROpenXR::Action_Haptic,     RH "output/haptic" },
		{ VROpenXR::Action_Trigger,    LH "input/select/click" },     { VROpenXR::Action_Trigger,    RH "input/select/click" },
		{ VROpenXR::Action_Secondary,  LH "input/menu/click" },       { VROpenXR::Action_Secondary,  RH "input/menu/click" },
	};

	// Touch-like layouts behind extensions: HP Reverb G2, Pico 4 / Neo 3, Vive Cosmos, Vive Focus 3
	const VROpenXR::BindingDef kTouchLikeValueSqueeze[] =
	{
		{ VROpenXR::Action_GripPose,   LH "input/grip/pose" },        { VROpenXR::Action_GripPose,   RH "input/grip/pose" },
		{ VROpenXR::Action_AimPose,    LH "input/aim/pose" },         { VROpenXR::Action_AimPose,    RH "input/aim/pose" },
		{ VROpenXR::Action_Haptic,     LH "output/haptic" },          { VROpenXR::Action_Haptic,     RH "output/haptic" },
		{ VROpenXR::Action_Trigger,    LH "input/trigger/value" },    { VROpenXR::Action_Trigger,    RH "input/trigger/value" },
		{ VROpenXR::Action_Squeeze,    LH "input/squeeze/value" },    { VROpenXR::Action_Squeeze,    RH "input/squeeze/value" },
		{ VROpenXR::Action_Stick,      LH "input/thumbstick" },       { VROpenXR::Action_Stick,      RH "input/thumbstick" },
		{ VROpenXR::Action_StickClick, LH "input/thumbstick/click" }, { VROpenXR::Action_StickClick, RH "input/thumbstick/click" },
		{ VROpenXR::Action_Primary,    LH "input/x/click" },          { VROpenXR::Action_Primary,    RH "input/a/click" },
		{ VROpenXR::Action_Secondary,  LH "input/y/click" },          { VROpenXR::Action_Secondary,  RH "input/b/click" },
	};

	const VROpenXR::BindingDef kTouchLikeClickSqueeze[] =
	{
		{ VROpenXR::Action_GripPose,   LH "input/grip/pose" },        { VROpenXR::Action_GripPose,   RH "input/grip/pose" },
		{ VROpenXR::Action_AimPose,    LH "input/aim/pose" },         { VROpenXR::Action_AimPose,    RH "input/aim/pose" },
		{ VROpenXR::Action_Haptic,     LH "output/haptic" },          { VROpenXR::Action_Haptic,     RH "output/haptic" },
		{ VROpenXR::Action_Trigger,    LH "input/trigger/value" },    { VROpenXR::Action_Trigger,    RH "input/trigger/value" },
		{ VROpenXR::Action_Squeeze,    LH "input/squeeze/click" },    { VROpenXR::Action_Squeeze,    RH "input/squeeze/click" },
		{ VROpenXR::Action_Stick,      LH "input/thumbstick" },       { VROpenXR::Action_Stick,      RH "input/thumbstick" },
		{ VROpenXR::Action_StickClick, LH "input/thumbstick/click" }, { VROpenXR::Action_StickClick, RH "input/thumbstick/click" },
		{ VROpenXR::Action_Primary,    LH "input/x/click" },          { VROpenXR::Action_Primary,    RH "input/a/click" },
		{ VROpenXR::Action_Secondary,  LH "input/y/click" },          { VROpenXR::Action_Secondary,  RH "input/b/click" },
	};

	#undef LH
	#undef RH

	#define BINDINGS(table) table, (int)(sizeof(table) / sizeof(table[0]))
}

bool VROpenXR::CreateActions()
{
	XrActionSetCreateInfo setInfo;
	Zero(setInfo, XR_TYPE_ACTION_SET_CREATE_INFO);
	strcpy(setInfo.actionSetName, "farcry");
	strcpy(setInfo.localizedActionSetName, "Far Cry VR");
	if (!Check(xrCreateActionSet(m_instance, &setInfo, &m_actionSet), "xrCreateActionSet"))
	{
		m_actionSet = XR_NULL_HANDLE;
		return false;
	}

	m_handPath[Hand_Left] = Path("/user/hand/left");
	m_handPath[Hand_Right] = Path("/user/hand/right");

	struct ActionDef
	{
		ActionId id;
		const char* name;
		const char* localized;
		XrActionType type;
	};
	const ActionDef defs[] =
	{
		{ Action_GripPose,   "grip_pose",   "Hand Pose",          XR_ACTION_TYPE_POSE_INPUT },
		{ Action_AimPose,    "aim_pose",    "Pointer Pose",       XR_ACTION_TYPE_POSE_INPUT },
		{ Action_Haptic,     "haptic",      "Vibration",          XR_ACTION_TYPE_VIBRATION_OUTPUT },
		{ Action_Trigger,    "trigger",     "Trigger",            XR_ACTION_TYPE_FLOAT_INPUT },
		{ Action_Squeeze,    "squeeze",     "Grip",               XR_ACTION_TYPE_FLOAT_INPUT },
		{ Action_Stick,      "thumbstick",  "Thumbstick",         XR_ACTION_TYPE_VECTOR2F_INPUT },
		{ Action_StickClick, "stick_click", "Thumbstick Click",   XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ Action_PadClick,   "pad_click",   "Trackpad Click",     XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ Action_Primary,    "primary",     "Primary Button",     XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ Action_Secondary,  "secondary",   "Secondary Button",   XR_ACTION_TYPE_BOOLEAN_INPUT },
	};

	for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); ++i)
	{
		XrActionCreateInfo actionInfo;
		Zero(actionInfo, XR_TYPE_ACTION_CREATE_INFO);
		strcpy(actionInfo.actionName, defs[i].name);
		strcpy(actionInfo.localizedActionName, defs[i].localized);
		actionInfo.actionType = defs[i].type;
		actionInfo.countSubactionPaths = 2;
		actionInfo.subactionPaths = m_handPath;
		if (!Check(xrCreateAction(m_actionSet, &actionInfo, &m_actions[defs[i].id]), "xrCreateAction"))
		{
			m_actions[defs[i].id] = XR_NULL_HANDLE;
			return false;
		}
	}

	SuggestBindings("/interaction_profiles/oculus/touch_controller", BINDINGS(kTouchBindings));
	SuggestBindings("/interaction_profiles/valve/index_controller", BINDINGS(kIndexBindings));
	SuggestBindings("/interaction_profiles/microsoft/motion_controller", BINDINGS(kWmrBindings));
	SuggestBindings("/interaction_profiles/htc/vive_controller", BINDINGS(kViveBindings));
	SuggestBindings("/interaction_profiles/khr/simple_controller", BINDINGS(kSimpleBindings));
	if (HasExtension("XR_EXT_hp_mixed_reality_controller"))
		SuggestBindings("/interaction_profiles/hp/mixed_reality_controller", BINDINGS(kTouchLikeValueSqueeze));
	if (HasExtension("XR_BD_controller_interaction"))
	{
		SuggestBindings("/interaction_profiles/bytedance/pico4_controller", BINDINGS(kTouchLikeValueSqueeze));
		SuggestBindings("/interaction_profiles/bytedance/pico_neo3_controller", BINDINGS(kTouchLikeValueSqueeze));
	}
	if (HasExtension("XR_HTC_vive_cosmos_controller_interaction"))
		SuggestBindings("/interaction_profiles/htc/vive_cosmos_controller", BINDINGS(kTouchLikeClickSqueeze));
	if (HasExtension("XR_HTC_vive_focus3_controller_interaction"))
		SuggestBindings("/interaction_profiles/htc/vive_focus3_controller", BINDINGS(kTouchLikeClickSqueeze));   // squeeze/value needs revision 2

	XrSessionActionSetsAttachInfo attachInfo;
	Zero(attachInfo, XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &m_actionSet;
	if (!Check(xrAttachSessionActionSets(m_session, &attachInfo), "xrAttachSessionActionSets"))
		return false;
	m_inputAttached = true;

	for (int hand = 0; hand < 2; ++hand)
	{
		XrActionSpaceCreateInfo spaceInfo;
		Zero(spaceInfo, XR_TYPE_ACTION_SPACE_CREATE_INFO);
		spaceInfo.subactionPath = m_handPath[hand];
		spaceInfo.poseInActionSpace = xrmath::Identity();

		spaceInfo.action = m_actions[Action_GripPose];
		if (!Check(xrCreateActionSpace(m_session, &spaceInfo, &m_gripSpace[hand]), "xrCreateActionSpace(grip)"))
		{
			m_gripSpace[hand] = XR_NULL_HANDLE;
			return false;
		}
		spaceInfo.action = m_actions[Action_AimPose];
		if (!Check(xrCreateActionSpace(m_session, &spaceInfo, &m_aimSpace[hand]), "xrCreateActionSpace(aim)"))
		{
			m_aimSpace[hand] = XR_NULL_HANDLE;
			return false;
		}
	}
	return true;
}

void VROpenXR::SuggestBindings(const char* profile, const BindingDef* defs, int count)
{
	std::vector<XrActionSuggestedBinding> bindings;
	for (int i = 0; i < count; ++i)
	{
		XrActionSuggestedBinding binding;
		binding.action = m_actions[defs[i].action];
		binding.binding = Path(defs[i].path);
		if (binding.action != XR_NULL_HANDLE && binding.binding != XR_NULL_PATH)
			bindings.push_back(binding);
	}
	if (bindings.empty())
		return;

	XrInteractionProfileSuggestedBinding suggested;
	Zero(suggested, XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
	suggested.interactionProfile = Path(profile);
	suggested.suggestedBindings = &bindings[0];
	suggested.countSuggestedBindings = (uint32_t)bindings.size();
	if (XR_FAILED(xrSuggestInteractionProfileBindings(m_instance, &suggested)))
		CryLogAlways("OpenXR: bindings for %s were rejected by the runtime", profile);
}

// ---------------------------------------------------------------------------------------------
// session events
// ---------------------------------------------------------------------------------------------

void VROpenXR::PollEvents()
{
	if (m_instance == XR_NULL_HANDLE)
		return;

	for (;;)
	{
		XrEventDataBuffer event;
		Zero(event, XR_TYPE_EVENT_DATA_BUFFER);
		if (xrPollEvent(m_instance, &event) != XR_SUCCESS)
			break;

		switch (event.type)
		{
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
		{
			const XrEventDataSessionStateChanged* changed = (const XrEventDataSessionStateChanged*)&event;
			XrSessionState previous = m_sessionState;
			m_sessionState = changed->state;
			CryLogAlways("OpenXR: session %s", SessionStateName(m_sessionState));

			switch (m_sessionState)
			{
			case XR_SESSION_STATE_READY:
			{
				XrSessionBeginInfo beginInfo;
				Zero(beginInfo, XR_TYPE_SESSION_BEGIN_INFO);
				beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				ScopedQueueLock lock(*this, false);
				if (Check(xrBeginSession(m_session, &beginInfo), "xrBeginSession"))
					m_sessionRunning = true;
				break;
			}
			case XR_SESSION_STATE_VISIBLE:
				if (previous == XR_SESSION_STATE_FOCUSED)
					m_focusLost = true;
				break;
			case XR_SESSION_STATE_STOPPING:
			{
				m_sessionRunning = false;
				m_frameOpen = false;
				ScopedQueueLock lock(*this, false);
				Check(xrEndSession(m_session), "xrEndSession");
				break;
			}
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				m_sessionRunning = false;
				m_frameOpen = false;
				m_quitRequested = true;
				break;
			default:
				break;
			}
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			m_sessionRunning = false;
			m_frameOpen = false;
			m_quitRequested = true;
			break;
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			m_recenter = true;
			break;
		case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
		{
			for (int hand = 0; hand < 2; ++hand)
			{
				XrInteractionProfileState state;
				Zero(state, XR_TYPE_INTERACTION_PROFILE_STATE);
				if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(m_session, m_handPath[hand], &state)) && state.interactionProfile != XR_NULL_PATH)
				{
					char name[XR_MAX_PATH_LENGTH] = {};
					unsigned length = 0;
					xrPathToString(m_instance, state.interactionProfile, XR_MAX_PATH_LENGTH, &length, name);
					CryLogAlways("OpenXR: %s hand controller: %s", hand == Hand_Left ? "left" : "right", name);
				}
			}
			break;
		}
		default:
			break;
		}
	}
}

bool VROpenXR::TakeQuitRequest()
{
	bool result = m_quitRequested;
	m_quitRequested = false;
	return result;
}

bool VROpenXR::TakeFocusLost()
{
	bool result = m_focusLost;
	m_focusLost = false;
	return result;
}

bool VROpenXR::TakeRecenter()
{
	bool result = m_recenter;
	m_recenter = false;
	return result;
}

// ---------------------------------------------------------------------------------------------
// frame loop
// ---------------------------------------------------------------------------------------------

bool VROpenXR::WarmUp()
{
	// the session becomes ready shortly after creation; wait for it so the field of view is known
	// before the game decides on its render resolution
	for (int i = 0; i < 300 && !m_sessionRunning; ++i)
	{
		PollEvents();
		if (!m_sessionRunning)
			Sleep(10);
	}
	if (!m_sessionRunning)
	{
		CryLogAlways("OpenXR: the session did not become ready in time, continuing without a known field of view");
		return false;
	}

	if (!BeginFrame())
		return false;
	EndFrame(nullptr, nullptr, 0);
	return m_fovKnown;
}

bool VROpenXR::BeginFrame()
{
	if (m_session == XR_NULL_HANDLE)
		return false;

	PollEvents();
	if (!m_sessionRunning)
		return false;

	XrFrameState frameState;
	Zero(frameState, XR_TYPE_FRAME_STATE);
	XrFrameWaitInfo waitInfo;
	Zero(waitInfo, XR_TYPE_FRAME_WAIT_INFO);
	if (!Check(xrWaitFrame(m_session, &waitInfo, &frameState), "xrWaitFrame"))
		return false;

	XrFrameBeginInfo beginInfo;
	Zero(beginInfo, XR_TYPE_FRAME_BEGIN_INFO);
	XrResult result;
	{
		// xrWaitFrame above is pure timing, but some runtimes do start GPU work for the frame in here
		ScopedQueueLock lock(*this, false);
		result = xrBeginFrame(m_session, &beginInfo);
	}
	if (XR_FAILED(result))
	{
		Check(result, "xrBeginFrame");
		return false;
	}

	m_frameState = frameState;
	m_frameOpen = true;
	m_shouldRender = frameState.shouldRender == XR_TRUE;

	SyncInput();
	LocateFrame();
	return true;
}

void VROpenXR::LocateFrame()
{
	const XrTime time = m_frameState.predictedDisplayTime;

	// eyes
	for (int i = 0; i < 2; ++i)
		Zero(m_views[i], XR_TYPE_VIEW);
	XrViewLocateInfo locateInfo;
	Zero(locateInfo, XR_TYPE_VIEW_LOCATE_INFO);
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = time;
	locateInfo.space = m_stageSpace;
	XrViewState viewState;
	Zero(viewState, XR_TYPE_VIEW_STATE);
	unsigned located = 0;
	m_viewsValid = false;
	if (XR_SUCCEEDED(xrLocateViews(m_session, &locateInfo, &viewState, 2, &located, m_views)) && located == 2)
	{
		m_fovKnown = true;
		m_viewsValid = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
	}

	// head
	XrSpaceLocation location;
	Zero(location, XR_TYPE_SPACE_LOCATION);
	m_headValid = false;
	if (XR_SUCCEEDED(xrLocateSpace(m_viewSpace, m_stageSpace, time, &location))
		&& (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
	{
		m_headPose.orientation = location.pose.orientation;
		if (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
			m_headPose.position = location.pose.position;   // else keep the last known position
		m_headValid = true;
	}

	// hands: the orientation has to be tracked, a lost position keeps its last known value
	for (int hand = 0; hand < 2; ++hand)
	{
		m_gripValid[hand] = LocateHand(m_gripSpace[hand], time, m_gripPose[hand]);
		m_aimValid[hand] = LocateHand(m_aimSpace[hand], time, m_aimPose[hand]);
	}
}

bool VROpenXR::LocateHand(XrSpace space, XrTime time, XrPosef& pose) const
{
	if (space == XR_NULL_HANDLE)
		return false;
	XrSpaceLocation location;
	Zero(location, XR_TYPE_SPACE_LOCATION);
	if (XR_FAILED(xrLocateSpace(space, m_stageSpace, time, &location)) || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		return false;
	pose.orientation = location.pose.orientation;
	if (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
		pose.position = location.pose.position;
	return true;
}

void VROpenXR::SyncInput()
{
	for (int hand = 0; hand < 2; ++hand)
		m_hands[hand] = HandState();

	if (!m_inputAttached)
		return;

	XrActiveActionSet activeSet;
	activeSet.actionSet = m_actionSet;
	activeSet.subactionPath = XR_NULL_PATH;
	XrActionsSyncInfo syncInfo;
	Zero(syncInfo, XR_TYPE_ACTIONS_SYNC_INFO);
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeSet;
	XrResult result = xrSyncActions(m_session, &syncInfo);
	if (result != XR_SUCCESS)
		return;   // XR_SESSION_NOT_FOCUSED: another application has the controllers

	for (int hand = 0; hand < 2; ++hand)
	{
		HandState& state = m_hands[hand];
		XrActionStateGetInfo getInfo;
		Zero(getInfo, XR_TYPE_ACTION_STATE_GET_INFO);
		getInfo.subactionPath = m_handPath[hand];

		XrActionStateFloat floatState;
		XrActionStateBoolean boolState;
		XrActionStateVector2f vectorState;

		getInfo.action = m_actions[Action_Trigger];
		Zero(floatState, XR_TYPE_ACTION_STATE_FLOAT);
		if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &getInfo, &floatState)) && floatState.isActive)
		{
			state.trigger = floatState.currentState;
			state.active = true;
		}

		getInfo.action = m_actions[Action_Squeeze];
		Zero(floatState, XR_TYPE_ACTION_STATE_FLOAT);
		if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &getInfo, &floatState)) && floatState.isActive)
		{
			state.squeeze = floatState.currentState;
			state.active = true;
		}

		getInfo.action = m_actions[Action_Stick];
		Zero(vectorState, XR_TYPE_ACTION_STATE_VECTOR2F);
		if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &getInfo, &vectorState)) && vectorState.isActive)
		{
			state.stickX = vectorState.currentState.x;
			state.stickY = vectorState.currentState.y;
			state.active = true;
		}

		const ActionId boolActions[] = { Action_StickClick, Action_PadClick, Action_Primary, Action_Secondary };
		bool* boolTargets[] = { &state.stickClick, &state.padClick, &state.primary, &state.secondary };
		for (int i = 0; i < 4; ++i)
		{
			getInfo.action = m_actions[boolActions[i]];
			Zero(boolState, XR_TYPE_ACTION_STATE_BOOLEAN);
			if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &getInfo, &boolState)) && boolState.isActive)
			{
				*boolTargets[i] = boolState.currentState == XR_TRUE;
				state.active = true;
			}
		}
	}
}

void VROpenXR::EndFrame(const ProjectionView* projection, const QuadLayer* quads, int quadCount)
{
	if (!m_frameOpen)
		return;

	const XrCompositionLayerBaseHeader* layers[kMaxLayers];
	int layerCount = 0;

	XrCompositionLayerProjectionView projectionViews[2];
	XrCompositionLayerProjection projectionLayer;
	if (projection && m_shouldRender && m_viewsValid && projection[0].swapchain && projection[1].swapchain
		&& projection[0].swapchain->handle != XR_NULL_HANDLE && projection[1].swapchain->handle != XR_NULL_HANDLE)
	{
		for (int eye = 0; eye < 2; ++eye)
		{
			Zero(projectionViews[eye], XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW);
			projectionViews[eye].pose = m_views[eye].pose;
			projectionViews[eye].fov = projection[eye].fov;
			projectionViews[eye].subImage.swapchain = projection[eye].swapchain->handle;
			projectionViews[eye].subImage.imageRect = projection[eye].rect;
			projectionViews[eye].subImage.imageArrayIndex = 0;
		}
		Zero(projectionLayer, XR_TYPE_COMPOSITION_LAYER_PROJECTION);
		projectionLayer.space = m_stageSpace;
		projectionLayer.viewCount = 2;
		projectionLayer.views = projectionViews;
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&projectionLayer;
	}

	XrCompositionLayerQuad quadLayers[kMaxLayers];
	for (int i = 0; i < quadCount && layerCount < kMaxLayers; ++i)
	{
		const QuadLayer& quad = quads[i];
		if (!m_shouldRender || !quad.swapchain || quad.swapchain->handle == XR_NULL_HANDLE)
			continue;
		if (quad.headLocked && m_viewSpace == XR_NULL_HANDLE)
			continue;

		XrCompositionLayerQuad& layer = quadLayers[layerCount];
		Zero(layer, XR_TYPE_COMPOSITION_LAYER_QUAD);
		// the game composes its 2D layers onto a cleared (transparent black) target, which leaves
		// premultiplied alpha behind
		layer.layerFlags = quad.alphaBlend ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
		layer.space = quad.headLocked ? m_viewSpace : m_stageSpace;
		layer.eyeVisibility = quad.eye;
		layer.subImage.swapchain = quad.swapchain->handle;
		layer.subImage.imageRect = quad.rect;
		layer.subImage.imageArrayIndex = 0;
		layer.pose = quad.pose;
		layer.size.width = quad.width;
		layer.size.height = quad.height;
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&layer;
	}

	XrFrameEndInfo endInfo;
	Zero(endInfo, XR_TYPE_FRAME_END_INFO);
	endInfo.displayTime = m_frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = (uint32_t)layerCount;
	endInfo.layers = layerCount ? layers : nullptr;
	{
		// the runtime synchronizes with our copies by submitting to the (dxvk's) queue itself
		ScopedQueueLock lock(*this, false);
		Check(xrEndFrame(m_session, &endInfo), "xrEndFrame");
	}

	m_frameOpen = false;
}

// ---------------------------------------------------------------------------------------------
// tracking / input queries
// ---------------------------------------------------------------------------------------------

bool VROpenXR::GetHeadPose(XrPosef& pose) const
{
	pose = m_headPose;
	return m_headValid;
}

bool VROpenXR::GetEyePose(int eye, XrPosef& pose) const
{
	eye = eye < 0 ? 0 : (eye > 1 ? 1 : eye);
	pose = m_views[eye].pose;
	return m_viewsValid;
}

bool VROpenXR::GetEyeFov(int eye, XrFovf& fov) const
{
	eye = eye < 0 ? 0 : (eye > 1 ? 1 : eye);
	fov = m_views[eye].fov;
	return m_fovKnown;
}

bool VROpenXR::GetHandPose(int hand, bool aim, XrPosef& pose) const
{
	hand = hand < 0 ? 0 : (hand > 1 ? 1 : hand);
	pose = aim ? m_aimPose[hand] : m_gripPose[hand];
	return aim ? m_aimValid[hand] : m_gripValid[hand];
}

void VROpenXR::GetRecommendedEyeSize(unsigned& width, unsigned& height) const
{
	width = m_configViews[0].recommendedImageRectWidth;
	height = m_configViews[0].recommendedImageRectHeight;
	if (width == 0 || height == 0)
	{
		width = 1280;
		height = 1280;
	}
}

void VROpenXR::ApplyHaptic(int hand, float amplitude, float frequency, float seconds)
{
	if (!m_inputAttached || !m_sessionRunning || m_actions[Action_Haptic] == XR_NULL_HANDLE)
		return;
	hand = hand < 0 ? 0 : (hand > 1 ? 1 : hand);

	XrHapticActionInfo hapticInfo;
	Zero(hapticInfo, XR_TYPE_HAPTIC_ACTION_INFO);
	hapticInfo.action = m_actions[Action_Haptic];
	hapticInfo.subactionPath = m_handPath[hand];

	if (amplitude <= 0.0f)
	{
		// the haptics code asks for silence every frame while idle; the runtime only needs to hear it once
		if (m_lastHapticAmplitude[hand] > 0.0f)
			xrStopHapticFeedback(m_session, &hapticInfo);
		m_lastHapticAmplitude[hand] = 0.0f;
		return;
	}
	m_lastHapticAmplitude[hand] = amplitude;

	XrHapticVibration vibration;
	Zero(vibration, XR_TYPE_HAPTIC_VIBRATION);
	// a little longer than asked: the haptics code re-issues a step every 1/30 s, and a buzz that expires
	// a millisecond before the next one arrives is felt as a gap
	vibration.duration = seconds > 0.0f ? (XrDuration)((seconds + 0.010f) * 1000000000.0) : XR_MIN_HAPTIC_DURATION;
	vibration.frequency = frequency > 0.0f ? frequency : XR_FREQUENCY_UNSPECIFIED;
	vibration.amplitude = amplitude > 1.0f ? 1.0f : amplitude;
	xrApplyHapticFeedback(m_session, &hapticInfo, (const XrHapticBaseHeader*)&vibration);
}

// ---------------------------------------------------------------------------------------------
// swapchains
// ---------------------------------------------------------------------------------------------

bool VROpenXR::CreateSwapchain(Swapchain& swapchain, unsigned width, unsigned height)
{
	if (swapchain.handle != XR_NULL_HANDLE && swapchain.width == width && swapchain.height == height)
		return true;
	DestroySwapchain(swapchain);
	if (m_session == XR_NULL_HANDLE || width == 0 || height == 0)
		return false;

	ScopedQueueLock lock(*this, false);

	XrSwapchainCreateInfo createInfo;
	Zero(createInfo, XR_TYPE_SWAPCHAIN_CREATE_INFO);
	// COLOR_ATTACHMENT decides the layout the runtime hands the images over in (COLOR_ATTACHMENT_OPTIMAL),
	// TRANSFER_DST is what we really do with them
	createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	createInfo.format = m_swapchainFormat;
	createInfo.sampleCount = 1;
	createInfo.width = width;
	createInfo.height = height;
	createInfo.faceCount = 1;
	createInfo.arraySize = 1;
	createInfo.mipCount = 1;
	if (!Check(xrCreateSwapchain(m_session, &createInfo, &swapchain.handle), "xrCreateSwapchain"))
	{
		swapchain.handle = XR_NULL_HANDLE;
		return false;
	}

	unsigned imageCount = 0;
	if (!Check(xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr), "xrEnumerateSwapchainImages") || imageCount == 0)
	{
		DestroySwapchain(swapchain);
		return false;
	}
	std::vector<XrSwapchainImageVulkanKHR> images(imageCount);
	for (unsigned i = 0; i < imageCount; ++i)
		Zero(images[i], XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR);
	if (!Check(xrEnumerateSwapchainImages(swapchain.handle, imageCount, &imageCount, (XrSwapchainImageBaseHeader*)&images[0]), "xrEnumerateSwapchainImages"))
	{
		DestroySwapchain(swapchain);
		return false;
	}
	swapchain.images.resize(imageCount);
	for (unsigned i = 0; i < imageCount; ++i)
		swapchain.images[i] = images[i].image;
	swapchain.width = width;
	swapchain.height = height;
	return true;
}

void VROpenXR::DestroySwapchain(Swapchain& swapchain)
{
	if (swapchain.handle != XR_NULL_HANDLE)
	{
		ScopedQueueLock lock(*this, false);
		xrDestroySwapchain(swapchain.handle);
	}
	swapchain.handle = XR_NULL_HANDLE;
	swapchain.images.clear();
	swapchain.width = 0;
	swapchain.height = 0;
}

bool VROpenXR::AcquireImage(Swapchain& swapchain, unsigned& index)
{
	XrSwapchainImageAcquireInfo acquireInfo;
	Zero(acquireInfo, XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
	if (!Check(xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &index), "xrAcquireSwapchainImage"))
		return false;

	XrSwapchainImageWaitInfo waitInfo;
	Zero(waitInfo, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
	waitInfo.timeout = XR_INFINITE_DURATION;
	XrResult result = xrWaitSwapchainImage(swapchain.handle, &waitInfo);
	if (result == XR_TIMEOUT_EXPIRED || XR_FAILED(result))
	{
		// an image that was never waited for successfully must not be released
		Check(result, "xrWaitSwapchainImage");
		return false;
	}
	if (index >= swapchain.images.size())
	{
		ReleaseImage(swapchain);
		return false;
	}
	return true;
}

void VROpenXR::ReleaseImage(Swapchain& swapchain)
{
	XrSwapchainImageReleaseInfo releaseInfo;
	Zero(releaseInfo, XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
	xrReleaseSwapchainImage(swapchain.handle, &releaseInfo);
}

bool VROpenXR::CopyToSwapchain(Swapchain& swapchain, const SourceImage& source)
{
	if (swapchain.handle == XR_NULL_HANDLE || source.image == VK_NULL_HANDLE || m_commandPool == VK_NULL_HANDLE)
		return false;
	if (source.width != swapchain.width || source.height != swapchain.height)
	{
		CryLogAlways("OpenXR: source image (%u x %u) does not match the swapchain (%u x %u)", source.width, source.height, swapchain.width, swapchain.height);
		return false;
	}

	// acquire, copy and release all happen while dxvk is kept off the queue: the runtime may submit its own
	// synchronization to it in any of the three
	ScopedQueueLock lock(*this, false);

	unsigned index = 0;
	if (!AcquireImage(swapchain, index))
		return false;
	VkImage target = swapchain.images[index];

	CommandSlot* slot = BeginCommands();
	if (!slot)
	{
		ReleaseImage(swapchain);
		return false;
	}
	VkCommandBuffer cmd = slot->commandBuffer;

	// the runtime hands the image over in COLOR_ATTACHMENT_OPTIMAL and wants it back in that layout
	RecordImageBarrier(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	// the game's rendering of the source was submitted earlier on this very queue; make it visible to the copy
	RecordImageBarrier(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);

	if (SameChannelOrder(source.format, m_swapchainFormat))
	{
		// a raw copy: the game's gamma encoded bytes land in the (preferably sRGB) swapchain untouched
		VkImageCopy region = {};
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.layerCount = 1;
		region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.dstSubresource.layerCount = 1;
		region.extent.width = source.width;
		region.extent.height = source.height;
		region.extent.depth = 1;
		m_fn.vkCmdCopyImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}
	else
	{
		// different channel order: a blit swizzles (VRManager normally avoids this by matching the render target format)
		VkImageBlit region = {};
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.layerCount = 1;
		region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.dstSubresource.layerCount = 1;
		region.srcOffsets[1].x = region.dstOffsets[1].x = (int32_t)source.width;
		region.srcOffsets[1].y = region.dstOffsets[1].y = (int32_t)source.height;
		region.srcOffsets[1].z = region.dstOffsets[1].z = 1;
		m_fn.vkCmdBlitImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
	}

	RecordImageBarrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);

	bool ok = SubmitCommands(slot);
	ReleaseImage(swapchain);
	return ok;
}

bool VROpenXR::FillSwapchain(Swapchain& swapchain, unsigned colorBGRA)
{
	if (swapchain.handle == XR_NULL_HANDLE || m_commandPool == VK_NULL_HANDLE)
		return false;

	// the color is given as it should end up in memory (sRGB bytes); a clear value is written through the
	// format's encoding, so for an sRGB target feed it the linear equivalent
	VkClearColorValue color;
	const unsigned channels[3] = { (colorBGRA >> 16) & 0xff, (colorBGRA >> 8) & 0xff, colorBGRA & 0xff };   // r, g, b
	for (int i = 0; i < 3; ++i)
		color.float32[i] = IsSrgbFormat(m_swapchainFormat) ? SrgbToLinear(channels[i]) : channels[i] / 255.0f;
	color.float32[3] = ((colorBGRA >> 24) & 0xff) / 255.0f;

	ScopedQueueLock lock(*this, false);

	unsigned index = 0;
	if (!AcquireImage(swapchain, index))
		return false;
	VkImage target = swapchain.images[index];

	CommandSlot* slot = BeginCommands();
	if (!slot)
	{
		ReleaseImage(swapchain);
		return false;
	}
	VkCommandBuffer cmd = slot->commandBuffer;

	RecordImageBarrier(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	VkImageSubresourceRange range = {};
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;
	m_fn.vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
	RecordImageBarrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);

	bool ok = SubmitCommands(slot);
	ReleaseImage(swapchain);
	return ok;
}

// ---------------------------------------------------------------------------------------------

void VROpenXR::Shutdown()
{
	for (int hand = 0; hand < 2; ++hand)
	{
		if (m_gripSpace[hand] != XR_NULL_HANDLE)
			xrDestroySpace(m_gripSpace[hand]);
		if (m_aimSpace[hand] != XR_NULL_HANDLE)
			xrDestroySpace(m_aimSpace[hand]);
		m_gripSpace[hand] = XR_NULL_HANDLE;
		m_aimSpace[hand] = XR_NULL_HANDLE;
	}
	if (m_actionSet != XR_NULL_HANDLE)
	{
		xrDestroyActionSet(m_actionSet);   // destroys the actions as well
		m_actionSet = XR_NULL_HANDLE;
	}
	for (int i = 0; i < Action_Count; ++i)
		m_actions[i] = XR_NULL_HANDLE;
	m_inputAttached = false;
	m_lastHapticAmplitude[0] = m_lastHapticAmplitude[1] = 0.0f;

	if (m_viewSpace != XR_NULL_HANDLE)
	{
		xrDestroySpace(m_viewSpace);
		m_viewSpace = XR_NULL_HANDLE;
	}
	if (m_stageSpace != XR_NULL_HANDLE)
	{
		xrDestroySpace(m_stageSpace);
		m_stageSpace = XR_NULL_HANDLE;
	}
	if (m_session != XR_NULL_HANDLE)
	{
		ScopedQueueLock lock(*this, false);
		xrDestroySession(m_session);
		m_session = XR_NULL_HANDLE;
	}
	if (m_instance != XR_NULL_HANDLE)
	{
		xrDestroyInstance(m_instance);
		m_instance = XR_NULL_HANDLE;
	}
	m_systemId = XR_NULL_SYSTEM_ID;
	m_sessionState = XR_SESSION_STATE_UNKNOWN;
	m_sessionRunning = false;
	m_frameOpen = false;
	m_shouldRender = false;
	m_viewsValid = false;
	m_fovKnown = false;
	m_headValid = false;
	m_enabledExtensions.clear();
	m_swapchainFormat = VK_FORMAT_UNDEFINED;

	// our Vulkan objects; the device itself is dxvk's and stays
	DestroyCommandBuffers();
	m_vk = VulkanDevice();
	m_fn = VulkanFunctions();
	while (m_queueLockDepth > 0)   // never leave dxvk's queue locked behind
		UnlockQueue();
	m_queueLock = nullptr;
	if (m_vulkanLibrary)
	{
		FreeLibrary(m_vulkanLibrary);
		m_vulkanLibrary = nullptr;
	}
}
