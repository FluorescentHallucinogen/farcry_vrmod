#include "StdAfx.h"
#include "DxvkInterop.h"

namespace
{
	// the exports are plain extern "C" functions of dxvkry's d3d9.dll (src/d3d9/d3d9_dxvkry.cpp, d3d9_device.cpp)
	typedef IDirect3DDevice9Ex* (__cdecl *PFN_dxvkGetCreatedDevice)();
	typedef void (__cdecl *PFN_dxvkLockSubmissionQueue)(IDirect3DDevice9Ex* device, bool flush);
	typedef void (__cdecl *PFN_dxvkReleaseSubmissionQueue)(IDirect3DDevice9Ex* device);
	typedef HRESULT (__cdecl *PFN_dxvkFillVulkanTextureInfo)(IDirect3DDevice9Ex* device, IDirect3DTexture9* texture, DxvkVulkanTextureData* data, VkImageLayout* layout);
	typedef void (__cdecl *PFN_dxvkTransitionImageLayout)(IDirect3DDevice9Ex* device, IDirect3DTexture9* texture, VkImageLayout from, VkImageLayout to);

	struct Exports
	{
		bool resolved = false;
		bool complete = false;
		PFN_dxvkGetCreatedDevice getCreatedDevice = nullptr;
		PFN_dxvkLockSubmissionQueue lockSubmissionQueue = nullptr;
		PFN_dxvkReleaseSubmissionQueue releaseSubmissionQueue = nullptr;
		PFN_dxvkFillVulkanTextureInfo fillVulkanTextureInfo = nullptr;
		PFN_dxvkTransitionImageLayout transitionImageLayout = nullptr;
	};
	Exports g_exports;
}

bool dxvk::Load()
{
	if (g_exports.resolved)
		return g_exports.complete;
	g_exports.resolved = true;

	// the game has long since loaded d3d9.dll (the one next to FarCry.exe, dxvk's when the mod is installed)
	HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
	if (!d3d9)
	{
		CryLogAlways("dxvk: d3d9.dll is not loaded");
		return false;
	}

	g_exports.getCreatedDevice = (PFN_dxvkGetCreatedDevice)GetProcAddress(d3d9, "dxvkGetCreatedDevice");
	g_exports.lockSubmissionQueue = (PFN_dxvkLockSubmissionQueue)GetProcAddress(d3d9, "dxvkLockSubmissionQueue");
	g_exports.releaseSubmissionQueue = (PFN_dxvkReleaseSubmissionQueue)GetProcAddress(d3d9, "dxvkReleaseSubmissionQueue");
	g_exports.fillVulkanTextureInfo = (PFN_dxvkFillVulkanTextureInfo)GetProcAddress(d3d9, "dxvkFillVulkanTextureInfo");
	g_exports.transitionImageLayout = (PFN_dxvkTransitionImageLayout)GetProcAddress(d3d9, "dxvkTransitionImageLayout");
	g_exports.complete = g_exports.getCreatedDevice && g_exports.lockSubmissionQueue && g_exports.releaseSubmissionQueue
		&& g_exports.fillVulkanTextureInfo && g_exports.transitionImageLayout;

	if (!g_exports.complete)
	{
		char path[MAX_PATH] = {};
		GetModuleFileNameA(d3d9, path, MAX_PATH);
		CryLogAlways("dxvk: %s is not the Far Cry VR build of dxvk (dxvkry), its exports are missing", path);
	}
	return g_exports.complete;
}

bool dxvk::IsLoaded()
{
	return g_exports.resolved && g_exports.complete;
}

IDirect3DDevice9Ex* dxvk::GetCreatedDevice()
{
	if (!Load())
		return nullptr;
	return g_exports.getCreatedDevice();
}

void dxvk::LockSubmissionQueue(IDirect3DDevice9Ex* device, bool flush)
{
	if (IsLoaded() && device)
		g_exports.lockSubmissionQueue(device, flush);
}

void dxvk::ReleaseSubmissionQueue(IDirect3DDevice9Ex* device)
{
	if (IsLoaded() && device)
		g_exports.releaseSubmissionQueue(device);
}

HRESULT dxvk::FillVulkanTextureInfo(IDirect3DDevice9Ex* device, IDirect3DTexture9* texture, DxvkVulkanTextureData& data, VkImageLayout& layout)
{
	if (!IsLoaded() || !device || !texture)
		return E_FAIL;
	return g_exports.fillVulkanTextureInfo(device, texture, &data, &layout);
}

void dxvk::TransitionImageLayout(IDirect3DDevice9Ex* device, IDirect3DTexture9* texture, VkImageLayout from, VkImageLayout to)
{
	if (IsLoaded() && device && texture)
		g_exports.transitionImageLayout(device, texture, from, to);
}
