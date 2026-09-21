#include "StdAfx.h"
#include "VRManager.h"
#include "Cry_Camera.h"
#include "xplayer.h"
#include "ComPtr.h"

#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <openvr.h>

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

// OpenVR: x = right, y = up, -z = forward
// FarCry: x = left, -y = forward, z = up
Matrix34 OpenVRToFarCry(const vr::HmdMatrix34_t &mat)
{
	Matrix34 m;
	m.m00 = mat.m[0][0];
	m.m01 = -mat.m[0][2];
	m.m02 = -mat.m[0][1];
	m.m03 = -mat.m[0][3];
	m.m10 = -mat.m[2][0];
	m.m11 = mat.m[2][2];
	m.m12 = mat.m[2][1];
	m.m13 = mat.m[2][3];
	m.m20 = -mat.m[1][0];
	m.m21 = mat.m[1][2];
	m.m22 = mat.m[1][1];
	m.m23 = mat.m[1][3];
	return m;
}

vr::HmdMatrix34_t FarCryToOpenVR(const Matrix34& mat)
{
	vr::HmdMatrix34_t res;
	res.m[0][0] = mat.m00;
	res.m[0][1] = -mat.m02;
	res.m[0][2] = -mat.m01;
	res.m[0][3] = -mat.m03;
	res.m[1][0] = -mat.m20;
	res.m[1][1] = mat.m22;
	res.m[1][2] = mat.m21;
	res.m[1][3] = mat.m23;
	res.m[2][0] = -mat.m10;
	res.m[2][1] = mat.m12;
	res.m[2][2] = mat.m11;
	res.m[2][3] = mat.m13;
	return res;
}

struct VRManager::D3DResources
{
	ComPtr<IDirect3DDevice9Ex> device;
	ComPtr<IDirect3DQuery9> flushQuery;
	ComPtr<IDirect3DTexture9> hudTexture;
	ComPtr<IDirect3DTexture9> stereoTexture;
	ComPtr<IDirect3DTexture9> eyeTextures[2];

	// SteamVR only accepts D3D11 (or Vulkan/OpenGL) textures, so every render target above is created as a
	// shared D3D9Ex surface and opened on this D3D11 device, which lives on the adapter SteamVR asked for.
	ComPtr<ID3D11Device> device11;
	ComPtr<ID3D11DeviceContext> context11;
	ComPtr<ID3D11Texture2D> hudTexture11;
	ComPtr<ID3D11Texture2D> stereoTexture11;
	ComPtr<ID3D11Texture2D> eyeTextures11[2];
};

VRManager::VRManager()
{
	m_d3d = new D3DResources;
	m_hmdTransform = Matrix34::CreateIdentity();
}


VRManager::~VRManager()
{
	// if Shutdown isn't properly called, we will get an infinite hang when trying to dispose of our D3D resources after
	// the game already shut down. So just let go here to avoid that
	m_d3d->device.Detach();
	m_d3d->context11.Detach();
	m_d3d->device11.Detach();
	delete m_d3d;
}

bool VRManager::Init(CXGame *game)
{
	if (m_initialized)
		return true;

	HMODULE module = GetCurrentModule();
	CryLogAlways("Initializing CryVR, base module address: 0x%x", module);

	m_pGame = game;

	vr::EVRInitError error;
	vr::VR_Init(&error, vr::VRApplication_Scene);
	if (error != vr::VRInitError_None)
	{
		CryError("Failed to initialize OpenVR: %s", vr::VR_GetVRInitErrorAsEnglishDescription(error));
		return false;
	}

	vr::VRCompositor()->SetTrackingSpace(vr::TrackingUniverseStanding);

	vr::VROverlay()->CreateOverlay("FarCryHud", "FarCry HUD", &m_hudOverlay);
	vr::VROverlay()->SetOverlaySortOrder(m_hudOverlay, 1);
	vr::VROverlay()->CreateOverlay("FarCry3D", "FarCry 3D", &m_3DOverlay);
	vr::VROverlay()->SetOverlayWidthInMeters(m_hudOverlay, 2.f);
	vr::VROverlay()->ShowOverlay(m_hudOverlay);

	vr::VROverlay()->CreateOverlay("FarCry3D", "FarCry 3D", &m_3DOverlay);
	vr::VROverlay()->SetOverlayFlag(m_3DOverlay, vr::VROverlayFlags_SideBySide_Parallel, true);
	vr::VROverlay()->HideOverlay(m_3DOverlay);

	float ll, lr, lt, lb, rl, rr, rt, rb;
	vr::VRSystem()->GetProjectionRaw(vr::Eye_Left, &ll, &lr, &lt, &lb);
	vr::VRSystem()->GetProjectionRaw(vr::Eye_Right, &rl, &rr, &rt, &rb);
	CryLogAlways(" Left eye - l: %.2f  r: %.2f  t: %.2f  b: %.2f", ll, lr, lt, lb);
	CryLogAlways("Right eye - l: %.2f  r: %.2f  t: %.2f  b: %.2f", rl, rr, rt, rb);
	m_verticalFov = max(max(fabsf(lt), fabsf(lb)), max(fabsf(rt), fabsf(rb)));
	m_horizontalFov = max(max(fabsf(ll), fabsf(lr)), max(fabsf(rl), fabsf(rr)));
	m_vertRenderScale = 2.f * m_verticalFov / min(fabsf(lt) + fabsf(lb), fabsf(rt) + fabsf(rb));
	CryLogAlways("VR vert fov: %.2f  horz fov: %.2f  vert scale: %.2f", m_verticalFov, m_horizontalFov, m_vertRenderScale);

	RegisterCVars();

	m_inputReady = m_input.Init(game);
	m_vrHaptics.Init(game, &m_input);

	m_hmdTransform = Matrix34::CreateIdentity();
	m_referencePosition = Vec3(0, 0, 0);
	m_referenceYaw = 0;
	m_uncommittedReferenceYaw = 0;
	m_uncommittedReferencePosition = Vec3(0, 0, 0);

	m_initialized = true;
	return true;
}

void VRManager::Shutdown()
{
	ReleaseDeviceResources();
	m_d3d->context11.Reset();
	m_d3d->device11.Reset();
	m_d3d->device.Reset();

	if (!m_initialized)
		return;

	vr::VROverlay()->DestroyOverlay(m_hudOverlay);
	vr::VR_Shutdown();
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

	vr::VRCompositor()->WaitGetPoses(&m_headPose, 1, nullptr, 0);

	UpdateHmdTransform();
}

void VRManager::HandleEvents()
{
	vr::VREvent_t event;
	while (vr::VRSystem()->PollNextEvent(&event, sizeof(vr::VREvent_t)))
	{
		if (event.eventType == vr::VREvent_SeatedZeroPoseReset)
		{
			vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &m_headPose, 1);
			RecalibrateView();
		}
		if (event.eventType == vr::VREvent_Quit)
		{
			vr::VRSystem()->AcknowledgeQuit_Exiting();
			m_pGame->GetSystem()->Quit();
		}
		if (event.eventType == vr::VREvent_DashboardActivated)
		{
			m_pGame->GotoMenu(false);
		}
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
	if (!m_initialized || !m_d3d->device || !m_d3d->eyeTextures11[0] || !m_d3d->eyeTextures11[1])
		return;

	// the eye textures were filled on the D3D9 device; make sure the GPU is done with them before SteamVR
	// reads them through the D3D11 aliases (D3D9Ex shared surfaces carry no synchronization of their own)
	FlushRendering();

	for (int eye = 0; eye < 2; ++eye) 
	{
		// game is currently using symmetric projection, we need to cut off the texture accordingly
		vr::VRTextureBounds_t bounds;
		GetEffectiveRenderLimits(eye, &bounds.uMin, &bounds.uMax, &bounds.vMin, &bounds.vMax);

		vr::Texture_t vrTexData;
		vrTexData.eColorSpace = vr::ColorSpace_Gamma;
		vrTexData.eType = vr::TextureType_DirectX;
		vrTexData.handle = m_d3d->eyeTextures11[eye].Get();

		auto error = vr::VRCompositor()->Submit(eye == 0 ? vr::Eye_Left : vr::Eye_Right, &vrTexData, &bounds);
		if (error != vr::VRCompositorError_None && error != vr::VRCompositorError_AlreadySubmitted)
		{
			CryLogAlways("Submitting eye texture failed: %i", error);
		}
	}

	if (m_d3d->hudTexture11.Get())
	{
		vr::Texture_t texInfo;
		texInfo.eColorSpace = vr::ColorSpace_Gamma;
		texInfo.eType = vr::TextureType_DirectX;
		texInfo.handle = m_d3d->hudTexture11.Get();
		vr::VROverlay()->SetOverlayTexture(m_hudOverlay, &texInfo);
	}

	if (m_d3d->stereoTexture11.Get())
	{
		vr::Texture_t texInfo;
		texInfo.eColorSpace = vr::ColorSpace_Gamma;
		texInfo.eType = vr::TextureType_DirectX;
		texInfo.handle = m_d3d->stereoTexture11.Get();
		vr::VROverlay()->SetOverlayTexture(m_3DOverlay, &texInfo);
	}

	// apparently we need to set the overlay mouse scale to some values with the proper aspect ratio, otherwise it just won't work
	vr::HmdVector2_t mouseScale;
	mouseScale.v[0] = m_pGame->m_pRenderer->GetWidth();
	mouseScale.v[1] = m_pGame->m_pRenderer->GetHeight();
	vr::VROverlay()->SetOverlayMouseScale(m_hudOverlay, &mouseScale);

	vr::VRCompositor()->PostPresentHandoff();

	m_wasBinocular = m_pGame->AreBinocularsActive();
}

vector2di VRManager::GetRenderSize() const
{
	if (!m_initialized)
		return vector2di(1280, 800);

	uint32_t width, height;
	vr::VRSystem()->GetRecommendedRenderTargetSize(&width, &height);
	height *= m_vertRenderScale;
	width = height * m_horizontalFov / m_verticalFov;
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

	vr::HmdMatrix34_t eyeMatVR = vr::VRSystem()->GetEyeToHeadTransform(eye == 0 ? vr::Eye_Left : vr::Eye_Right);
	Matrix34 eyeMat = OpenVRToFarCry(eyeMatVR);
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

	// but we can set up frustum planes for our asymmetric projection, which should help culling accuracy.
	float tanl, tanr, tant, tanb;
	vr::VRSystem()->GetProjectionRaw(eye == 0 ? vr::Eye_Left : vr::Eye_Right, &tanl, &tanr, &tant, &tanb);
	//cam.UpdateFrustumFromVRRaw(tanl, tanr, -tanb, -tant);
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
	float l, r, t, b;
	vr::VRSystem()->GetProjectionRaw(eye == 0 ? vr::Eye_Left : vr::Eye_Right, &l, &r, &t, &b);
	*left = 0.5f + 0.5f * l / m_horizontalFov;
	*right = 0.5f + 0.5f * r / m_horizontalFov;
	*top = 0.5f - 0.5f * b / m_verticalFov;
	*bottom = 0.5f - 0.5f * t / m_verticalFov;
}

void VRManager::ProcessInput()
{
	bool firstValidPose = m_referenceHeight < 0 && m_headPose.bPoseIsValid;
	if (firstValidPose)
	{
		RecalibrateView();
	}

	if (!gVRRenderer->ShouldRenderStereo())
		vr::VROverlay()->HideOverlay(m_3DOverlay);

	if ((m_pGame->IsInMenu() || m_pGame->GetSystem()->GetIConsole()->IsOpened()) && UseMotionControllers())
	{
		if (!m_wasInMenu)
		{
			CryLogAlways("Entering menu...");
			m_wasInMenu = true;
			m_buttonPressed = false;
			vr::VROverlay()->SetOverlayInputMethod(m_hudOverlay, vr::VROverlayInputMethod_Mouse);
			vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
			vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_HideLaserIntersection, true);
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
		vr::VROverlay()->SetOverlayInputMethod(m_hudOverlay, vr::VROverlayInputMethod_None);
		vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
		RecalibrateView();
	}

	if (!UseMotionControllers())
		return;

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

	vr::VREvent_t event;
	while (vr::VROverlay()->PollNextOverlayEvent(m_hudOverlay, &event, sizeof(vr::VREvent_t)))
	{
		if (event.eventType == vr::VREvent_MouseMove)
		{
			IMouse* mouse = m_pGame->GetSystem()->GetIInput()->GetIMouse();
			mouse->SetVScreenX(800.f * event.data.mouse.x / m_pGame->m_pRenderer->GetWidth());
			mouse->SetVScreenY(600.f * (1.f - event.data.mouse.y / m_pGame->m_pRenderer->GetHeight()));
		}
		if (event.eventType == vr::VREvent_MouseButtonDown)
		{
			if (event.data.mouse.button == vr::VRMouseButton_Left)
				m_mousePressed = true;
			m_buttonPressed = true;
			m_lastTimeButtonPressed = m_pGame->GetSystem()->GetITimer()->GetAsyncCurTime();
		}
		if (event.eventType == vr::VREvent_MouseButtonUp)
		{
			if (event.data.mouse.button == vr::VRMouseButton_Left)
				m_mouseReleased = true;
			m_buttonPressed = false;
		}
		if (event.eventType == vr::VREvent_ButtonPress)
		{
			m_buttonPressed = true;
			m_lastTimeButtonPressed = m_pGame->GetSystem()->GetITimer()->GetAsyncCurTime();
		}
		if (event.eventType == vr::VREvent_ButtonUnpress)
		{
			m_buttonPressed = false;
		}
	}

	if (m_buttonPressed && m_pGame->GetSystem()->GetITimer()->GetAsyncCurTime() - m_lastTimeButtonPressed >= 0.5f)
	{
		m_pGame->RequestStopVideo(true);
		m_buttonPressed = false;
	}
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
	if (!m_headPose.bPoseIsValid)
		return;

	CryLogAlways("Recalibrating view");
	Matrix34 rawHmdTransform = OpenVRToFarCry(m_headPose.mDeviceToAbsoluteTracking);
	Ang3 rawAngles;
	rawAngles.SetAnglesXYZ((Matrix33)rawHmdTransform);
	m_referencePosition = rawHmdTransform.GetTranslation();
	m_referenceHeight = m_referencePosition.z;
	m_referencePosition.z = 0;
	m_referenceYaw = rawAngles.z;
	UpdateHmdTransform();

	// recalibrate menu HUD positioning
	m_fixedHudTransform = OpenVRToFarCry(m_headPose.mDeviceToAbsoluteTracking);
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
	vr::HmdMatrix34_t hudTransform;
	memset(&hudTransform, 0, sizeof(vr::HmdMatrix34_t));
	hudTransform.m[0][0] = hudTransform.m[1][1] = hudTransform.m[2][2] = 1;
	hudTransform.m[2][3] = -vr_hud_distance;
	vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_IgnoreTextureAlpha, false);
	vr::VROverlay()->SetOverlayWidthInMeters(m_hudOverlay, vr_hud_width);
	vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(m_hudOverlay, vr::k_unTrackedDeviceIndex_Hmd, &hudTransform);
}

void VRManager::SetHudInFrontOfPlayer()
{
	if (!m_fixedPositionInitialized)
	{
		RecalibrateView();
		m_fixedPositionInitialized = true;
	}

	vr::HmdMatrix34_t hudTransform = FarCryToOpenVR(m_fixedHudTransform);
	vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_IgnoreTextureAlpha, false);
	vr::VROverlay()->SetOverlayWidthInMeters(m_hudOverlay, vr_menu_width);
	vr::VROverlay()->SetOverlayTransformAbsolute(m_hudOverlay, vr::TrackingUniverseStanding, &hudTransform);
	if (gVRRenderer->ShouldRenderStereo())
	{
		vr::VROverlay()->ShowOverlay(m_3DOverlay);
		vr::VROverlay()->SetOverlayWidthInMeters(m_3DOverlay, vr_menu_width);
		vr::VROverlay()->SetOverlayTransformAbsolute(m_3DOverlay, vr::TrackingUniverseStanding, &hudTransform);
	}
}

void VRManager::SetHudAsBinoculars()
{
	m_fixedPositionInitialized = false;
	bool leftHanded = m_pGame->g_LeftHanded->GetIVal() == 1;
	Matrix34 transform = m_input.GetControllerTransform(leftHanded ? 1 : 0);
	transform = transform * Matrix34::CreateTranslationMat(Vec3((leftHanded ? 1 : -1) * vr_binocular_size / 2, 0, vr_binocular_size / 2));
	vr::HmdMatrix34_t hudTransform = FarCryToOpenVR(transform);
	vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_IgnoreTextureAlpha, true);
	vr::VROverlay()->SetOverlayWidthInMeters(m_hudOverlay, vr_binocular_size);
	vr::VROverlay()->SetOverlayTransformAbsolute(m_hudOverlay, vr::TrackingUniverseStanding, &hudTransform);
}

void VRManager::SetHudAsWeaponZoom()
{
	m_fixedPositionInitialized = false;
	Matrix34 transform = m_input.GetControllerTransform(m_pGame->g_LeftHanded->GetIVal() == 1 ? 1 : 0);
	Matrix34 rawHmdTransform = OpenVRToFarCry(m_headPose.mDeviceToAbsoluteTracking);
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
	vr::HmdMatrix34_t hudTransform = FarCryToOpenVR(transform);
	vr::VROverlay()->SetOverlayFlag(m_hudOverlay, vr::VROverlayFlags_IgnoreTextureAlpha, true);
	vr::VROverlay()->SetOverlayWidthInMeters(m_hudOverlay, vr_scope_size);
	vr::VROverlay()->SetOverlayTransformAbsolute(m_hudOverlay, vr::TrackingUniverseStanding, &hudTransform);
}

void VRManager::ReleaseDeviceResources()
{
	// the D3D11 aliases only borrow the D3D9Ex surfaces, drop them first
	m_d3d->hudTexture11.Reset();
	m_d3d->stereoTexture11.Reset();
	m_d3d->eyeTextures11[0].Reset();
	m_d3d->eyeTextures11[1].Reset();
	m_d3d->flushQuery.Reset();
	m_d3d->hudTexture.Reset();
	m_d3d->eyeTextures[0].Reset();
	m_d3d->eyeTextures[1].Reset();
	m_d3d->stereoTexture.Reset();
}

void VRManager::InitDevice(IDirect3DDevice9Ex* device)
{
	ReleaseDeviceResources();
	m_d3d->context11.Reset();
	m_d3d->device11.Reset();

	CryLogAlways("Acquiring device...");
	m_d3d->device = device;
	if (!device)
		return;

	// SteamVR tells us which adapter it renders on; the D3D11 device must live there, otherwise Submit rejects
	// the textures (and the D3D9Ex surfaces could not be opened across adapters anyway)
	int32_t adapterIndex = -1;
	if (vr::VRSystem())
		vr::VRSystem()->GetDXGIOutputInfo(&adapterIndex);

	ComPtr<IDXGIFactory1> factory;
	ComPtr<IDXGIAdapter1> adapter;
	if (adapterIndex >= 0 && SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf())))
	{
		if (FAILED(factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf())))
			adapter.Reset();
	}

	D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
	HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter.Get() ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, m_d3d->device11.GetAddressOf(), &level, m_d3d->context11.GetAddressOf());
	if (FAILED(hr))
	{
		CryLogAlways("ERROR: D3D11CreateDevice failed: 0x%08x - VR frames cannot be submitted", hr);
		m_d3d->context11.Reset();
		m_d3d->device11.Reset();
		return;
	}
	CryLogAlways("Created D3D11 device on adapter %i (feature level 0x%x)", adapterIndex, level);
}

bool VRManager::CreateSharedRenderTarget(int width, int height, const char* name, IDirect3DTexture9** ppTexture9, ID3D11Texture2D** ppTexture11)
{
	*ppTexture9 = nullptr;
	*ppTexture11 = nullptr;
	if (!m_d3d->device)
		return false;

	CryLogAlways("Creating %s texture: %i x %i", name, width, height);

	// a shared handle is only available on D3D9Ex devices, which the Far Cry VR d3d9.dll proxy provides
	HANDLE sharedHandle = nullptr;
	ComPtr<IDirect3DTexture9> texture9;
	HRESULT hr = m_d3d->device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, texture9.GetAddressOf(), &sharedHandle);
	if (FAILED(hr) || !texture9)
	{
		CryLogAlways("ERROR: creating %s texture failed: 0x%08x (is the game running on a D3D9Ex device?)", name, hr);
		return false;
	}
	if (!sharedHandle)
	{
		CryLogAlways("ERROR: %s texture was created without a shared handle, it cannot be handed to SteamVR", name);
		return false;
	}
	if (!m_d3d->device11)
	{
		CryLogAlways("ERROR: no D3D11 device, %s texture cannot be handed to SteamVR", name);
		return false;
	}

	ComPtr<ID3D11Texture2D> texture11;
	hr = m_d3d->device11->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D), (void**)texture11.GetAddressOf());
	if (FAILED(hr) || !texture11)
	{
		CryLogAlways("ERROR: opening %s texture on the D3D11 device failed: 0x%08x", name, hr);
		return false;
	}

	*ppTexture9 = texture9.Detach();
	*ppTexture11 = texture11.Detach();
	return true;
}

void VRManager::FlushRendering()
{
	if (!m_d3d->device)
		return;

	if (!m_d3d->flushQuery)
		m_d3d->device->CreateQuery(D3DQUERYTYPE_EVENT, m_d3d->flushQuery.GetAddressOf());
	if (!m_d3d->flushQuery)
		return;

	m_d3d->flushQuery->Issue(D3DISSUE_END);
	while (m_d3d->flushQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
		SwitchToThread();
}

void VRManager::CreateEyeTexture(int eye)
{
	vector2di size = GetRenderSize();
	CreateSharedRenderTarget(size.x, size.y, eye == 0 ? "left eye" : "right eye",
		m_d3d->eyeTextures[eye].ReleaseAndGetAddressOf(), m_d3d->eyeTextures11[eye].ReleaseAndGetAddressOf());
}

void VRManager::CreateHUDTexture()
{
	vector2di size = GetRenderSize();
	CreateSharedRenderTarget(size.x, size.y, "HUD",
		m_d3d->hudTexture.ReleaseAndGetAddressOf(), m_d3d->hudTexture11.ReleaseAndGetAddressOf());
}

void VRManager::CreateStereoTexture()
{
	vector2di size = GetRenderSize();
	size.x *= 2;
	CreateSharedRenderTarget(size.x, size.y, "stereo",
		m_d3d->stereoTexture.ReleaseAndGetAddressOf(), m_d3d->stereoTexture11.ReleaseAndGetAddressOf());
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
	if (!m_headPose.bPoseIsValid)
		return;

	Ang3 refAngles(0, 0, m_referenceYaw);
	Matrix33 refTransform;
	refTransform.SetRotationXYZ(refAngles);
	refTransform.Transpose();

	Matrix34 rawHmdTransform = OpenVRToFarCry(m_headPose.mDeviceToAbsoluteTracking);
	rawHmdTransform.SetTranslation(rawHmdTransform.GetTranslation() - m_referencePosition);
	m_hmdTransform = refTransform * rawHmdTransform;
}
