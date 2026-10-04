# dxvk patches

Changes to dxvk that the mod needs and that are not (yet) in the `farcry` branch of
[fholger/dxvkry](https://github.com/fholger/dxvkry), which `Sources/ThirdParty/dxvk` points at.

The CI pipeline applies them before building dxvk (`git apply ../dxvk-patches/*.patch` inside the
submodule). For a local build do the same once after checking the submodule out:

```
cd Sources\ThirdParty\dxvk
git apply ..\dxvk-patches\*.patch
dxvkry_setup.bat
cd build
msbuild dxvk.sln -m
meson install
```

Note that the prebuilt `Sources/ThirdParty/dxvk/bin/d3d9.dll` that comes with the submodule does
*not* contain these changes; `meson install` replaces it with the freshly built one.

## 0001-openxr-extension-provider-for-windows

dxvk only enables the Vulkan extensions it needs itself. A VR runtime needs a few more on the device
the game renders with (external memory / semaphore / fence sharing with the compositor), and dxvk
has "extension providers" that ask the VR runtime about them before the device is created. The
OpenVR provider does that through `openvr_api.dll` (that is how the SteamVR based versions of the
mod worked); the OpenXR provider only knew Wine's `wineopenxr.dll`.

This patch teaches the OpenXR provider Windows: it loads the `openxr_loader.dll` next to
`FarCry.exe`, creates a throwaway `XrInstance` with `XR_KHR_vulkan_enable`, asks the active runtime
for its Vulkan instance and device extensions, destroys the instance again (the mod creates its
own later), and drops extensions the loader or GPU does not offer. `DXVK_OPENXR=0` disables it.
Without this, `xrCreateSession` on dxvk's device fails unless SteamVR happens to be started through
a stray `openvr_api.dll` first.
