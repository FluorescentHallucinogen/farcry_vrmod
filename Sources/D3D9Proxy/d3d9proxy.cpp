// Far Cry VR - d3d9.dll proxy
//
// Sits between the game (CryRenderD3D9.dll) and the system d3d9.dll. When active it turns the
// game's legacy Direct3D 9 device into a Direct3D 9Ex device, because only 9Ex render targets
// can be shared with the D3D11 device the VR mod uses to hand frames to SteamVR. The engine's
// renderer creates its device long before the mod's CryGame.dll is loaded, so this has to happen
// here, on the way into d3d9.dll.
//
// 9Ex has no D3DPOOL_MANAGED, so every managed resource the game creates is transparently moved
// to D3DPOOL_DEFAULT (plus D3DUSAGE_DYNAMIC for textures, so they stay lockable), and the swap
// chain is forced into windowed mode, since the VR mod runs the game at the headset's render
// resolution which no monitor can display.
//
// The proxy is active when the game was started by the FarCryVR launcher (FCVR_D3D9EX=1 in the
// environment) or with -MOD:CryVR on the command line. Otherwise every export is forwarded to
// the system d3d9.dll untouched and the flat game behaves exactly as before.
//
// Diagnostics go to FarCryVR_d3d9.log next to FarCry.exe (only when active).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace
{
	HMODULE g_systemD3D9 = nullptr;
	bool g_active = false;
	bool g_verbose = false;
	FILE* g_log = nullptr;
	IDirect3DDevice9Ex* g_device = nullptr;

	typedef IDirect3D9* (WINAPI* PFN_Direct3DCreate9)(UINT sdkVersion);
	typedef HRESULT (WINAPI* PFN_Direct3DCreate9Ex)(UINT sdkVersion, IDirect3D9Ex** ppD3D);
	PFN_Direct3DCreate9 g_systemCreate9 = nullptr;
	PFN_Direct3DCreate9Ex g_systemCreate9Ex = nullptr;

	void Log(const char* format, ...)
	{
		if (!g_active)
			return;
		if (!g_log)
		{
			g_log = fopen("FarCryVR_d3d9.log", "w");
			if (!g_log)
				return;
		}
		va_list args;
		va_start(args, format);
		vfprintf(g_log, format, args);
		va_end(args);
		fputc('\n', g_log);
		fflush(g_log);
	}

	bool PatchVTable(void* object, unsigned index, void* detour, void** original)
	{
		void** vtable = *reinterpret_cast<void***>(object);
		if (vtable[index] == detour)
			return true;

		DWORD oldProtect = 0;
		if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect))
		{
			Log("VirtualProtect on vtable slot %u failed: %lu", index, GetLastError());
			return false;
		}
		if (original)
			*original = vtable[index];
		vtable[index] = detour;
		VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
		return true;
	}

	// The VR render resolution is no display mode any monitor supports, and D3D9Ex windowed mode
	// never loses the device, so the engine is never allowed to go exclusive fullscreen.
	void ForceWindowed(D3DPRESENT_PARAMETERS& params, const char* where)
	{
		if (params.Windowed)
			return;

		Log("%s: forcing windowed mode (game asked for fullscreen %ux%u @ %u Hz)", where,
			params.BackBufferWidth, params.BackBufferHeight, params.FullScreen_RefreshRateInHz);
		params.Windowed = TRUE;
		params.FullScreen_RefreshRateInHz = 0;
		if (params.PresentationInterval != D3DPRESENT_INTERVAL_DEFAULT
			&& params.PresentationInterval != D3DPRESENT_INTERVAL_ONE
			&& params.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE)
		{
			// the multi-vblank intervals only exist in fullscreen mode
			params.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
		}
	}

	// ---------------------------------------------------------------------------------------------
	// IDirect3DDevice9 hooks (vtable slot numbers follow the declaration order in d3d9.h)
	// ---------------------------------------------------------------------------------------------

	enum DeviceSlot
	{
		DeviceSlot_Reset               = 16,
		DeviceSlot_CreateTexture       = 23,
		DeviceSlot_CreateVolumeTexture = 24,
		DeviceSlot_CreateCubeTexture   = 25,
		DeviceSlot_CreateVertexBuffer  = 26,
		DeviceSlot_CreateIndexBuffer   = 27,
	};

	typedef HRESULT (STDMETHODCALLTYPE* PFN_Reset)(IDirect3DDevice9Ex* self, D3DPRESENT_PARAMETERS* params);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateTexture)(IDirect3DDevice9Ex* self, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateVolumeTexture)(IDirect3DDevice9Ex* self, UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9** ppTexture, HANDLE* pSharedHandle);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateCubeTexture)(IDirect3DDevice9Ex* self, UINT edgeLength, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9** ppTexture, HANDLE* pSharedHandle);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateVertexBuffer)(IDirect3DDevice9Ex* self, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** ppBuffer, HANDLE* pSharedHandle);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateIndexBuffer)(IDirect3DDevice9Ex* self, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9** ppBuffer, HANDLE* pSharedHandle);

	PFN_Reset g_origReset = nullptr;
	PFN_CreateTexture g_origCreateTexture = nullptr;
	PFN_CreateVolumeTexture g_origCreateVolumeTexture = nullptr;
	PFN_CreateCubeTexture g_origCreateCubeTexture = nullptr;
	PFN_CreateVertexBuffer g_origCreateVertexBuffer = nullptr;
	PFN_CreateIndexBuffer g_origCreateIndexBuffer = nullptr;

	HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9Ex* self, D3DPRESENT_PARAMETERS* params)
	{
		if (params)
			ForceWindowed(*params, "Reset");
		HRESULT hr = g_origReset(self, params);
		if (FAILED(hr))
			Log("Reset failed: 0x%08lx", hr);
		else if (params)
			Log("Reset: %ux%u", params->BackBufferWidth, params->BackBufferHeight);
		return hr;
	}

	// D3D9Ex rejects D3DPOOL_MANAGED. Keep such textures in VRAM but lockable, so the engine can
	// still upload into them; drivers may refuse DYNAMIC for some formats, so retry without it.
	HRESULT STDMETHODCALLTYPE Hook_CreateTexture(IDirect3DDevice9Ex* self, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle)
	{
		if (pool != D3DPOOL_MANAGED)
			return g_origCreateTexture(self, width, height, levels, usage, format, pool, ppTexture, pSharedHandle);

		HRESULT hr = g_origCreateTexture(self, width, height, levels, usage | D3DUSAGE_DYNAMIC, format, D3DPOOL_DEFAULT, ppTexture, pSharedHandle);
		if (FAILED(hr))
			hr = g_origCreateTexture(self, width, height, levels, usage, format, D3DPOOL_DEFAULT, ppTexture, pSharedHandle);
		if (FAILED(hr))
			Log("CreateTexture %ux%u levels %u usage 0x%lx format %d: MANAGED -> DEFAULT failed: 0x%08lx", width, height, levels, usage, format, hr);
		else if (g_verbose)
			Log("CreateTexture %ux%u levels %u usage 0x%lx format %d: MANAGED -> DEFAULT", width, height, levels, usage, format);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE Hook_CreateVolumeTexture(IDirect3DDevice9Ex* self, UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9** ppTexture, HANDLE* pSharedHandle)
	{
		if (pool != D3DPOOL_MANAGED)
			return g_origCreateVolumeTexture(self, width, height, depth, levels, usage, format, pool, ppTexture, pSharedHandle);

		HRESULT hr = g_origCreateVolumeTexture(self, width, height, depth, levels, usage | D3DUSAGE_DYNAMIC, format, D3DPOOL_DEFAULT, ppTexture, pSharedHandle);
		if (FAILED(hr))
			hr = g_origCreateVolumeTexture(self, width, height, depth, levels, usage, format, D3DPOOL_DEFAULT, ppTexture, pSharedHandle);
		if (FAILED(hr))
			Log("CreateVolumeTexture %ux%ux%u levels %u usage 0x%lx format %d: MANAGED -> DEFAULT failed: 0x%08lx", width, height, depth, levels, usage, format, hr);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE Hook_CreateCubeTexture(IDirect3DDevice9Ex* self, UINT edgeLength, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9** ppTexture, HANDLE* pSharedHandle)
	{
		if (pool != D3DPOOL_MANAGED)
			return g_origCreateCubeTexture(self, edgeLength, levels, usage, format, pool, ppTexture, pSharedHandle);

		HRESULT hr = g_origCreateCubeTexture(self, edgeLength, levels, usage | D3DUSAGE_DYNAMIC, format, D3DPOOL_DEFAULT, ppTexture, pSharedHandle);
		if (FAILED(hr))
			hr = g_origCreateCubeTexture(self, edgeLength, levels, usage, format, D3DPOOL_DEFAULT, ppTexture, pSharedHandle);
		if (FAILED(hr))
			Log("CreateCubeTexture %u levels %u usage 0x%lx format %d: MANAGED -> DEFAULT failed: 0x%08lx", edgeLength, levels, usage, format, hr);
		return hr;
	}

	// Buffers in DEFAULT pool stay lockable without DYNAMIC (just slower to lock), and managed
	// buffers are never locked with DISCARD/NOOVERWRITE, so their usage flags can stay as they are.
	HRESULT STDMETHODCALLTYPE Hook_CreateVertexBuffer(IDirect3DDevice9Ex* self, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** ppBuffer, HANDLE* pSharedHandle)
	{
		if (pool == D3DPOOL_MANAGED)
			pool = D3DPOOL_DEFAULT;
		HRESULT hr = g_origCreateVertexBuffer(self, length, usage, fvf, pool, ppBuffer, pSharedHandle);
		if (FAILED(hr))
			Log("CreateVertexBuffer %u bytes usage 0x%lx pool %d failed: 0x%08lx", length, usage, pool, hr);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE Hook_CreateIndexBuffer(IDirect3DDevice9Ex* self, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9** ppBuffer, HANDLE* pSharedHandle)
	{
		if (pool == D3DPOOL_MANAGED)
			pool = D3DPOOL_DEFAULT;
		HRESULT hr = g_origCreateIndexBuffer(self, length, usage, format, pool, ppBuffer, pSharedHandle);
		if (FAILED(hr))
			Log("CreateIndexBuffer %u bytes usage 0x%lx pool %d failed: 0x%08lx", length, usage, pool, hr);
		return hr;
	}

	void HookDevice(IDirect3DDevice9Ex* device)
	{
		PatchVTable(device, DeviceSlot_Reset, (void*)&Hook_Reset, (void**)&g_origReset);
		PatchVTable(device, DeviceSlot_CreateTexture, (void*)&Hook_CreateTexture, (void**)&g_origCreateTexture);
		PatchVTable(device, DeviceSlot_CreateVolumeTexture, (void*)&Hook_CreateVolumeTexture, (void**)&g_origCreateVolumeTexture);
		PatchVTable(device, DeviceSlot_CreateCubeTexture, (void*)&Hook_CreateCubeTexture, (void**)&g_origCreateCubeTexture);
		PatchVTable(device, DeviceSlot_CreateVertexBuffer, (void*)&Hook_CreateVertexBuffer, (void**)&g_origCreateVertexBuffer);
		PatchVTable(device, DeviceSlot_CreateIndexBuffer, (void*)&Hook_CreateIndexBuffer, (void**)&g_origCreateIndexBuffer);
	}

	// ---------------------------------------------------------------------------------------------
	// IDirect3D9 hooks
	// ---------------------------------------------------------------------------------------------

	enum InterfaceSlot
	{
		InterfaceSlot_CreateDevice = 16,
	};

	typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateDevice)(IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow, DWORD behaviorFlags, D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** ppDevice);
	PFN_CreateDevice g_origCreateDevice = nullptr;

	HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow, DWORD behaviorFlags, D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** ppDevice)
	{
		if (!params || !ppDevice)
			return g_origCreateDevice(self, adapter, deviceType, focusWindow, behaviorFlags, params, ppDevice);

		D3DPRESENT_PARAMETERS exParams = *params;
		ForceWindowed(exParams, "CreateDevice");

		IDirect3DDevice9Ex* device = nullptr;
		HRESULT hr = self->CreateDeviceEx(adapter, deviceType, focusWindow, behaviorFlags, &exParams, nullptr, &device);
		if (FAILED(hr) || !device)
		{
			Log("CreateDeviceEx failed: 0x%08lx, falling back to a legacy D3D9 device (VR will not work)", hr);
			return g_origCreateDevice(self, adapter, deviceType, focusWindow, behaviorFlags, params, ppDevice);
		}

		// D3D9 hands adjusted presentation parameters back to the caller
		*params = exParams;

		HookDevice(device);
		g_device = device;
		*ppDevice = device;
		Log("Created IDirect3DDevice9Ex on adapter %u: %ux%u, back buffer format %d, behavior flags 0x%lx",
			adapter, exParams.BackBufferWidth, exParams.BackBufferHeight, exParams.BackBufferFormat, behaviorFlags);
		return hr;
	}

	// ---------------------------------------------------------------------------------------------
	// Forwarders for the d3d9.dll exports we do not care about
	// ---------------------------------------------------------------------------------------------

	FARPROC g_missingExport = nullptr;

	void __cdecl MissingExport()
	{
		// never reached unless the game calls an export the system d3d9.dll lacks
	}
}

// d3d9.h declares most of these with their real signatures, so the forwarders get an internal
// Proxy_ name and are mapped to the exported name in d3d9proxy.def.
#define FORWARD_EXPORT(name) \
	static FARPROC pfn_##name = nullptr; \
	extern "C" __declspec(naked) void Proxy_##name() { __asm { jmp dword ptr [pfn_##name] } }

FORWARD_EXPORT(Direct3DCreate9Ex)
FORWARD_EXPORT(Direct3DCreate9On12)
FORWARD_EXPORT(Direct3DCreate9On12Ex)
FORWARD_EXPORT(Direct3DShaderValidatorCreate9)
FORWARD_EXPORT(Direct3D9EnableMaximizedWindowedModeShim)
FORWARD_EXPORT(D3DPERF_BeginEvent)
FORWARD_EXPORT(D3DPERF_EndEvent)
FORWARD_EXPORT(D3DPERF_GetStatus)
FORWARD_EXPORT(D3DPERF_QueryRepeatFrame)
FORWARD_EXPORT(D3DPERF_SetMarker)
FORWARD_EXPORT(D3DPERF_SetOptions)
FORWARD_EXPORT(D3DPERF_SetRegion)
FORWARD_EXPORT(DebugSetLevel)
FORWARD_EXPORT(DebugSetMute)
FORWARD_EXPORT(PSGPError)
FORWARD_EXPORT(PSGPSampleTexture)

#define RESOLVE_EXPORT(name) \
	pfn_##name = GetProcAddress(g_systemD3D9, #name); \
	if (!pfn_##name) pfn_##name = g_missingExport;

// ---------------------------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------------------------

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
	if (!g_systemCreate9)
		return nullptr;
	if (!g_active)
		return g_systemCreate9(sdkVersion);

	IDirect3D9Ex* d3d = nullptr;
	HRESULT hr = g_systemCreate9Ex ? g_systemCreate9Ex(sdkVersion, &d3d) : E_NOTIMPL;
	if (FAILED(hr) || !d3d)
	{
		Log("Direct3DCreate9Ex failed: 0x%08lx, falling back to legacy Direct3D 9 (VR will not work)", hr);
		return g_systemCreate9(sdkVersion);
	}

	PatchVTable(d3d, InterfaceSlot_CreateDevice, (void*)&Hook_CreateDevice, (void**)&g_origCreateDevice);
	Log("Created IDirect3D9Ex (SDK version %u)", sdkVersion);
	return d3d;
}

// The device the game's renderer created, once it exists. Used by the VR mod's CryGame.dll, which
// is loaded after the renderer initialized and therefore cannot hook device creation itself.
extern "C" IDirect3DDevice9Ex* FarCryVR_GetDevice()
{
	return g_device;
}

extern "C" BOOL FarCryVR_IsActive()
{
	return g_active ? TRUE : FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(instance);

		char path[MAX_PATH];
		UINT length = GetSystemDirectoryA(path, MAX_PATH);
		if (length == 0 || length >= MAX_PATH - 10)
			return FALSE;
		strcat(path, "\\d3d9.dll");
		g_systemD3D9 = LoadLibraryA(path);
		if (!g_systemD3D9)
			return FALSE;

		g_systemCreate9 = (PFN_Direct3DCreate9)GetProcAddress(g_systemD3D9, "Direct3DCreate9");
		g_systemCreate9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(g_systemD3D9, "Direct3DCreate9Ex");
		g_missingExport = (FARPROC)&MissingExport;

		RESOLVE_EXPORT(Direct3DCreate9Ex)
		RESOLVE_EXPORT(Direct3DCreate9On12)
		RESOLVE_EXPORT(Direct3DCreate9On12Ex)
		RESOLVE_EXPORT(Direct3DShaderValidatorCreate9)
		RESOLVE_EXPORT(Direct3D9EnableMaximizedWindowedModeShim)
		RESOLVE_EXPORT(D3DPERF_BeginEvent)
		RESOLVE_EXPORT(D3DPERF_EndEvent)
		RESOLVE_EXPORT(D3DPERF_GetStatus)
		RESOLVE_EXPORT(D3DPERF_QueryRepeatFrame)
		RESOLVE_EXPORT(D3DPERF_SetMarker)
		RESOLVE_EXPORT(D3DPERF_SetOptions)
		RESOLVE_EXPORT(D3DPERF_SetRegion)
		RESOLVE_EXPORT(DebugSetLevel)
		RESOLVE_EXPORT(DebugSetMute)
		RESOLVE_EXPORT(PSGPError)
		RESOLVE_EXPORT(PSGPSampleTexture)

		char value[8] = {};
		if (GetEnvironmentVariableA("FCVR_D3D9EX", value, sizeof(value)) && value[0] == '1')
			g_active = true;
		if (GetEnvironmentVariableA("FCVR_D3D9EX_LOG", value, sizeof(value)) && value[0] == '1')
			g_verbose = true;

		if (!g_active)
		{
			// also cover launching FarCry.exe -MOD:CryVR by hand
			const char* commandLine = GetCommandLineA();
			for (const char* p = commandLine; *p; ++p)
			{
				if (_strnicmp(p, "-MOD:CryVR", 10) == 0)
				{
					g_active = true;
					break;
				}
			}
		}

		Log("Far Cry VR d3d9.dll proxy active, system d3d9.dll loaded from %s", path);
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		if (g_log)
		{
			fclose(g_log);
			g_log = nullptr;
		}
	}
	return TRUE;
}
