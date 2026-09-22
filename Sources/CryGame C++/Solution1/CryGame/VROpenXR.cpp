#include "StdAfx.h"
#include "VROpenXR.h"

#include <dxgi.h>
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

bool VROpenXR::Init()
{
	if (m_instance != XR_NULL_HANDLE)
		return true;

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

	bool haveD3D11 = false;
	for (size_t i = 0; i < available.size(); ++i)
	{
		if (strcmp(available[i].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
			haveD3D11 = true;
		for (size_t j = 0; j < sizeof(kOptionalExtensions) / sizeof(kOptionalExtensions[0]); ++j)
		{
			if (strcmp(available[i].extensionName, kOptionalExtensions[j]) == 0)
				m_enabledExtensions.push_back(kOptionalExtensions[j]);
		}
	}
	if (!haveD3D11)
	{
		CryLogAlways("OpenXR: the runtime does not support XR_KHR_D3D11_enable (or no runtime is installed)");
		Shutdown();
		return false;
	}
	m_enabledExtensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);

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

	// --- D3D11 device on the adapter the runtime renders on --------------------------------------

	if (!CreateDevice11())
	{
		Shutdown();
		return false;
	}

	// --- session ---------------------------------------------------------------------------------

	XrGraphicsBindingD3D11KHR binding;
	Zero(binding, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR);
	binding.device = m_device11;

	XrSessionCreateInfo sessionInfo;
	Zero(sessionInfo, XR_TYPE_SESSION_CREATE_INFO);
	sessionInfo.next = &binding;
	sessionInfo.systemId = m_systemId;
	if (!Check(xrCreateSession(m_instance, &sessionInfo, &m_session), "xrCreateSession"))
	{
		m_session = XR_NULL_HANDLE;
		Shutdown();
		return false;
	}

	// --- swapchain format: the D3D9Ex surfaces are A8R8G8B8 = DXGI B8G8R8A8, and CopyResource needs
	// the same format family, so it has to be one of these ---------------------------------------

	unsigned formatCount = 0;
	std::vector<int64_t> formats;
	if (XR_SUCCEEDED(xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr)) && formatCount > 0)
	{
		formats.resize(formatCount);
		if (XR_FAILED(xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, &formats[0])))
			formats.clear();
	}
	const DXGI_FORMAT preferred[] = { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM };
	m_swapchainFormat = DXGI_FORMAT_UNKNOWN;
	for (int p = 0; p < 2 && m_swapchainFormat == DXGI_FORMAT_UNKNOWN; ++p)
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
	if (m_swapchainFormat == DXGI_FORMAT_UNKNOWN)
	{
		CryLogAlways("OpenXR: the runtime offers no BGRA swapchain format, the D3D9 frames cannot be handed over");
		Shutdown();
		return false;
	}
	if (m_swapchainFormat == DXGI_FORMAT_B8G8R8A8_UNORM)
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

bool VROpenXR::CreateDevice11()
{
	PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
	if (!Check(xrGetInstanceProcAddr(m_instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getRequirements), "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)"))
		return false;

	XrGraphicsRequirementsD3D11KHR requirements;
	Zero(requirements, XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR);
	if (!Check(getRequirements(m_instance, m_systemId, &requirements), "xrGetD3D11GraphicsRequirementsKHR"))
		return false;
	m_adapterLuid = requirements.adapterLuid;

	IDXGIFactory1* factory = nullptr;
	IDXGIAdapter1* adapter = nullptr;
	if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) && factory)
	{
		IDXGIAdapter1* candidate = nullptr;
		for (UINT i = 0; factory->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; ++i)
		{
			DXGI_ADAPTER_DESC1 desc;
			if (SUCCEEDED(candidate->GetDesc1(&desc))
				&& desc.AdapterLuid.LowPart == requirements.adapterLuid.LowPart
				&& desc.AdapterLuid.HighPart == requirements.adapterLuid.HighPart)
			{
				adapter = candidate;
				CryLogAlways("OpenXR: rendering on adapter %u: %ls", i, desc.Description);
				break;
			}
			candidate->Release();
			candidate = nullptr;
		}
		factory->Release();
	}
	if (!adapter)
		CryLogAlways("OpenXR: WARNING: the adapter the runtime asked for was not found, using the default one");

	D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
	HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device11, &level, &m_context11);
	if (FAILED(hr) && adapter)
	{
		// older runtimes/drivers reject the 11_1 entry outright, retry without it
		hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
			levels + 1, ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION, &m_device11, &level, &m_context11);
	}
	if (adapter)
		adapter->Release();

	if (FAILED(hr) || !m_device11)
	{
		CryLogAlways("OpenXR: D3D11CreateDevice failed: 0x%08x", hr);
		m_device11 = nullptr;
		m_context11 = nullptr;
		return false;
	}
	if (level < requirements.minFeatureLevel)
		CryLogAlways("OpenXR: WARNING: feature level 0x%x is below what the runtime requires (0x%x)", level, requirements.minFeatureLevel);
	else
		CryLogAlways("OpenXR: created D3D11 device (feature level 0x%x)", level);
	return true;
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
				if (Check(xrBeginSession(m_session, &beginInfo), "xrBeginSession"))
					m_sessionRunning = true;
				break;
			}
			case XR_SESSION_STATE_VISIBLE:
				if (previous == XR_SESSION_STATE_FOCUSED)
					m_focusLost = true;
				break;
			case XR_SESSION_STATE_STOPPING:
				m_sessionRunning = false;
				m_frameOpen = false;
				Check(xrEndSession(m_session), "xrEndSession");
				break;
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
	XrResult result = xrBeginFrame(m_session, &beginInfo);
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
	Check(xrEndFrame(m_session, &endInfo), "xrEndFrame");

	if (m_context11)
		m_context11->Flush();

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

	XrSwapchainCreateInfo createInfo;
	Zero(createInfo, XR_TYPE_SWAPCHAIN_CREATE_INFO);
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
	std::vector<XrSwapchainImageD3D11KHR> images(imageCount);
	for (unsigned i = 0; i < imageCount; ++i)
		Zero(images[i], XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR);
	if (!Check(xrEnumerateSwapchainImages(swapchain.handle, imageCount, &imageCount, (XrSwapchainImageBaseHeader*)&images[0]), "xrEnumerateSwapchainImages"))
	{
		DestroySwapchain(swapchain);
		return false;
	}
	swapchain.images.resize(imageCount);
	for (unsigned i = 0; i < imageCount; ++i)
		swapchain.images[i] = images[i].texture;
	swapchain.width = width;
	swapchain.height = height;
	return true;
}

void VROpenXR::DestroySwapchain(Swapchain& swapchain)
{
	if (swapchain.handle != XR_NULL_HANDLE)
		xrDestroySwapchain(swapchain.handle);
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

bool VROpenXR::CopyToSwapchain(Swapchain& swapchain, ID3D11Texture2D* source)
{
	if (swapchain.handle == XR_NULL_HANDLE || !source || !m_context11)
		return false;

	unsigned index = 0;
	if (!AcquireImage(swapchain, index))
		return false;
	m_context11->CopyResource(swapchain.images[index], source);
	ReleaseImage(swapchain);
	return true;
}

bool VROpenXR::FillSwapchain(Swapchain& swapchain, unsigned colorBGRA)
{
	if (swapchain.handle == XR_NULL_HANDLE || !m_context11)
		return false;

	std::vector<unsigned> pixels((size_t)swapchain.width * swapchain.height, colorBGRA);
	unsigned index = 0;
	if (!AcquireImage(swapchain, index))
		return false;
	m_context11->UpdateSubresource(swapchain.images[index], 0, nullptr, &pixels[0], swapchain.width * 4, 0);
	ReleaseImage(swapchain);
	return true;
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

	if (m_context11)
	{
		m_context11->ClearState();
		m_context11->Flush();
		m_context11->Release();
		m_context11 = nullptr;
	}
	if (m_device11)
	{
		m_device11->Release();
		m_device11 = nullptr;
	}
}
