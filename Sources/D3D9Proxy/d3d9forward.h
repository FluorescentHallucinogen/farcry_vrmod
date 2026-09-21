#pragma once

#include <windows.h>

// Resolves every forwarded d3d9.dll export against the system d3d9.dll (see d3d9forward.cpp).
// Called from DllMain, before the game can reach any of them.
void ResolveForwardedExports(HMODULE systemD3D9);
