#pragma once

#include <windows.h>
#include <d3d9.h>

// Shared between the proxy's translation units; implemented in d3d9proxy.cpp.
void Log(const char* format, ...);
bool Verbose();
bool PatchVTable(void* object, unsigned index, void* detour, void** original);

// The device's own resource creation functions, as found in its vtable before it was hooked.
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateTexture)(IDirect3DDevice9Ex* self, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateVolumeTexture)(IDirect3DDevice9Ex* self, UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9** ppTexture, HANDLE* pSharedHandle);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateCubeTexture)(IDirect3DDevice9Ex* self, UINT edgeLength, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9** ppTexture, HANDLE* pSharedHandle);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateVertexBuffer)(IDirect3DDevice9Ex* self, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** ppBuffer, HANDLE* pSharedHandle);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateIndexBuffer)(IDirect3DDevice9Ex* self, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9** ppBuffer, HANDLE* pSharedHandle);
