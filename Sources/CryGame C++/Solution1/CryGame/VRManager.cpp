#include "StdAfx.h"
#include "VRManager.h"
#include "Cry_Camera.h"
#include "xplayer.h"
#include "ComPtr.h"
#include "DxvkInterop.h"

#include <d3d9.h>

#include "UISystem.h"
#include "VRRenderer.h"
#include "WeaponClass.h"
#include "XVehicle.h"


HMODULE GetCurrentModule()
{
	HMODULE module = nullptr;
	GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCTSTR)GetCurrentModule, &module);
	return module;
}

VRManager s_VRManager;
VRManager* gVR = &s_VRManager;

const float BinocularWidth = 0.5f;

namespace
{
	// the menu pointer, in the spirit of the SteamVR laser: a thin red beam from the controller to the
	// menu and a small grey box standing in for the controller (Far Cry draws no hands in its menus)
	const unsigned kBeamTextureSize = 8;
	const float kRayWidth = 0.004f;          // meters
	const unsigned kRayColor = 0xFFFF0000;   // B8G8R8A8: opaque red
	const float kWandLength = 0.14f;
	const float kWandWidth = 0.025f;
	const unsigned kWandColor = 0xFFB0B0B8;  // light grey
	const float kMenuClickPress = 0.6f;      // trigger thresholds for a menu click, with hysteresis
	const float kMenuClickRelease = 0.4f;
	const float kLoadingFrameInterval = 1.0f / 30.0f;

	// OpenXR (like OpenVR): x = right, y = up, -z = forward
	// FarCry: x = left, -y = forward, z = up
	// m is a 3x4 row-major matrix, m[row][col]; the columns are the axes, the last column the position
	Matrix34 XrMatrixToFarCry(const float mat[3][4])
	{
		Matrix34 m;
		m.m00 = mat[0][0];
		m.m01 = -mat[0][2];
		m.m02 = -mat[0][1];
		m.m03 = -mat[0][3];
		m.m10 = -mat[2][0];
		m.m11 = mat[2][2];
		m.m12 = mat[2][1];
		m.m13 = mat[2][3];
		m.m20 = -mat[1][0];
		m.m21 = mat[1][2];
		m.m22 = mat[1][1];
		m.m23 = mat[1][3];
		return m;
	}

	void FarCryToXrMatrix(const Matrix34& mat, float res[3][4])
	{
		res[0][0] = mat.m00;
		res[0][1] = -mat.m02;
		res[0][2] = -mat.m01;
		res[0][3] = -mat.m03;
		res[1][0] = -mat.m20;
		res[1][1] = mat.m22;
		res[1][2] = mat.m21;
		res[1][3] = mat.m23;
		res[2][0] = -mat.m10;
		res[2][1] = mat.m12;
		res[2][2] = mat.m11;
		res[2][3] = mat.m13;
	}

	template <typename T>
	T Clamp(T value, T low, T high)
	{
		return value < low ? low : (value > high ? high : value);
	}

	XrRect2Di MakeRect(int x, int y, int width, int height)
	{
		XrRect2Di rect;
		rect.offset.x = x;
		rect.offset.y = y;
		rect.extent.width = width;
		rect.extent.height = height;
		return rect;
	}
}

Matrix34 XrPoseToFarCry(const XrPosef& pose)
{
	XrVector3f x, y, z;
	xrmath::ToAxes(pose.orientation, x, y, z);
	float m[3][4] =
	{
		{ x.x, y.x, z.x, pose.position.x },
		{ x.y, y.y, z.y, pose.position.y },
		{ x.z, y.z, z.z, pose.position.z },
	};
	return XrMatrixToFarCry(m);
}

XrPosef FarCryToXrPose(const Matrix34& mat)
{
	float m[3][4];
	FarCryToXrMatrix(mat, m);
	XrVector3f x = { m[0][0], m[1][0], m[2][0] };
	XrVector3f y = { m[0][1], m[1][1], m[2][1] };
	XrVector3f z = { m[0][2], m[1][2], m[2][2] };
	XrPosef pose;
	pose.orientation = xrmath::FromAxes(x, y, z);
	pose.position.x = m[0][3];
	pose.position.y = m[1][3];
	pose.position.z = m[2][3];
	return pose;
}

struct VRManager::D3DResources
{
	ComPtr<IDirect3DDevice9Ex> device;
	ComPtr<IDirect3DTexture9> hudTexture;
	ComPtr<IDirect3DTexture9> stereoTexture;
	ComPtr<IDirect3DTexture9> eyeTextures[2];

	// The textures above live on dxvk's D3D9 device, which renders with Vulkan; the OpenXR session runs on that
	// same Vulkan device, and every frame the Vulkan images behind the textures are copied into the runtime's
	// swapchains below (see FinishFrame).
	VROpenXR::Swapchain eyeSwapchains[2];
	VROpenXR::Swapchain hudSwapchain;
	VROpenXR::Swapchain stereoSwapchain;
	VROpenXR::Swapchain raySwapchain;
	VROpenXR::Swapchain wandSwapchain;
	bool rayFilled = false;
	bool wandFilled = false;
};

VRManager::VRManager()
{
	m_d3d = new D3DResources;
	m_hmdTransform = Matrix34::CreateIdentity();
	m_headPoseXr = xrmath::Identity();
	m_hud.pose = xrmath::Identity();
	m_hud.pose.position.z = -2.5f;
	m_pointerHit.x = m_pointerHit.y = m_pointerHit.z = 0;
	// a sane menu position until the first head pose comes in: 4 m ahead at eye height
	m_fixedHudTransform = Matrix34::CreateTranslationMat(Vec3(0, -4.0f, 1.6f));
	m_verticalFov = 1.0f;
	m_horizontalFov = 1.0f;
	m_vertRenderScale = 1.0f;
	m_horzRenderScale = 1.0f;
}

VRManager::~VRManager()
{
	// if Shutdown isn't properly called, we will get an infinite hang when trying to dispose of our D3D resources after
	// the game already shut down. So just let go here to avoid that
	m_d3d->device.Detach();
	delete m_d3d;
}

bool VRManager::Init(CXGame *game)
{
	if (m_initialized)
		return true;

	HMODULE module = GetCurrentModule();
	CryLogAlways("Initializing CryVR, base module address: 0x%x", module);

	m_pGame = game;

	// the game renders through dxvk, whose Vulkan device is where the OpenXR session has to live
	IDirect3DDevice9Ex* device = dxvk::GetCreatedDevice();
	if (!device)
	{
		CryError("Failed to get the D3D9 device from dxvk. Is the Far Cry VR d3d9.dll (dxvk) installed in Bin32?");
		return false;
	}
	InitDevice(device);

	VROpenXR::VulkanDevice vulkanDevice;
	if (!QueryVulkanDevice(device, vulkanDevice))
	{
		CryError("Failed to get the Vulkan device from dxvk.");
		return false;
	}

	if (!m_xr.Init(vulkanDevice, this))
	{
		CryError("Failed to initialize OpenXR. Is an OpenXR runtime with Vulkan support (SteamVR, Meta, Virtual Desktop, ...) set as the active runtime and the headset connected? See the log for details.");
		return false;
	}

	// the runtime reports the field of view once the session runs (VROpenXR::Init waits for that); until
	// then render a symmetric 90 degree view
	m_verticalFov = 1.0f;
	m_horizontalFov = 1.0f;
	m_vertRenderScale = 1.0f;
	m_horzRenderScale = 1.0f;
	m_fovKnown = false;
	UpdateFovFromRuntime();

	RegisterCVars();

	m_inputReady = m_input.Init(game, &m_xr);
	m_vrHaptics.Init(game, &m_input);
	if (m_pGame->g_LeftHanded)
		m_pointerHand = m_pGame->g_LeftHanded->GetIVal() != 0 ? VROpenXR::Hand_Left : VROpenXR::Hand_Right;

	m_hmdTransform = Matrix34::CreateIdentity();
	m_referencePosition = Vec3(0, 0, 0);
	m_referenceYaw = 0;
	m_uncommittedReferenceYaw = 0;
	m_uncommittedReferencePosition = Vec3(0, 0, 0);
	m_headPoseValid = m_xr.GetHeadPose(m_headPoseXr);

	m_initialized = true;
	return true;
}

void VRManager::Shutdown()
{
	ReleaseDeviceResources();
	for (int eye = 0; eye < 2; ++eye)
		m_xr.DestroySwapchain(m_d3d->eyeSwapchains[eye]);
	m_xr.DestroySwapchain(m_d3d->hudSwapchain);
	m_xr.DestroySwapchain(m_d3d->stereoSwapchain);
	m_xr.DestroySwapchain(m_d3d->raySwapchain);
	m_xr.DestroySwapchain(m_d3d->wandSwapchain);
	m_d3d->rayFilled = false;
	m_d3d->wandFilled = false;

	// the session (and our command buffers) go while dxvk's device is still around; the device itself is only
	// let go of afterwards, the lock callbacks need it
	m_xr.Shutdown();
	m_d3d->device.Reset();
	m_initialized = false;
}

void VRManager::Update()
{
	if (!m_initialized)
		return;

	m_vrHaptics.Update();
	HandleEvents();
	if (vr_window_width != m_curWindowWidth || vr_window_height != m_curWindowHeight)
	{
		m_pGame->m_pRenderer->ChangeResolution(vr_window_width, vr_window_height, 32, 0, false);
		m_curWindowWidth = vr_window_width;
		m_curWindowHeight = vr_window_height;
	}
}

void VRManager::AwaitFrame()
{
	if (!m_initialized || !m_d3d->device)
		return;

	// waits for the runtime's frame timing and locates head, eyes and controllers for the frame about to
	// be rendered; FinishFrame closes the frame once the game has presented it
	if (!m_xr.BeginFrame())
		return;

	m_headPoseValid = m_xr.GetHeadPose(m_headPoseXr);
	UpdateFovFromRuntime();

	if (m_recalibratePending && m_headPoseValid)
	{
		m_recalibratePending = false;
		RecalibrateView();
	}

	UpdateHmdTransform();
}

void VRManager::HandleEvents()
{
	m_xr.PollEvents();

	if (m_xr.TakeRecenter())
	{
		// the runtime moved its tracking space; the poses of the next frame are relative to the new one
		m_recalibratePending = true;
	}
	if (m_xr.TakeFocusLost())
	{
		// the runtime's own menu (dashboard) took over the controllers
		m_pGame->GotoMenu(false);
	}
	if (m_xr.TakeQuitRequest())
	{
		m_pGame->GetSystem()->Quit();
	}
}

void VRManager::CaptureEye(int eye)
{
	if (!m_d3d->device)
		return;

	if (!m_d3d->eyeTextures[eye])
	{
		CreateEyeTexture(eye);
		if (!m_d3d->eyeTextures[eye])
			return;
	}

	D3DSURFACE_DESC desc;
	m_d3d->eyeTextures[eye]->GetLevelDesc(0, &desc);
	vector2di expectedSize = GetRenderSize();
	if (desc.Width != expectedSize.x || desc.Height != expectedSize.y)
	{
		// recreate with new resolution
		CreateEyeTexture(eye);
		if (!m_d3d->eyeTextures[eye])
			return;
	}

	// acquire and copy the current swap chain buffer to the eye texture
	ComPtr<IDirect3DSurface9> backBuffer;
	m_d3d->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backBuffer.GetAddressOf());
	ComPtr<IDirect3DSurface9> texSurface;
	m_d3d->eyeTextures[eye]->GetSurfaceLevel(0, texSurface.GetAddressOf());
	HRESULT hr = m_d3d->device->StretchRect(backBuffer.Get(), nullptr, texSurface.Get(), nullptr, D3DTEXF_POINT);
	if (hr != S_OK)
	{
		CryLogAlways("ERROR: Capturing HUD failed: %i", hr);
	}
}

void VRManager::CaptureStereo(int eye)
{
	if (!m_d3d->device)
		return;

	if (!m_d3d->stereoTexture)
	{
		CreateStereoTexture();
		if (!m_d3d->stereoTexture)
			return;
	}

	D3DSURFACE_DESC desc;
	m_d3d->stereoTexture->GetLevelDesc(0, &desc);
	vector2di expectedSize = GetRenderSize();
	expectedSize.x *= 2;
	if (desc.Width != expectedSize.x || desc.Height != expectedSize.y)
	{
		// recreate with new resolution
		CreateStereoTexture();
		if (!m_d3d->stereoTexture)
			return;
	}

	// acquire and copy the current back buffer to the right part of the stereo texture
	ComPtr<IDirect3DSurface9> backBuffer;
	m_d3d->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backBuffer.GetAddressOf());
	ComPtr<IDirect3DSurface9> texSurface;
	m_d3d->stereoTexture->GetSurfaceLevel(0, texSurface.GetAddressOf());
	RECT dst;
	dst.top = 0;
	dst.bottom = expectedSize.y;
	if (eye == 0)
	{
		dst.left = 0;
		dst.right = expectedSize.x / 2;
	}
	else
	{
		dst.left = expectedSize.x / 2;
		dst.right = expectedSize.x;
	}
	HRESULT hr = m_d3d->device->StretchRect(backBuffer.Get(), nullptr, texSurface.Get(), &dst, D3DTEXF_POINT);
	if (hr != S_OK)
	{
		CryLogAlways("ERROR: Capturing stereo failed: %i", hr);
	}
}

void VRManager::CaptureHUD()
{
	if (!m_d3d->device)
		return;

	if (!m_d3d->hudTexture)
	{
		CreateHUDTexture();
		if (!m_d3d->hudTexture)
			return;
	}

	D3DSURFACE_DESC desc;
	m_d3d->hudTexture->GetLevelDesc(0, &desc);
	vector2di expectedSize = GetRenderSize();
	if (desc.Width != expectedSize.x || desc.Height != expectedSize.y)
	{
		// recreate with new resolution
		CreateHUDTexture();
		if (!m_d3d->hudTexture)
			return;
	}

	// acquire and copy the current back buffer to the HUD texture
	ComPtr<IDirect3DSurface9> backBuffer;
	m_d3d->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, backBuffer.GetAddressOf());
	ComPtr<IDirect3DSurface9> texSurface;
	m_d3d->hudTexture->GetSurfaceLevel(0, texSurface.GetAddressOf());
	HRESULT hr = m_d3d->device->StretchRect(backBuffer.Get(), nullptr, texSurface.Get(), nullptr, D3DTEXF_POINT);
	if (hr != S_OK)
	{
		CryLogAlways("ERROR: Capturing HUD failed: %i", hr);
	}
}

void VRManager::MirrorEyeToBackBuffer()
{
	if (!gVRRenderer->ShouldRenderVR() || gVRRenderer->ShouldRender2D())
		return;

	int eye = clamp_tpl(vr_mirrored_eye, 0, 1);

	if (!m_d3d->device || !m_d3d->eyeTextures[eye] || m_pGame->IsInMenu())
		return;

	// figure out aspect ratio correction
	float windowAspect = (float)vr_window_width / vr_window_height;
	float vrAspect = (float)m_pGame->m_pRenderer->GetWidth() / m_pGame->m_pRenderer->GetHeight();
	float scale = vrAspect / windowAspect;

	Vec2 size;
	if (scale < 1.f)
	{
		// mirror view is wider than rendered eye
		size.x = 1.f;
		size.y = scale;
	} else
	{
		// rendered eye is wider than mirror view
		size.x = scale;
		size.y = 1.f;
	}
	Vec2 offset(.5f - .5f * size.x, .5f - .5f * size.y);

	struct Vertex
	{
		float x, y, z, w;
		float u, v;
	};
	Vertex vertices[4] =
	{
		{ -0.5f, -0.5f, 0.0f, 1.0f, offset.x, offset.y },
		{ m_pGame->m_pRenderer->GetWidth() - 0.5f, -0.5f, 0.0f, 1.0f, offset.x + size.x, offset.y },
		{ m_pGame->m_pRenderer->GetWidth() - 0.5f, m_pGame->m_pRenderer->GetHeight() - 0.5f, 0.0f, 1.0f, offset.x + size.x, offset.y + size.y },
		{ -0.5f, m_pGame->m_pRenderer->GetHeight() - 0.5f, 0.0f, 1.0f, offset.x, offset.y + size.y },
	};

	m_pGame->m_pRenderer->ResetToDefault();

	// save current render state
	IDirect3DStateBlock9* stateBlock = nullptr;
	m_d3d->device->CreateStateBlock(D3DSBT_ALL, &stateBlock);

	// set state for fullscreen quad
	m_d3d->device->SetRenderState(D3DRS_LIGHTING, FALSE);
	m_d3d->device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	m_d3d->device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
	m_d3d->device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	m_d3d->device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVDESTALPHA);
	m_d3d->device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_DESTALPHA);
	m_d3d->device->SetRenderState(D3DRS_VERTEXBLEND, FALSE);
	m_d3d->device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	m_d3d->device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
	m_d3d->device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	m_d3d->device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	m_d3d->device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	m_d3d->device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	m_d3d->device->SetTexture(0, m_d3d->eyeTextures[eye].Get());
	m_d3d->device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	m_d3d->device->SetVertexShader(nullptr);
	m_d3d->device->SetPixelShader(nullptr);

	// draw quad
	m_d3d->device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, vertices, sizeof(Vertex));

	// restore state
	if (stateBlock)
	{
		stateBlock->Apply();
		stateBlock->Release();
	}
}

void VRManager::SetDevice(IDirect3DDevice9Ex *device)
{
	if (device != m_d3d->device.Get())
		InitDevice(device);
}

void VRManager::FinishFrame()
{
	if (!m_initialized || !m_d3d->device)
		return;

	// Normally VRRenderer::Render opened this frame (AwaitFrame) and rendered both eyes into it. While a
	// level loads the game presents on its own, without rendering through VRRenderer: then open a frame
	// just for the loading screen, which is shown on the HUD quad over a black background. The loader
	// presents often, so those frames are limited to a modest rate (the runtime keeps showing the last one).
	const bool eyesRendered = m_xr.IsFrameOpen();
	if (!eyesRendered)
	{
		float now = m_pGame->GetSystem()->GetITimer()->GetAsyncCurTime();
		if (now - m_lastStandaloneFrameTime < kLoadingFrameInterval)
			return;
		m_lastStandaloneFrameTime = now;
		if (!m_xr.BeginFrame())
			return;
	}

	if (!m_xr.ShouldRender())
	{
		// the runtime does not want a picture right now (headset idle, another application in front)
		m_xr.EndFrame(nullptr, nullptr, 0);
		m_wasBinocular = m_pGame->AreBinocularsActive();
		return;
	}

	// The textures were filled through D3D9; dxvk has the draws in its command stream, not necessarily on the GPU yet.
	// Move the Vulkan images behind them into the copy source layout (that transition joins the same stream), then
	// have dxvk flush everything to its queue and keep the queue to ourselves: from here to EndFrame the copies
	// and the runtime's own synchronization are the only things submitted to it.
	SubmissionImage eyeImages[2], stereoImage, hudImage;
	bool haveEyes = eyesRendered
		&& PrepareTextureForSubmission(m_d3d->eyeTextures[0].Get(), eyeImages[0])
		&& PrepareTextureForSubmission(m_d3d->eyeTextures[1].Get(), eyeImages[1]);
	const bool haveStereo = m_stereoVisible && PrepareTextureForSubmission(m_d3d->stereoTexture.Get(), stereoImage);
	const bool haveHud = PrepareTextureForSubmission(m_d3d->hudTexture.Get(), hudImage);

	m_xr.LockQueue(true);

	// --- eyes: projection layer, cut down to each eye's real (asymmetric) field of view ------------
	VROpenXR::ProjectionView projection[2];
	for (int eye = 0; eye < 2 && haveEyes; ++eye)
	{
		const VROpenXR::SourceImage& source = eyeImages[eye].source;
		VROpenXR::Swapchain& swapchain = m_d3d->eyeSwapchains[eye];
		if (!m_xr.CreateSwapchain(swapchain, source.width, source.height) || !m_xr.CopyToSwapchain(swapchain, source))
		{
			haveEyes = false;
			break;
		}

		float left, right, top, bottom;
		GetEffectiveRenderLimits(eye, &left, &right, &top, &bottom);
		int x0 = Clamp((int)(left * source.width + 0.5f), 0, (int)source.width - 1);
		int x1 = Clamp((int)(right * source.width + 0.5f), x0 + 1, (int)source.width);
		int y0 = Clamp((int)(top * source.height + 0.5f), 0, (int)source.height - 1);
		int y1 = Clamp((int)(bottom * source.height + 0.5f), y0 + 1, (int)source.height);
		projection[eye].swapchain = &swapchain;
		projection[eye].rect = MakeRect(x0, y0, x1 - x0, y1 - y0);
		// the field of view that rect really covers (rounded to pixels, clamped to the rendered image)
		projection[eye].fov.angleLeft = atanf((2.0f * x0 / source.width - 1.0f) * m_horizontalFov);
		projection[eye].fov.angleRight = atanf((2.0f * x1 / source.width - 1.0f) * m_horizontalFov);
		projection[eye].fov.angleUp = atanf((1.0f - 2.0f * y0 / source.height) * m_verticalFov);
		projection[eye].fov.angleDown = atanf((1.0f - 2.0f * y1 / source.height) * m_verticalFov);
	}

	// --- quads: 3D cinema, HUD / menu / binoculars / scope, menu pointer ---------------------------
	VROpenXR::QuadLayer quads[16];
	int quadCount = 0;

	if (haveStereo)
	{
		// the stereo texture holds the left and the right eye side by side; both quads share its placement
		const VROpenXR::SourceImage& source = stereoImage.source;
		VROpenXR::Swapchain& swapchain = m_d3d->stereoSwapchain;
		if (m_xr.CreateSwapchain(swapchain, source.width, source.height) && m_xr.CopyToSwapchain(swapchain, source))
		{
			const int halfWidth = (int)source.width / 2;
			for (int eye = 0; eye < 2; ++eye)
			{
				VROpenXR::QuadLayer& quad = quads[quadCount++];
				quad.swapchain = &swapchain;
				quad.rect = MakeRect(eye * halfWidth, 0, halfWidth, (int)source.height);
				quad.headLocked = false;
				quad.pose = m_hud.pose;
				quad.width = m_hud.width;
				quad.height = m_hud.width * source.height / (float)halfWidth;
				quad.alphaBlend = false;
				quad.eye = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
			}
		}
	}

	if (haveHud)
	{
		const VROpenXR::SourceImage& source = hudImage.source;
		VROpenXR::Swapchain& swapchain = m_d3d->hudSwapchain;
		if (m_xr.CreateSwapchain(swapchain, source.width, source.height) && m_xr.CopyToSwapchain(swapchain, source))
		{
			VROpenXR::QuadLayer& quad = quads[quadCount++];
			quad.swapchain = &swapchain;
			quad.rect = MakeRect(0, 0, (int)source.width, (int)source.height);
			quad.headLocked = m_hud.headLocked;
			quad.pose = m_hud.pose;
			quad.width = m_hud.width;
			quad.height = m_hud.width * source.height / (float)source.width;
			quad.alphaBlend = !m_hud.opaque;
			quad.eye = XR_EYE_VISIBILITY_BOTH;
		}
	}

	// (only with a rendered frame: while a level loads nobody updates the pointer)
	if (m_pointerVisible && eyesRendered)
		quadCount += BuildPointerQuads(quads + quadCount, (int)(sizeof(quads) / sizeof(quads[0])) - quadCount);

	m_xr.EndFrame(haveEyes ? projection : nullptr, quads, quadCount);

	m_xr.UnlockQueue();

	// give the textures back to dxvk in the layout it expects them in
	PostSubmissionTransitionTexture(eyeImages[0]);
	PostSubmissionTransitionTexture(eyeImages[1]);
	PostSubmissionTransitionTexture(stereoImage);
	PostSubmissionTransitionTexture(hudImage);

	m_wasBinocular = m_pGame->AreBinocularsActive();
}

vector2di VRManager::GetRenderSize() const
{
	if (!m_initialized)
		return vector2di(1280, 800);

	unsigned width, height;
	m_xr.GetRecommendedEyeSize(width, height);
	height = (unsigned)(height * m_vertRenderScale);
	width = (unsigned)(height * m_horizontalFov / m_verticalFov);
	return vector2di(width, height);
}

void VRManager::ModifyViewCamera(int eye, CCamera& cam)
{
	if (IsEquivalent(cam.GetPos(), Vec3(0, 0, 0), VEC_EPSILON))
	{
		// no valid camera set, leave it
		return;
	}

	if (!m_initialized)
	{
		if (eye == 1)
		{
			Vec3 pos = cam.GetPos();
			pos.x += 0.1f;
			cam.SetPos(pos);
		}
		return;
	}

	if (m_pGame->AreBinocularsActive() || m_wasBinocular)
	{
		cam = m_binocularOriginalPlayerCam;
	}

	Ang3 angles = cam.GetAngles();
	Vec3 position = cam.GetPos();
	position.z -= m_referenceHeight;

	CPlayer* player = m_pGame->GetLocalPlayer();
	if (player && !m_pGame->IsCutSceneActive())
	{
		position = player->GetVRBasePos();
	}

	angles = Deg2Rad(angles);
	// eliminate pitch and roll
	angles.y = 0;
	angles.x = 0;

	if (eye == 0)
	{
		// manage the aiming deadzone in which the camera should not be rotated
		float yawAngle = DEG2RAD(AngleMod(RAD2DEG(angles.z)));
		float yawDiff = yawAngle - m_prevViewYaw;
		if (yawDiff < -gf_PI)
			yawDiff += 2 * gf_PI;
		else if (yawDiff > gf_PI)
			yawDiff -= 2 * gf_PI;

		float maxDiff = vr_yaw_deadzone_angle * gf_PI / 180.f;
		if (yawDiff > maxDiff)
			m_prevViewYaw += yawDiff - maxDiff;
		if (yawDiff < -maxDiff)
			m_prevViewYaw += yawDiff + maxDiff;
		if (m_prevViewYaw > gf_PI)
			m_prevViewYaw -= 2*gf_PI;
		if (m_prevViewYaw < -gf_PI)
			m_prevViewYaw += 2*gf_PI;

		CPlayer *pPlayer = 0;
		if (m_pGame->GetMyPlayer())
		{
			m_pGame->GetMyPlayer()->GetContainer()->QueryContainerInterface(CIT_IPLAYER,(void **)&pPlayer);
		}
		if (pPlayer && pPlayer->GetVehicle())
		{
			// don't use this while in a vehicle, it feels off
			m_prevViewYaw = angles.z;
		}
	}
	if (!UseMotionControllers())
		angles.z = m_prevViewYaw;

	Matrix34 viewMat;
	viewMat.SetRotationXYZ(angles, position);

	// eye relative to the head: both were located for this frame in the same tracking space
	Matrix34 eyeMat = Matrix34::CreateIdentity();
	XrPosef eyePose;
	if (m_headPoseValid && m_xr.GetEyePose(eye, eyePose))
		eyeMat = XrPoseToFarCry(xrmath::Compose(xrmath::Inverse(m_headPoseXr), eyePose));
	else
		eyeMat.SetTranslation(Vec3(eye == 0 ? 0.032f : -0.032f, 0, 0));   // FarCry x = left
	Matrix34 headMat = m_hmdTransform;
	viewMat = viewMat * headMat * eyeMat;

	position = viewMat.GetTranslation();
	cam.SetPos(position);
	angles.SetAnglesXYZ(Matrix33(viewMat));
	angles.Rad2Deg();
	cam.SetAngle(angles);

	// we don't have obvious access to the projection matrix, and the camera code is written with symmetric projection in mind
	// for now, set up a symmetric FOV and cut off parts of the image during submission
	vector2di renderSize = GetRenderSize();
	float vertFovAngle = atanf(m_verticalFov) * 2;
	float horzFovAngle = vertFovAngle * renderSize.x / (float)renderSize.y;
	cam.Init(renderSize.x, renderSize.y, horzFovAngle, cam.GetZMax(), 0, cam.GetZMin());
	cam.Update();
}

void VRManager::Modify2DCamera(CCamera& cam)
{
	// in some instances (e.g. binoculars, weapon zoom) we still want to include head movements in the camera orientation

	if (IsEquivalent(cam.GetPos(), Vec3(0, 0, 0), VEC_EPSILON))
	{
		// no valid camera set, leave it
		return;
	}

	if (m_pGame->IsCutSceneActive() || IsDrivingVehicleInCinemaMode())
		return;

	if (m_pGame->AreBinocularsActive())
	{
		// already corrected in player cam by necessity - otherwise, the motion tracking markers just don't display at the right position
		return;
	}

	Ang3 angles = cam.GetAngles();
	Vec3 position = cam.GetPos();
	position.z -= m_referenceHeight;

	CPlayer* player = m_pGame->GetLocalPlayer();
	if (player && !m_pGame->IsCutSceneActive())
	{
		position = player->GetVRBasePos();
	}

	angles = Deg2Rad(angles);
	// eliminate pitch and roll
	angles.y = 0;
	angles.x = 0;

	Matrix34 viewMat = Matrix34::CreateRotationXYZ(angles, position);

	Matrix34 headMat = m_hmdTransform;
	Matrix34 modifiedViewMat = viewMat * headMat;

	position = modifiedViewMat.GetTranslation();
	cam.SetPos(position);
	angles.SetAnglesXYZ(Matrix33(modifiedViewMat));
	angles.Rad2Deg();
	cam.SetAngle(angles);

	if (player && player->IsWeaponZoomActive())
	{
		// set camera to weapon firing pos, instead
		Vec3 muzzlePos, muzzleAngles;
		player->GetFirePosAngles(muzzlePos, muzzleAngles);
		cam.SetPos(muzzlePos);
		cam.SetAngle(muzzleAngles);
	}
}

void VRManager::Modify3DCamera(int eye, CCamera& cam)
{
	// start from the 2D camera setup
	Modify2DCamera(cam);

	float eyeShift = 0.025f;
	//cam.SetZMin(0.75f);
	cam.Update();

	// shift position slightly based on eye
	Matrix34 camTransform = Matrix34::CreateRotationXYZ(Deg2Rad(cam.GetAngles()), cam.GetPos());
	Matrix34 shift = Matrix34::CreateTranslationMat(Vec3(eye == 0 ? eyeShift : -eyeShift, 0, 0));
	camTransform = camTransform * shift;

	cam.SetPos(camTransform.GetTranslation() - 0.f * camTransform.GetForward());
	cam.SetAngle(ToAnglesDeg(camTransform));
}

void VRManager::ModifyBinocularCamera(IEntityCamera* cam)
{
	m_binocularOriginalPlayerCam = cam->GetCamera();

	if (!cam || !UseMotionControllers() || !m_pGame->AreBinocularsActive())
		return;

	Ang3 angles = cam->GetAngles();
	Vec3 position = cam->GetPos();
	position.z -= m_referenceHeight;

	CPlayer* player = m_pGame->GetLocalPlayer();
	if (player && !m_pGame->IsCutSceneActive())
	{
		position = player->GetVRBasePos();
	}

	angles = Deg2Rad(angles);
	// eliminate pitch and roll
	angles.y = 0;
	angles.x = 0;
	Matrix34 viewMat = Matrix34::CreateRotationXYZ(angles, position);

	// set camera to off hand position, instead
	Matrix34 offset = Matrix34::CreateTranslationMat(Vec3(-vr_binocular_size / 2, 0, vr_binocular_size / 2));
	Matrix34 controllerTransform = GetControllerTransform(m_pGame->g_LeftHanded->GetIVal() == 1 ? 1 : 0);
	Matrix34 modifiedViewMat = viewMat * controllerTransform * offset;
	m_curBinocularPos = modifiedViewMat.GetTranslation();
	cam->SetPos(m_curBinocularPos);
	angles.SetAnglesXYZ(Matrix33(modifiedViewMat));
	angles.Rad2Deg();

	// smooth rotation for a more stable zoom
	Vec3 smoothedAngles = angles;
	float factor = 0.025 * (DEFAULT_FOV / cam->GetFov());
	float yawPitchDecay = powf(2.f, -m_pGame->GetSystem()->GetITimer()->GetFrameTime() / factor);
	smoothedAngles.z = angles.z + GetAngleDifference360(m_curBinocularAngles.z, angles.z) * yawPitchDecay;
	smoothedAngles.x = angles.x + GetAngleDifference360(m_curBinocularAngles.x, angles.x) * yawPitchDecay;
	m_curBinocularAngles = smoothedAngles;

	cam->SetAngles(smoothedAngles);
}

void VRManager::GetEffectiveRenderLimits(int eye, float* left, float* right, float* top, float* bottom)
{
	// the eyes are rendered with a symmetric projection of +-m_horizontalFov / +-m_verticalFov (tangents);
	// this is the part of that image the runtime should show for the eye's real field of view
	XrFovf fov;
	if (!m_xr.GetEyeFov(eye, fov))
	{
		*left = 0;
		*right = 1;
		*top = 0;
		*bottom = 1;
		return;
	}
	*left = Clamp(0.5f + 0.5f * tanf(fov.angleLeft) / m_horizontalFov, 0.0f, 1.0f);
	*right = Clamp(0.5f + 0.5f * tanf(fov.angleRight) / m_horizontalFov, 0.0f, 1.0f);
	*top = Clamp(0.5f - 0.5f * tanf(fov.angleUp) / m_verticalFov, 0.0f, 1.0f);
	*bottom = Clamp(0.5f - 0.5f * tanf(fov.angleDown) / m_verticalFov, 0.0f, 1.0f);
}

void VRManager::UpdateFovFromRuntime()
{
	XrFovf fov[2];
	if (!m_xr.GetEyeFov(0, fov[0]) || !m_xr.GetEyeFov(1, fov[1]))
		return;

	const float ll = fabsf(tanf(fov[0].angleLeft)), lr = fabsf(tanf(fov[0].angleRight));
	const float lu = fabsf(tanf(fov[0].angleUp)), ld = fabsf(tanf(fov[0].angleDown));
	const float rl = fabsf(tanf(fov[1].angleLeft)), rr = fabsf(tanf(fov[1].angleRight));
	const float ru = fabsf(tanf(fov[1].angleUp)), rd = fabsf(tanf(fov[1].angleDown));
	const float vertical = max(max(lu, ld), max(ru, rd));
	const float horizontal = max(max(ll, lr), max(rl, rr));
	const float minVertical = min(lu + ld, ru + rd);
	if (vertical <= 0.01f || horizontal <= 0.01f || minVertical <= 0.01f)
		return;

	// the runtime may refine its numbers after the first frames; only react to real changes, the game
	// changes its render resolution whenever GetRenderSize() moves
	if (m_fovKnown && fabsf(vertical - m_verticalFov) < 0.01f * m_verticalFov && fabsf(horizontal - m_horizontalFov) < 0.01f * m_horizontalFov)
		return;

	CryLogAlways(" Left eye - l: %.2f  r: %.2f  u: %.2f  d: %.2f", -ll, lr, lu, -ld);
	CryLogAlways("Right eye - l: %.2f  r: %.2f  u: %.2f  d: %.2f", -rl, rr, ru, -rd);
	m_verticalFov = vertical;
	m_horizontalFov = horizontal;
	m_vertRenderScale = 2.f * vertical / minVertical;
	m_fovKnown = true;
	CryLogAlways("VR vert fov: %.2f  horz fov: %.2f  vert scale: %.2f", m_verticalFov, m_horizontalFov, m_vertRenderScale);
}

void VRManager::ProcessInput()
{
	bool firstValidPose = m_referenceHeight < 0 && m_headPoseValid;
	if (firstValidPose)
	{
		RecalibrateView();
	}

	// the HUD placement functions below decide what is shown this frame
	m_stereoVisible = false;
	m_pointerVisible = false;

	if ((m_pGame->IsInMenu() || m_pGame->GetSystem()->GetIConsole()->IsOpened()) && UseMotionControllers())
	{
		if (!m_wasInMenu)
		{
			CryLogAlways("Entering menu...");
			m_wasInMenu = true;
			m_buttonPressed = false;
			// whatever is held right now does not count as a press in the menu
			m_menuAnyButtonDown = false;
			for (int hand = 0; hand < 2; ++hand)
			{
				const VROpenXR::HandState& state = m_xr.GetHand(hand);
				m_menuTriggerDown[hand] = state.active && state.trigger > kMenuClickPress;
				m_menuAnyButtonDown = m_menuAnyButtonDown || m_menuTriggerDown[hand]
					|| (state.active && (state.squeeze > kMenuClickPress || state.primary || state.secondary || state.stickClick || state.padClick));
			}
			m_menuClickDown = m_menuTriggerDown[m_pointerHand];
			RecalibrateView();

			m_vrHaptics.StopAllEffects();
		}
		SetHudInFrontOfPlayer();
		ProcessMenuInput();
		return;
	}

	if (m_wasInMenu)
	{
		m_wasInMenu = false;
		m_buttonPressed = false;
		RecalibrateView();
	}

	if (!UseMotionControllers())
	{
		SetHudAttachedToHead();
		return;
	}

	CPlayer* player = m_pGame->GetLocalPlayer();
	if (player && player->IsWeaponZoomActive())
		SetHudAsWeaponZoom();
	else if (m_pGame->AreBinocularsActive())
		SetHudAsBinoculars();
	else if (m_pGame->IsCutSceneActive() && gVR->vr_cutscenes_cinema_mode > 0)
		SetHudInFrontOfPlayer();
	else if (IsDrivingVehicleInCinemaMode())
		SetHudInFrontOfPlayer();
	else
		SetHudAttachedToHead();

	m_input.ProcessInput();
	ProcessRoomscale();
}

void VRManager::ProcessMenuInput()
{
	m_mousePressed = false;
	m_mouseReleased = false;
	m_pGame->RequestStopVideo(false);

	// the game does not get controller input while the menu is up, but the input layer still has to see
	// the buttons so that nothing held right now registers as a fresh press once the game resumes
	m_input.Poll();

	// the pointer belongs to the hand whose trigger was pressed last; any button counts for skipping videos
	bool anyButton = false;
	for (int hand = 0; hand < 2; ++hand)
	{
		const VROpenXR::HandState& state = m_xr.GetHand(hand);
		bool trigger = state.active && state.trigger > (m_menuTriggerDown[hand] ? kMenuClickRelease : kMenuClickPress);
		if (trigger && !m_menuTriggerDown[hand])
			m_pointerHand = hand;
		m_menuTriggerDown[hand] = trigger;
		anyButton = anyButton || trigger || (state.active && (state.squeeze > kMenuClickPress || state.primary || state.secondary || state.stickClick || state.padClick));
	}

	UpdateMenuPointer();
	m_pointerVisible = vr_menu_pointer != 0;

	if (m_pointerValid)
	{
		IMouse* mouse = m_pGame->GetSystem()->GetIInput()->GetIMouse();
		mouse->SetVScreenX(800.f * m_pointerU);
		mouse->SetVScreenY(600.f * m_pointerV);
	}

	// the pointing hand's trigger is the left mouse button
	const bool click = m_menuTriggerDown[m_pointerHand];
	if (click && !m_menuClickDown && m_pointerValid)
		m_mousePressed = true;
	if (!click && m_menuClickDown)
		m_mouseReleased = true;
	m_menuClickDown = click;

	// holding any button for half a second skips a video
	const float now = m_pGame->GetSystem()->GetITimer()->GetAsyncCurTime();
	if (anyButton && !m_menuAnyButtonDown)
	{
		m_buttonPressed = true;
		m_lastTimeButtonPressed = now;
	}
	if (!anyButton)
		m_buttonPressed = false;
	m_menuAnyButtonDown = anyButton;

	if (m_buttonPressed && now - m_lastTimeButtonPressed >= 0.5f)
	{
		m_pGame->RequestStopVideo(true);
		m_buttonPressed = false;
	}
}

void VRManager::UpdateMenuPointer()
{
	m_pointerValid = false;

	XrPosef aim;
	if (m_hud.headLocked || !m_xr.GetHandPose(m_pointerHand, true, aim))
		return;

	// the menu quad: center, in-plane axes and its normal (which faces the viewer)
	XrVector3f axisX, axisY, normal;
	xrmath::ToAxes(m_hud.pose.orientation, axisX, axisY, normal);
	const vector2di hudSize = GetRenderSize();
	const float width = m_hud.width;
	const float height = hudSize.x > 0 ? width * hudSize.y / (float)hudSize.x : width * 0.75f;

	// ray from the controller against the quad's plane
	const XrVector3f origin = aim.position;
	const XrVector3f dir = xrmath::Forward(aim.orientation);
	const float denom = xrmath::Dot(dir, normal);
	if (denom > -1e-4f)
		return;   // pointing away from the menu
	const float t = xrmath::Dot(xrmath::Sub(m_hud.pose.position, origin), normal) / denom;
	if (t <= 0.0f)
		return;
	const XrVector3f local = xrmath::Sub(xrmath::Add(origin, xrmath::Scale(dir, t)), m_hud.pose.position);
	float u = xrmath::Dot(local, axisX) / width + 0.5f;
	float v = 0.5f - xrmath::Dot(local, axisY) / height;

	// a small margin so the cursor can reach the edges comfortably
	if (u < -0.1f || u > 1.1f || v < -0.1f || v > 1.1f)
		return;
	u = Clamp(u, 0.0f, 1.0f);
	v = Clamp(v, 0.0f, 1.0f);

	m_pointerU = u;
	m_pointerV = v;
	m_pointerHit = xrmath::Add(m_hud.pose.position, xrmath::Add(xrmath::Scale(axisX, (u - 0.5f) * width), xrmath::Scale(axisY, (0.5f - v) * height)));
	m_pointerValid = true;
}

bool VRManager::BuildBeamQuad(const XrVector3f& from, const XrVector3f& to, float width, const XrVector3f& eye, VROpenXR::QuadLayer& quad)
{
	// a thin quad along the beam, turned to face the viewer
	XrVector3f along = xrmath::Sub(to, from);
	const float length = xrmath::Length(along);
	if (length < 0.01f)
		return false;
	const XrVector3f axisY = xrmath::Scale(along, 1.0f / length);
	const XrVector3f center = xrmath::Scale(xrmath::Add(from, to), 0.5f);
	XrVector3f toEye = xrmath::Normalize(xrmath::Sub(eye, center));
	if (xrmath::Length(toEye) < 0.5f)
	{
		toEye.x = 0; toEye.y = 0; toEye.z = 1;
	}
	XrVector3f axisX = xrmath::Cross(axisY, toEye);
	if (xrmath::Length(axisX) < 0.001f)
		return false;   // the beam points straight at the viewer
	axisX = xrmath::Normalize(axisX);
	const XrVector3f axisZ = xrmath::Cross(axisX, axisY);

	quad.pose.orientation = xrmath::FromAxes(axisX, axisY, axisZ);
	quad.pose.position = center;
	quad.width = width;
	quad.height = length;
	quad.headLocked = false;
	quad.alphaBlend = false;
	quad.eye = XR_EYE_VISIBILITY_BOTH;
	return true;
}

bool VRManager::BuildFaceQuad(const XrVector3f& center, const XrVector3f& x, const XrVector3f& y, const XrVector3f& normal, float w, float h, const XrVector3f& eye, VROpenXR::QuadLayer& quad)
{
	// one face of the wand box; faces turned away from the viewer are left out, so the visible faces of
	// the convex box never overlap and no depth buffer is needed
	if (xrmath::Dot(normal, xrmath::Sub(eye, center)) <= 0.0f)
		return false;

	quad.pose.orientation = xrmath::FromAxes(x, y, normal);
	quad.pose.position = center;
	quad.width = w;
	quad.height = h;
	quad.headLocked = false;
	quad.alphaBlend = false;
	quad.eye = XR_EYE_VISIBILITY_BOTH;
	return true;
}

int VRManager::BuildPointerQuads(VROpenXR::QuadLayer* quads, int maxQuads)
{
	XrPosef aim;
	if (maxQuads < 1 || !m_xr.GetHandPose(m_pointerHand, true, aim))
		return 0;

	// the pointer was worked out before this frame's poses came in; redo it so the ray meets the menu
	// where the controller points now (the cursor itself moves with the next ProcessMenuInput)
	UpdateMenuPointer();

	// tiny solid-color textures, filled once
	if (m_d3d->wandSwapchain.handle == XR_NULL_HANDLE && m_xr.CreateSwapchain(m_d3d->wandSwapchain, kBeamTextureSize, kBeamTextureSize))
		m_d3d->wandFilled = m_xr.FillSwapchain(m_d3d->wandSwapchain, kWandColor);
	if (m_d3d->raySwapchain.handle == XR_NULL_HANDLE && m_xr.CreateSwapchain(m_d3d->raySwapchain, kBeamTextureSize, kBeamTextureSize))
		m_d3d->rayFilled = m_xr.FillSwapchain(m_d3d->raySwapchain, kRayColor);

	const XrVector3f eye = m_headPoseValid ? m_headPoseXr.position : xrmath::Identity().position;
	const XrVector3f fwd = xrmath::Forward(aim.orientation);
	const XrVector3f right = xrmath::Right(aim.orientation);
	const XrVector3f up = xrmath::Up(aim.orientation);
	const XrVector3f negFwd = xrmath::Scale(fwd, -1.0f);
	const XrVector3f negRight = xrmath::Scale(right, -1.0f);
	const XrVector3f negUp = xrmath::Scale(up, -1.0f);
	int count = 0;

	// the wand: a solid box around the aim pose, running along the controller's forward direction
	if (m_d3d->wandFilled)
	{
		const float halfLength = kWandLength * 0.5f;
		const float halfWidth = kWandWidth * 0.5f;
		const XrVector3f center = xrmath::Sub(aim.position, xrmath::Scale(fwd, kWandLength * 0.1f));

		// each face: outward direction and extent, in-plane axes x and y with normal = x cross y, and size
		struct Face
		{
			XrVector3f dir; float extent;
			XrVector3f x; XrVector3f y; XrVector3f normal;
			float w, h;
		};
		const Face faces[6] =
		{
			{ right,    halfWidth,  fwd,   up,    right,    kWandLength, kWandWidth },
			{ negRight, halfWidth,  up,    fwd,   negRight, kWandWidth,  kWandLength },
			{ up,       halfWidth,  right, fwd,   up,       kWandWidth,  kWandLength },
			{ negUp,    halfWidth,  fwd,   right, negUp,    kWandLength, kWandWidth },
			{ fwd,      halfLength, up,    right, fwd,      kWandWidth,  kWandWidth },
			{ negFwd,   halfLength, right, up,    negFwd,   kWandWidth,  kWandWidth },
		};
		for (int i = 0; i < 6 && count < maxQuads; ++i)
		{
			const XrVector3f faceCenter = xrmath::Add(center, xrmath::Scale(faces[i].dir, faces[i].extent));
			if (BuildFaceQuad(faceCenter, faces[i].x, faces[i].y, faces[i].normal, faces[i].w, faces[i].h, eye, quads[count]))
			{
				quads[count].swapchain = &m_d3d->wandSwapchain;
				quads[count].rect = MakeRect(0, 0, kBeamTextureSize, kBeamTextureSize);
				++count;
			}
		}
	}

	// the ray: from just in front of the controller to the point it hits on the menu
	if (m_d3d->rayFilled && m_pointerValid && count < maxQuads)
	{
		const XrVector3f from = xrmath::Add(aim.position, xrmath::Scale(fwd, 0.06f));
		if (BuildBeamQuad(from, m_pointerHit, kRayWidth, eye, quads[count]))
		{
			quads[count].swapchain = &m_d3d->raySwapchain;
			quads[count].rect = MakeRect(0, 0, kBeamTextureSize, kBeamTextureSize);
			++count;
		}
	}

	return count;
}

bool VRManager::UseMotionControllers() const
{
	return (m_inputReady && vr_enable_motion_controllers);
}

Matrix34 VRManager::GetControllerTransform(int hand)
{
	Ang3 refAngles(0, 0, m_referenceYaw);
	Matrix33 refTransform;
	refTransform.SetRotationXYZ(refAngles);
	refTransform.Transpose();
	Matrix34 rawControllerTransform = m_input.GetControllerTransform(hand);
	rawControllerTransform.SetTranslation(rawControllerTransform.GetTranslation() - m_referencePosition);
	return refTransform * rawControllerTransform;
}

void VRManager::UpdatePlayerTurnOffset(float yawDeltaDeg)
{
	m_uncommittedReferenceYaw += DEG2RAD(yawDeltaDeg);
	//UpdateHmdTransform();
}

void VRManager::UpdatePlayerMoveOffset(const Vec3& offset, const Ang3& hmdAnglesDeg)
{
	// transform offset back into raw HMD space
	Ang3 refAngles(0, 0, m_uncommittedReferenceYaw - DEG2RAD(hmdAnglesDeg.z));
	Matrix33 refTransform = Matrix33::CreateRotationXYZ(refAngles);

	Vec3 rawOffset = refTransform * offset;
	rawOffset.z = 0;
	m_uncommittedReferencePosition += rawOffset;
	UpdateHmdTransform();
}

void VRManager::OnPostPlayerCameraUpdate() 
{
	CommitYawAndOffsetChanges();
}

void VRManager::CommitYawAndOffsetChanges() 
{
	m_referenceYaw = m_uncommittedReferenceYaw;
	m_referencePosition = m_uncommittedReferencePosition;
	m_referencePosition.z = 0;
	UpdateHmdTransform();
}

bool VRManager::IsDrivingVehicleInCinemaMode()
{
	if (CPlayer* player = m_pGame->GetLocalPlayer())
	{
		return player->GetVehicle() && player->GetVehicle()->GetUserInState(CPlayer::PVS_DRIVER) == player && vr_vehicles_cinema_mode != 0;
	}

	return false;
}

void VRManager::ProcessRoomscale()
{
	CPlayer* player = m_pGame->GetLocalPlayer();
	if (!player || m_pGame->IsCutSceneActive() || m_pGame->IsInMenu())
	{
		m_skippedRoomscaleMovement = true;
		return;
	}

	if (m_skippedRoomscaleMovement)
	{
		// if we previously skipped roomscale movement, reset our offsets to not accidentally move way too much
		RecalibrateView();
		m_skippedRoomscaleMovement = false;
	}

	m_referenceHeight = max(m_referenceHeight, m_hmdTransform.GetTranslation().z);

	if (m_pGame->GetClient())
	{
		m_pGame->GetClient()->EnableMotionControls(m_pGame->g_LeftHanded->GetIVal() == 0);
		Vec3 hmdPos = m_hmdTransform.GetTranslation();
		Ang3 hmdAngles = ToAnglesDeg(m_hmdTransform);
		m_pGame->GetClient()->UpdateHmdTransform(hmdPos, hmdAngles, m_referenceHeight);

		for (int i = 0; i < 2; ++i)
		{
			Matrix34 controllerTransform = GetControllerTransform(i);
			Vec3 controllerPos = controllerTransform.GetTranslation();
			Ang3 controllerAngles = ToAnglesDeg(controllerTransform);
			m_pGame->GetClient()->UpdateControllerTransform(i, controllerPos, controllerAngles);
		}
	}
}

void VRManager::RecalibrateView()
{
	if (!m_headPoseValid)
		return;

	CryLogAlways("Recalibrating view");
	Matrix34 rawHmdTransform = XrPoseToFarCry(m_headPoseXr);
	Ang3 rawAngles;
	rawAngles.SetAnglesXYZ((Matrix33)rawHmdTransform);
	m_referencePosition = rawHmdTransform.GetTranslation();
	m_referenceHeight = m_referencePosition.z;
	m_referencePosition.z = 0;
	m_referenceYaw = rawAngles.z;
	UpdateHmdTransform();

	// recalibrate menu HUD positioning
	m_fixedHudTransform = XrPoseToFarCry(m_headPoseXr);
	// erase pitch and roll
	Ang3 angles;
	angles.SetAnglesXYZ((Matrix33)m_fixedHudTransform);
	angles.x = angles.y = 0;
	m_fixedHudTransform.SetRotationXYZ(angles, m_fixedHudTransform.GetTranslation());
	Vec3 dir = -((Matrix33)m_fixedHudTransform).GetColumn(1);
	Vec3 pos = m_fixedHudTransform.GetTranslation() + vr_menu_distance * dir;
	m_fixedHudTransform.SetTranslation(pos);
}

void VRManager::SetHudAttachedToHead()
{
	m_fixedPositionInitialized = false;
	m_hud.headLocked = true;
	m_hud.pose = xrmath::Identity();
	m_hud.pose.position.z = -vr_hud_distance;
	m_hud.width = vr_hud_width;
	m_hud.opaque = false;
}

void VRManager::SetHudInFrontOfPlayer()
{
	if (!m_fixedPositionInitialized)
	{
		RecalibrateView();
		m_fixedPositionInitialized = true;
	}

	m_hud.headLocked = false;
	m_hud.pose = FarCryToXrPose(m_fixedHudTransform);
	m_hud.width = vr_menu_width;
	m_hud.opaque = false;
	if (gVRRenderer->ShouldRenderStereo())
		m_stereoVisible = true;   // the 3D cinema quads take the same place
}

void VRManager::SetHudAsBinoculars()
{
	m_fixedPositionInitialized = false;
	bool leftHanded = m_pGame->g_LeftHanded->GetIVal() == 1;
	Matrix34 transform = m_input.GetControllerTransform(leftHanded ? 1 : 0);
	transform = transform * Matrix34::CreateTranslationMat(Vec3((leftHanded ? 1 : -1) * vr_binocular_size / 2, 0, vr_binocular_size / 2));
	m_hud.headLocked = false;
	m_hud.pose = FarCryToXrPose(transform);
	m_hud.width = vr_binocular_size;
	m_hud.opaque = true;
}

void VRManager::SetHudAsWeaponZoom()
{
	m_fixedPositionInitialized = false;
	Matrix34 transform = m_input.GetControllerTransform(m_pGame->g_LeftHanded->GetIVal() == 1 ? 1 : 0);
	Matrix34 rawHmdTransform = XrPoseToFarCry(m_headPoseXr);
	Vec3 headPos = rawHmdTransform.GetTranslation() - Vec3(0, 0, vr_scope_size / 2);
	Vec3 fwd = transform.GetTranslation() - headPos;
	Vec3 up(0, 0, 1);
	Vec3 left = -fwd.Cross(up).GetNormalized();
	up = left.Cross(-fwd).GetNormalized();
	transform.SetMatFromVectors(left, -fwd, up, transform.GetTranslation());
	Ang3 angles = ToAnglesDeg(transform);
	angles.y = 0;
	transform.SetRotationXYZ(Deg2Rad(angles), transform.GetTranslation());
	transform = transform * Matrix34::CreateTranslationMat(Vec3(0, 0, vr_scope_size / 2));
	m_hud.headLocked = false;
	m_hud.pose = FarCryToXrPose(transform);
	m_hud.width = vr_scope_size;
	m_hud.opaque = true;
}

void VRManager::ReleaseDeviceResources()
{
	m_d3d->hudTexture.Reset();
	m_d3d->eyeTextures[0].Reset();
	m_d3d->eyeTextures[1].Reset();
	m_d3d->stereoTexture.Reset();
}

void VRManager::InitDevice(IDirect3DDevice9Ex* device)
{
	ReleaseDeviceResources();

	CryLogAlways("Acquiring device...");
	m_d3d->device = device;
}

bool VRManager::QueryVulkanDevice(IDirect3DDevice9Ex* device, VROpenXR::VulkanDevice& vulkanDevice)
{
	vulkanDevice = VROpenXR::VulkanDevice();
	if (!device || !dxvk::Load())
		return false;

	// dxvk only hands its Vulkan objects out alongside a texture's image, so ask with a throwaway texture
	ComPtr<IDirect3DTexture9> probe;
	HRESULT hr = device->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, probe.GetAddressOf(), nullptr);
	if (FAILED(hr) || !probe)
	{
		CryLogAlways("ERROR: creating the probe texture failed: 0x%08x", hr);
		return false;
	}

	DxvkVulkanTextureData data = {};
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	hr = dxvk::FillVulkanTextureInfo(device, probe.Get(), data, layout);
	if (hr != S_OK)
	{
		CryLogAlways("ERROR: dxvk did not hand out its Vulkan device: 0x%08x", hr);
		return false;
	}

	vulkanDevice.instance = data.instance;
	vulkanDevice.physicalDevice = data.physicalDevice;
	vulkanDevice.device = data.device;
	vulkanDevice.queue = data.queue;
	vulkanDevice.queueFamilyIndex = data.queueFamilyIndex;
	vulkanDevice.queueIndex = 0;   // dxvk's graphics queue is the first one it creates in its family
	return vulkanDevice.instance != VK_NULL_HANDLE && vulkanDevice.device != VK_NULL_HANDLE && vulkanDevice.queue != VK_NULL_HANDLE;
}

bool VRManager::CreateRenderTarget(int width, int height, const char* name, IDirect3DTexture9** ppTexture)
{
	*ppTexture = nullptr;
	if (!m_d3d->device)
		return false;

	// the game's back buffer is X8R8G8B8, so A8R8G8B8 (Vulkan B8G8R8A8) is the natural choice; with a runtime that
	// only offers RGBA swapchains render into A8B8G8R8 (R8G8B8A8) instead, StretchRect does the swizzle for free
	// and the frame can still be copied over 1:1
	const VkFormat swapchainFormat = m_xr.GetSwapchainFormat();
	const bool rgba = swapchainFormat == VK_FORMAT_R8G8B8A8_UNORM || swapchainFormat == VK_FORMAT_R8G8B8A8_SRGB;
	const D3DFORMAT format = rgba ? D3DFMT_A8B8G8R8 : D3DFMT_A8R8G8B8;

	CryLogAlways("Creating %s texture: %i x %i (%s)", name, width, height, rgba ? "A8B8G8R8" : "A8R8G8B8");
	HRESULT hr = m_d3d->device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, ppTexture, nullptr);
	if (FAILED(hr) || !*ppTexture)
	{
		CryLogAlways("ERROR: creating %s texture failed: 0x%08x", name, hr);
		*ppTexture = nullptr;
		return false;
	}
	return true;
}

bool VRManager::PrepareTextureForSubmission(IDirect3DTexture9* texture, SubmissionImage& image)
{
	image = SubmissionImage();
	if (!texture || !m_d3d->device)
		return false;

	DxvkVulkanTextureData data = {};
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	HRESULT hr = dxvk::FillVulkanTextureInfo(m_d3d->device.Get(), texture, data, layout);
	if (hr != S_OK || data.image == 0)
	{
		CryLogAlways("Fetching vulkan image info failed: 0x%08x", hr);
		return false;
	}

	image.source.image = (VkImage)data.image;
	image.source.format = (VkFormat)data.format;
	image.source.width = data.width;
	image.source.height = data.height;
	image.originalLayout = layout;
	image.texture = texture;

	// into the copy source layout; recorded into dxvk's command stream behind the game's draws into the texture
	if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
		dxvk::TransitionImageLayout(m_d3d->device.Get(), texture, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	return true;
}

void VRManager::PostSubmissionTransitionTexture(SubmissionImage& image)
{
	if (!image.texture || !m_d3d->device)
		return;
	if (image.originalLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
		dxvk::TransitionImageLayout(m_d3d->device.Get(), image.texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.originalLayout);
	image.texture = nullptr;
}

void VRManager::LockQueue(bool flushPending)
{
	dxvk::LockSubmissionQueue(m_d3d->device.Get(), flushPending);
}

void VRManager::UnlockQueue()
{
	dxvk::ReleaseSubmissionQueue(m_d3d->device.Get());
}

void VRManager::CreateEyeTexture(int eye)
{
	vector2di size = GetRenderSize();
	CreateRenderTarget(size.x, size.y, eye == 0 ? "left eye" : "right eye", m_d3d->eyeTextures[eye].ReleaseAndGetAddressOf());
}

void VRManager::CreateHUDTexture()
{
	vector2di size = GetRenderSize();
	CreateRenderTarget(size.x, size.y, "HUD", m_d3d->hudTexture.ReleaseAndGetAddressOf());
}

void VRManager::CreateStereoTexture()
{
	vector2di size = GetRenderSize();
	size.x *= 2;
	CreateRenderTarget(size.x, size.y, "stereo", m_d3d->stereoTexture.ReleaseAndGetAddressOf());
}

void VRManager::RegisterCVars()
{
	IConsole* console = m_pGame->GetSystem()->GetIConsole();
	console->Register("vr_yaw_deadzone_angle", &vr_yaw_deadzone_angle, 30, VF_DUMPTODISK, "Controls the deadzone angle in front of the player where weapon aim does not rotate the camera");
	console->Register("vr_enable_motion_controllers", &vr_enable_motion_controllers, 1, VF_DUMPTODISK, "Enable this to use VR motion controllers instead of keyboard+mouse");
	console->Register("vr_render_force_max_terrain_detail", &vr_render_force_max_terrain_detail, 1, VF_DUMPTODISK, "If enabled, will force terrain to render at max detail even in the distance");
	console->Register("vr_render_force_obj_draw_dist", &vr_render_force_obj_draw_dist, 0, VF_DUMPTODISK, "If enabled, will force objects and enemies to be drawn at much further distances (might result in rendering issues in some instances)");
	console->Register("vr_window_width", &vr_window_width, 1920, VF_DUMPTODISK, "Configures the Far Cry desktop window width");
	console->Register("vr_window_height", &vr_window_height, 1080, VF_DUMPTODISK, "Configures the Far Cry desktop window height");
	console->Register("vr_mirrored_eye", &vr_mirrored_eye, 1, VF_DUMPTODISK, "Which eye view is mirrored to the desktop window. 0 - left, 1 - right");
	console->Register("vr_melee_swing_threshold", &vr_melee_swing_threshold, 2.f, VF_CHEAT, "Configures speed threshold for physical swings to register as melee attacks");
	console->Register("vr_debug_draw_grip", &vr_debug_draw_grip, 0, 0, "If enabled, highlights the position of the current weapon's grip positions");
	console->Register("vr_debug_override_grip", &vr_debug_override_grip, 0, VF_CHEAT, "If enabled, overrides the weapon grip transform offsets");
	console->Register("vr_snap_turn_amount", &vr_snap_turn_amount, 0, VF_DUMPTODISK, "The amount of degrees to snap turn (set to 0 to disable snap turn)");
	console->Register("vr_smooth_turn_speed", &vr_smooth_turn_speed, 1.0f, VF_DUMPTODISK, "Determines speed of smooth turn.");
	console->Register("vr_button_long_press_time", &vr_button_long_press_time, 0.35f, VF_DUMPTODISK, "How long you need to hold a button down to register as a long press");
	console->Register("vr_haptics_effect_strength", &vr_haptics_effect_strength, 1.0f, VF_DUMPTODISK, "Modify the strength of controller haptic events. Set to 0 to disable haptics");
	console->Register("vr_weapon_pitch_offset", &vr_weapon_pitch_offset, 15.0f, VF_DUMPTODISK, "Modify the weapon grip vertical angle.");
	console->Register("vr_weapon_yaw_offset", &vr_weapon_yaw_offset, 0.0f, VF_DUMPTODISK, "Modify the weapon grip horizontal angle.");
	console->Register("vr_crosshair", &vr_crosshair, 1, VF_DUMPTODISK, "VR crosshair type. 0 - none, 1 - ball, 2 - laser");
	console->Register("vr_movement_dir", &vr_movement_dir, -1, VF_DUMPTODISK, "Movement direction reference: -1 = head, 0 = left hand, 1 = right hand");
	console->Register("vr_show_empty_hands", &vr_show_empty_hands, 1, VF_DUMPTODISK, "If enabled, draws empty player hands when appropriate");
	console->Register("vr_immersive_ladders", &vr_immersive_ladders, 1, VF_DUMPTODISK, "Climb ladders by grabbing with your hands");
	console->Register("vr_render_world_while_zoomed", &vr_render_world_while_zoomed, 1, VF_DUMPTODISK, "Keep rendering the world in VR while binoculars or weapon scopes are active - costs performance!");
	console->Register("vr_binocular_size", &vr_binocular_size, 0.4f, VF_DUMPTODISK, "Width of the binocular overlay (in meters)");
	console->Register("vr_scope_size", &vr_scope_size, 0.3f, VF_DUMPTODISK, "Width of the weapon scope overlay (in meters)");
	console->Register("vr_seated_mode", &vr_seated_mode, 0, VF_DUMPTODISK, "If enabled, will fix VR camera at player head height and disable physical crouching");
	console->Register("vr_cutscenes_cinema_mode", &vr_cutscenes_cinema_mode, 0, VF_DUMPTODISK, "Determines how cutscenes are played. 0 - full VR, 1 - 2D cinema, 2 - 3D cinema");
	console->Register("vr_vehicles_cinema_mode", &vr_vehicles_cinema_mode, 0, VF_DUMPTODISK, "Determines how vehicles are played. 0 - full VR, 1 - 2D cinema, 2 - 3D cinema");
	console->Register("vr_hud_distance", &vr_hud_distance, 2.5f, VF_DUMPTODISK, "Determines how far away from the player the ingame HUD is placed");
	console->Register("vr_hud_width", &vr_hud_width, 2, VF_DUMPTODISK, "Determines how large the ingame HUD is");
	console->Register("vr_menu_distance", &vr_menu_distance, 4, VF_DUMPTODISK, "Determines how far away from the player the menu and theater mode is placed");
	console->Register("vr_menu_width", &vr_menu_width, 4, VF_DUMPTODISK, "Determines how large the menu and theater mode is");
	console->Register("vr_skip_vehicle_transitions", &vr_skip_vehicle_transitions, 1, VF_DUMPTODISK, "If enabled, skip camera transitions when entering/exiting vehicles");
	console->Register("vr_decouple_vehicle_rotations", &vr_decouple_vehicle_rotations, 0, VF_DUMPTODISK, "If enabled, vehicle rotations do not automatically transfer to the player camera");
	console->Register("vr_vehicle_alt_controls", &vr_vehicle_alt_controls, 0, VF_DUMPTODISK, "Alternative vehicle controls: triggers accelerate and brake, A/X attack and leave, the main hand's thumbstick click toggles the lights");
	console->Register("vr_menu_pointer", &vr_menu_pointer, 1, VF_DUMPTODISK, "If enabled, shows the controller and its pointer ray in menus");
	vr_debug_override_rh_offset = console->CreateVariable("vr_debug_override_rh_offset", "0.0 -0.1 -0.018", VF_CHEAT);
	vr_debug_override_lh_offset = console->CreateVariable("vr_debug_override_lh_offset", "0.0 -0.1 -0.018", VF_CHEAT);
	vr_debug_override_rh_angles = console->CreateVariable("vr_debug_override_rh_angles", "0.0 0.0 0.0", VF_CHEAT);

	e_terrain_lod_ratio = console->GetCVar("e_terrain_lod_ratio");
	e_detail_texture_min_fov = console->GetCVar("e_detail_texture_min_fov");
	e_obj_view_dist_ratio = console->GetCVar("e_obj_view_dist_ratio");

	// disable motion blur, as it does not work properly in VR
	console->GetCVar("r_MotionBlur")->ForceSet("0");

	console->Update();
}

void VRManager::UpdateHmdTransform()
{
	if (!m_headPoseValid)
		return;

	Ang3 refAngles(0, 0, m_referenceYaw);
	Matrix33 refTransform;
	refTransform.SetRotationXYZ(refAngles);
	refTransform.Transpose();

	Matrix34 rawHmdTransform = XrPoseToFarCry(m_headPoseXr);
	rawHmdTransform.SetTranslation(rawHmdTransform.GetTranslation() - m_referencePosition);
	m_hmdTransform = refTransform * rawHmdTransform;
}
