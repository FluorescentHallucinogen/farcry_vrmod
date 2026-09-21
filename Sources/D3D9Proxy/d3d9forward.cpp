// Far Cry VR - d3d9.dll proxy: exports forwarded untouched to the system d3d9.dll
//
// Kept in a translation unit of its own that does not include d3d9.h: the stubs must carry the
// real export names so that d3d9proxy.def can list them plainly, and d3d9.h declares several of
// them (Direct3DCreate9Ex, D3DPERF_*) with signatures a naked jump stub cannot be declared with.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "d3d9forward.h"

namespace
{
	// Target for an export the system d3d9.dll does not have. FarCry.exe and CryRenderD3D9.dll
	// only ever call Direct3DCreate9, so this is never reached in practice.
	void __cdecl MissingExport()
	{
	}
}

// Each stub jumps straight to the system function, so the caller's arguments and calling
// convention pass through untouched whatever the real signature is. The stub's own signature
// is irrelevant; the linker resolves the undecorated names in d3d9proxy.def to these symbols.
#define FORWARD_EXPORT(name) \
	static FARPROC pfn_##name = nullptr; \
	extern "C" __declspec(naked) void name() { __asm { jmp dword ptr [pfn_##name] } }

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
	pfn_##name = GetProcAddress(systemD3D9, #name); \
	if (!pfn_##name) pfn_##name = (FARPROC)&MissingExport;

void ResolveForwardedExports(HMODULE systemD3D9)
{
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
}
