#pragma once

#include "d3d9proxy.h"

// D3DPOOL_MANAGED emulation for the Direct3D 9Ex device, see d3d9managed.cpp.
namespace managed
{
	void Initialize();
	void SetEnabled(bool enabled);
	bool Enabled();

	// Each returns the object the game gets to keep, or a failure the caller may fall back from.
	// 'create' is the device's original creation function (the pool argument is chosen here).
	HRESULT CreateTexture(IDirect3DDevice9Ex* device, PFN_CreateTexture create, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, IDirect3DTexture9** ppTexture);
	HRESULT CreateCubeTexture(IDirect3DDevice9Ex* device, PFN_CreateCubeTexture create, UINT edgeLength, UINT levels, DWORD usage, D3DFORMAT format, IDirect3DCubeTexture9** ppTexture);
	HRESULT CreateVolumeTexture(IDirect3DDevice9Ex* device, PFN_CreateVolumeTexture create, UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format, IDirect3DVolumeTexture9** ppTexture);
	HRESULT CreateVertexBuffer(IDirect3DDevice9Ex* device, PFN_CreateVertexBuffer create, UINT length, DWORD usage, DWORD fvf, IDirect3DVertexBuffer9** ppBuffer);
	HRESULT CreateIndexBuffer(IDirect3DDevice9Ex* device, PFN_CreateIndexBuffer create, UINT length, DWORD usage, D3DFORMAT format, IDirect3DIndexBuffer9** ppBuffer);

	// The texture the GPU should sample when the game binds 'texture': the DEFAULT-pool twin of an
	// emulated managed texture (uploaded first if the game changed it), otherwise 'texture' itself.
	IDirect3DBaseTexture9* ResolveTexture(IDirect3DDevice9Ex* device, IDirect3DBaseTexture9* texture);

	// After a successful Reset: DEFAULT-pool contents are not guaranteed to survive it, re-upload.
	void OnReset(IDirect3DDevice9Ex* device);
}
