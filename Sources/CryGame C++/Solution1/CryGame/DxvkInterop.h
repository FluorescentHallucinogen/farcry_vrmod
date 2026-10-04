#pragma once

// Access to the few things the Far Cry VR build of dxvk (fholger/dxvkry, Sources/ThirdParty/dxvk) exports on top of
// stock dxvk: the D3D9 device the game created, and a thin wrapper around dxvk's ID3D9VkInteropDevice /
// ID3D9VkInteropTexture that lets us hand the game's render targets to OpenXR as Vulkan images and submit our own
// work to dxvk's graphics queue.
//
// The exports are looked up in the d3d9.dll the game loaded (which has to be dxvk's); nothing here links against
// it, so CryGame.dll still loads (and can say what is wrong) when another d3d9.dll is in place.

#include <windows.h>
#include <d3d9.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

// dxvkFillVulkanTextureInfo was written against OpenVR's vr::VRVulkanTextureData_t; this is that struct, laid out
// exactly the same (OpenVR packs it to 8), so the mod does not need the OpenVR headers any more
#pragma pack(push, 8)
struct DxvkVulkanTextureData
{
	uint64_t image;                    // VkImage
	VkDevice device;
	VkPhysicalDevice physicalDevice;
	VkInstance instance;
	VkQueue queue;
	uint32_t queueFamilyIndex;
	uint32_t width;
	uint32_t height;
	uint32_t format;                   // VkFormat
	uint32_t sampleCount;
};
#pragma pack(pop)

namespace dxvk
{
	// resolves the exports; false (with a log line) when the loaded d3d9.dll is not dxvkry
	bool Load();
	bool IsLoaded();

	// the D3D9 device the game created (dxvk's device object implements IDirect3DDevice9Ex)
	IDirect3DDevice9Ex* GetCreatedDevice();

	// Waits until dxvk's submission thread has handed everything it had to the GPU queue and keeps it from
	// submitting more until ReleaseSubmissionQueue. With flush, all D3D9 work recorded so far is queued up
	// first, so once this returns the queue holds every draw the game issued up to now.
	void LockSubmissionQueue(IDirect3DDevice9Ex* device, bool flush);
	void ReleaseSubmissionQueue(IDirect3DDevice9Ex* device);

	// the Vulkan image behind a D3D9 texture plus dxvk's instance/device/queue; layout is the layout dxvk keeps
	// the image in between its own commands
	HRESULT FillVulkanTextureInfo(IDirect3DDevice9Ex* device, IDirect3DTexture9* texture, DxvkVulkanTextureData& data, VkImageLayout& layout);

	// records a layout transition for the texture's image in dxvk's command stream (it reaches the GPU with the
	// next flush)
	void TransitionImageLayout(IDirect3DDevice9Ex* device, IDirect3DTexture9* texture, VkImageLayout from, VkImageLayout to);
}
