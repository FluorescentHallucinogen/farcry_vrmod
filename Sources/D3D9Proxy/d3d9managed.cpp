// Far Cry VR - d3d9.dll proxy: D3DPOOL_MANAGED emulation for the Direct3D 9Ex device
//
// 9Ex has no managed pool, and the engine (CryRenderD3D9.dll, a binary) creates nearly every
// texture and every static vertex/index buffer there. What D3D9 used to do for managed resources
// is done here instead:
//
//  - A managed texture is a SYSTEMMEM texture, which is what the game gets to keep (lockable,
//    readable, unaffected by Reset, D3DX-friendly), plus a DEFAULT-pool twin the GPU samples.
//    Whenever the game unlocked the system copy, the next SetTexture uploads it to the twin with
//    UpdateTexture, and SetTexture binds the twin instead of the system copy.
//  - A managed buffer is a DEFAULT-pool buffer whose Lock hands out a system-memory shadow; on
//    Unlock the locked range is copied into the real buffer.
//  - After a Reset every twin is uploaded again, because the contents of DEFAULT-pool resources
//    are not guaranteed to survive it (which is exactly what turned the game black after the
//    VR mod's resolution changes).
//
// The hooks live in the vtables of the runtime's texture, surface and buffer classes (one vtable
// per class, shared by all instances), so they see every object of that class and look up the
// ones registered here. Each emulated resource carries a small IUnknown in its private data that
// the runtime releases when the resource is destroyed; that is what frees the twin or the shadow.

#define WIN32_LEAN_AND_MEAN
#include "d3d9proxy.h"
#include "d3d9managed.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <unordered_map>
#include <vector>

namespace
{
	// {7F1D2C0B-5A44-4E7B-9D63-2B0C8E5F1A90}
	const GUID GUID_ManagedSentinel = { 0x7f1d2c0b, 0x5a44, 0x4e7b, { 0x9d, 0x63, 0x2b, 0x0c, 0x8e, 0x5f, 0x1a, 0x90 } };

	enum ResourceKind
	{
		Kind_Texture,
		Kind_Buffer,
	};

	struct TextureEntry
	{
		IDirect3DBaseTexture9* vram;   // DEFAULT-pool twin the GPU samples
		D3DRESOURCETYPE type;
		bool dirty;                    // system copy changed since the last upload
		bool uploadFailed;             // logged once
	};

	struct BufferEntry
	{
		void* shadow;                  // system-memory copy the game locks
		UINT length;
		UINT lockCount;                // D3D9 allows nested locks; the upload happens when the last one ends
		UINT dirtyBegin;               // union of the ranges locked for writing since the last upload
		UINT dirtyEnd;
		bool uploadAll;                // set by Reset while the buffer was locked
		bool uploadFailed;             // logged once
	};

	struct VTableOriginal
	{
		void** vtable;
		unsigned slot;
		void* original;
	};

	bool g_enabled = true;
	CRITICAL_SECTION g_lock;
	std::unordered_map<void*, TextureEntry> g_textures;   // key: the SYSTEMMEM texture the game holds
	std::unordered_map<void*, BufferEntry> g_buffers;     // key: the DEFAULT-pool buffer the game holds
	std::vector<IUnknown*> g_pendingRelease;              // twins whose owner died, released outside the runtime's destructor
	VTableOriginal g_originals[32];
	unsigned g_originalCount = 0;
	unsigned g_uploads = 0;

	struct Guard
	{
		Guard() { EnterCriticalSection(&g_lock); }
		~Guard() { LeaveCriticalSection(&g_lock); }
	};

	// vtable slots, following the declaration order in d3d9.h
	enum
	{
		TextureSlot_Unlock       = 20,   // IDirect3DTexture9::UnlockRect, IDirect3DCubeTexture9::UnlockRect, IDirect3DVolumeTexture9::UnlockBox
		TextureSlot_AddDirty     = 21,   // IDirect3DTexture9::AddDirtyRect, IDirect3DCubeTexture9::AddDirtyRect, IDirect3DVolumeTexture9::AddDirtyBox
		SurfaceSlot_UnlockRect   = 14,   // IDirect3DSurface9::UnlockRect
		VolumeSlot_UnlockBox     = 10,   // IDirect3DVolume9::UnlockBox
		BufferSlot_Lock          = 11,   // IDirect3DVertexBuffer9::Lock, IDirect3DIndexBuffer9::Lock
		BufferSlot_Unlock        = 12,   // IDirect3DVertexBuffer9::Unlock, IDirect3DIndexBuffer9::Unlock
	};

	typedef HRESULT (STDMETHODCALLTYPE* PFN_TextureUnlockRect)(IDirect3DTexture9* self, UINT level);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_TextureAddDirtyRect)(IDirect3DTexture9* self, const RECT* rect);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_CubeUnlockRect)(IDirect3DCubeTexture9* self, D3DCUBEMAP_FACES face, UINT level);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_CubeAddDirtyRect)(IDirect3DCubeTexture9* self, D3DCUBEMAP_FACES face, const RECT* rect);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_VolumeTextureUnlockBox)(IDirect3DVolumeTexture9* self, UINT level);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_VolumeTextureAddDirtyBox)(IDirect3DVolumeTexture9* self, const D3DBOX* box);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_SurfaceUnlockRect)(IDirect3DSurface9* self);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_VolumeUnlockBox)(IDirect3DVolume9* self);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_BufferLock)(IDirect3DResource9* self, UINT offset, UINT size, void** ppbData, DWORD flags);
	typedef HRESULT (STDMETHODCALLTYPE* PFN_BufferUnlock)(IDirect3DResource9* self);

	// ---------------------------------------------------------------------------------------------
	// vtable hooking with one original per (class vtable, slot)
	// ---------------------------------------------------------------------------------------------

	void* FindOriginal(void* object, unsigned slot)
	{
		void** vtable = *reinterpret_cast<void***>(object);
		for (unsigned i = 0; i < g_originalCount; ++i)
		{
			if (g_originals[i].vtable == vtable && g_originals[i].slot == slot)
				return g_originals[i].original;
		}
		return nullptr;
	}

	// call with the lock held
	void HookSlot(void* object, unsigned slot, void* detour)
	{
		void** vtable = *reinterpret_cast<void***>(object);
		if (vtable[slot] == detour)
			return;
		if (g_originalCount >= sizeof(g_originals) / sizeof(g_originals[0]))
		{
			Log("managed: out of vtable hook slots");
			return;
		}
		void* original = nullptr;
		if (!PatchVTable(object, slot, detour, &original))
			return;
		g_originals[g_originalCount].vtable = vtable;
		g_originals[g_originalCount].slot = slot;
		g_originals[g_originalCount].original = original;
		++g_originalCount;
	}

	// ---------------------------------------------------------------------------------------------
	// lifetime: the runtime releases this object when the resource carrying it is destroyed
	// ---------------------------------------------------------------------------------------------

	void OnResourceDestroyed(void* key, ResourceKind kind);

	class Sentinel : public IUnknown
	{
	public:
		Sentinel(void* key, ResourceKind kind) : m_refs(1), m_key(key), m_kind(kind) {}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
		{
			if (!ppv)
				return E_POINTER;
			if (riid == __uuidof(IUnknown))
			{
				*ppv = static_cast<IUnknown*>(this);
				AddRef();
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}

		ULONG STDMETHODCALLTYPE AddRef() override
		{
			return (ULONG)InterlockedIncrement(&m_refs);
		}

		ULONG STDMETHODCALLTYPE Release() override
		{
			LONG refs = InterlockedDecrement(&m_refs);
			if (refs == 0)
			{
				OnResourceDestroyed(m_key, m_kind);
				delete this;
			}
			return (ULONG)refs;
		}

	private:
		LONG m_refs;
		void* m_key;
		ResourceKind m_kind;
	};

	// Without the sentinel the entry could outlive its resource and be matched by an unrelated
	// object later allocated at the same address, so on failure the registration is undone.
	bool AttachSentinel(IDirect3DResource9* resource, ResourceKind kind)
	{
		Sentinel* sentinel = new (std::nothrow) Sentinel(resource, kind);
		if (!sentinel)
			return false;
		HRESULT hr = resource->SetPrivateData(GUID_ManagedSentinel, static_cast<IUnknown*>(sentinel), sizeof(IUnknown*), D3DSPD_IUNKNOWN);
		if (FAILED(hr))
		{
			Log("managed: SetPrivateData failed: 0x%08lx", hr);
			delete sentinel;
			return false;
		}
		sentinel->Release();   // the runtime holds the only reference now
		return true;
	}

	void OnResourceDestroyed(void* key, ResourceKind kind)
	{
		Guard guard;
		if (kind == Kind_Texture)
		{
			std::unordered_map<void*, TextureEntry>::iterator it = g_textures.find(key);
			if (it != g_textures.end())
			{
				if (it->second.vram)
					g_pendingRelease.push_back(it->second.vram);
				g_textures.erase(it);
			}
		}
		else
		{
			std::unordered_map<void*, BufferEntry>::iterator it = g_buffers.find(key);
			if (it != g_buffers.end())
			{
				free(it->second.shadow);
				g_buffers.erase(it);
			}
		}
	}

	// call with the lock held, never from inside the runtime's destructor of another resource
	void ReleasePending()
	{
		for (size_t i = 0; i < g_pendingRelease.size(); ++i)
			g_pendingRelease[i]->Release();
		g_pendingRelease.clear();
	}

	// ---------------------------------------------------------------------------------------------
	// textures
	// ---------------------------------------------------------------------------------------------

	void MarkDirty(void* key)
	{
		Guard guard;
		std::unordered_map<void*, TextureEntry>::iterator it = g_textures.find(key);
		if (it != g_textures.end())
			it->second.dirty = true;
	}

	// Tell the runtime the whole system copy is dirty, so UpdateTexture copies every level.
	void MarkWholeDirty(IDirect3DBaseTexture9* texture, D3DRESOURCETYPE type)
	{
		switch (type)
		{
		case D3DRTYPE_TEXTURE:
			static_cast<IDirect3DTexture9*>(texture)->AddDirtyRect(nullptr);
			break;
		case D3DRTYPE_CUBETEXTURE:
			for (int face = 0; face < 6; ++face)
				static_cast<IDirect3DCubeTexture9*>(texture)->AddDirtyRect((D3DCUBEMAP_FACES)face, nullptr);
			break;
		case D3DRTYPE_VOLUMETEXTURE:
			static_cast<IDirect3DVolumeTexture9*>(texture)->AddDirtyBox(nullptr);
			break;
		default:
			break;
		}
	}

	void MarkContainerDirty(IDirect3DSurface9* surface)
	{
		IDirect3DBaseTexture9* parent = nullptr;
		if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DBaseTexture9), (void**)&parent)) && parent)
		{
			MarkDirty(parent);
			parent->Release();
		}
	}

	void MarkContainerDirty(IDirect3DVolume9* volume)
	{
		IDirect3DBaseTexture9* parent = nullptr;
		if (SUCCEEDED(volume->GetContainer(__uuidof(IDirect3DBaseTexture9), (void**)&parent)) && parent)
		{
			MarkDirty(parent);
			parent->Release();
		}
	}

	HRESULT STDMETHODCALLTYPE Hook_Texture_UnlockRect(IDirect3DTexture9* self, UINT level)
	{
		PFN_TextureUnlockRect original = (PFN_TextureUnlockRect)FindOriginal(self, TextureSlot_Unlock);
		HRESULT hr = original ? original(self, level) : D3DERR_INVALIDCALL;
		if (SUCCEEDED(hr))
			MarkDirty(self);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE Hook_Texture_AddDirtyRect(IDirect3DTexture9* self, const RECT* rect)
	{
		PFN_TextureAddDirtyRect original = (PFN_TextureAddDirtyRect)FindOriginal(self, TextureSlot_AddDirty);
		HRESULT hr = original ? original(self, rect) : D3DERR_INVALIDCALL;
		if (SUCCEEDED(hr))
			MarkDirty(self);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE Hook_Cube_UnlockRect(IDirect3DCubeTexture9* self, D3DCUBEMAP_FACES face, UINT level)
	{
		PFN_CubeUnlockRect original = (PFN_CubeUnlockRect)FindOriginal(self, TextureSlot_Unlock);
		HRESULT hr = original ? original(self, face, level) : D3DERR_INVALIDCALL;
		if (SUCCEEDED(hr))
			MarkDirty(self);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE Hook_Cube_AddDirtyRect(IDirect3DCubeTexture9* self, D3DCUBEMAP_FACES face, const RECT* rect)
	{
		PFN_CubeAddDirtyRect original = (PFN_CubeAddDirtyRect)FindOriginal(self, TextureSlot_AddDirty);
		HRESULT hr = original ? original(self, face, rect) : D3DERR_INVALIDCALL;
		if (SUCCEEDED(hr))
			MarkDirty(self);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE Hook_VolumeTexture_UnlockBox(IDirect3DVolumeTexture9* self, UINT level)
	{
		PFN_VolumeTextureUnlockBox original = (PFN_VolumeTextureUnlockBox)FindOriginal(self, TextureSlot_Unlock);
		HRESULT hr = original ? original(self, level) : D3DERR_INVALIDCALL;
		if (SUCCEEDED(hr))
			MarkDirty(self);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE Hook_VolumeTexture_AddDirtyBox(IDirect3DVolumeTexture9* self, const D3DBOX* box)
	{
		PFN_VolumeTextureAddDirtyBox original = (PFN_VolumeTextureAddDirtyBox)FindOriginal(self, TextureSlot_AddDirty);
		HRESULT hr = original ? original(self, box) : D3DERR_INVALIDCALL;
		if (SUCCEEDED(hr))
			MarkDirty(self);
		return hr;
	}

	// the game (or D3DX) may lock a level through its surface instead of through the texture
	HRESULT STDMETHODCALLTYPE Hook_Surface_UnlockRect(IDirect3DSurface9* self)
	{
		PFN_SurfaceUnlockRect original = (PFN_SurfaceUnlockRect)FindOriginal(self, SurfaceSlot_UnlockRect);
		HRESULT hr = original ? original(self) : D3DERR_INVALIDCALL;
		if (SUCCEEDED(hr))
			MarkContainerDirty(self);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE Hook_Volume_UnlockBox(IDirect3DVolume9* self)
	{
		PFN_VolumeUnlockBox original = (PFN_VolumeUnlockBox)FindOriginal(self, VolumeSlot_UnlockBox);
		HRESULT hr = original ? original(self) : D3DERR_INVALIDCALL;
		if (SUCCEEDED(hr))
			MarkContainerDirty(self);
		return hr;
	}

	// call with the lock held
	void RegisterTexture(IDirect3DBaseTexture9* sys, IDirect3DBaseTexture9* vram, D3DRESOURCETYPE type)
	{
		TextureEntry entry;
		entry.vram = vram;
		entry.type = type;
		entry.dirty = true;
		entry.uploadFailed = false;
		g_textures[sys] = entry;
	}

	// ---------------------------------------------------------------------------------------------
	// buffers
	// ---------------------------------------------------------------------------------------------

	// call with the lock held; copies [offset, offset + size) of the shadow into the real buffer
	void UploadBufferRange(IDirect3DResource9* buffer, BufferEntry& entry, UINT offset, UINT size)
	{
		PFN_BufferLock lock = (PFN_BufferLock)FindOriginal(buffer, BufferSlot_Lock);
		PFN_BufferUnlock unlock = (PFN_BufferUnlock)FindOriginal(buffer, BufferSlot_Unlock);
		if (!lock || !unlock || size == 0)
			return;

		void* dst = nullptr;
		HRESULT hr = lock(buffer, offset, size, &dst, 0);
		if (FAILED(hr) || !dst)
		{
			if (!entry.uploadFailed)
			{
				Log("managed: locking a %u byte DEFAULT-pool buffer for upload failed: 0x%08lx", entry.length, hr);
				entry.uploadFailed = true;
			}
			return;
		}
		memcpy(dst, static_cast<const char*>(entry.shadow) + offset, size);
		unlock(buffer);
	}

	HRESULT STDMETHODCALLTYPE Hook_Buffer_Lock(IDirect3DResource9* self, UINT offset, UINT size, void** ppbData, DWORD flags)
	{
		{
			Guard guard;
			std::unordered_map<void*, BufferEntry>::iterator it = g_buffers.find(self);
			if (it != g_buffers.end())
			{
				BufferEntry& entry = it->second;
				if (!ppbData || offset > entry.length)
					return D3DERR_INVALIDCALL;
				UINT lockSize = size == 0 ? entry.length - offset : size;
				if (lockSize > entry.length - offset)
					lockSize = entry.length - offset;
				++entry.lockCount;
				if (!(flags & D3DLOCK_READONLY))
				{
					if (entry.dirtyBegin >= entry.dirtyEnd)
					{
						entry.dirtyBegin = offset;
						entry.dirtyEnd = offset + lockSize;
					}
					else
					{
						if (offset < entry.dirtyBegin)
							entry.dirtyBegin = offset;
						if (offset + lockSize > entry.dirtyEnd)
							entry.dirtyEnd = offset + lockSize;
					}
				}
				*ppbData = static_cast<char*>(entry.shadow) + offset;
				return D3D_OK;
			}
		}

		PFN_BufferLock original = (PFN_BufferLock)FindOriginal(self, BufferSlot_Lock);
		return original ? original(self, offset, size, ppbData, flags) : D3DERR_INVALIDCALL;
	}

	HRESULT STDMETHODCALLTYPE Hook_Buffer_Unlock(IDirect3DResource9* self)
	{
		{
			Guard guard;
			std::unordered_map<void*, BufferEntry>::iterator it = g_buffers.find(self);
			if (it != g_buffers.end())
			{
				BufferEntry& entry = it->second;
				if (entry.lockCount == 0)
					return D3DERR_INVALIDCALL;
				if (--entry.lockCount == 0)
				{
					if (entry.uploadAll)
						UploadBufferRange(self, entry, 0, entry.length);
					else if (entry.dirtyBegin < entry.dirtyEnd)
						UploadBufferRange(self, entry, entry.dirtyBegin, entry.dirtyEnd - entry.dirtyBegin);
					entry.uploadAll = false;
					entry.dirtyBegin = 0;
					entry.dirtyEnd = 0;
				}
				return D3D_OK;
			}
		}

		PFN_BufferUnlock original = (PFN_BufferUnlock)FindOriginal(self, BufferSlot_Unlock);
		return original ? original(self) : D3DERR_INVALIDCALL;
	}

	// call with the lock held
	bool RegisterBuffer(IDirect3DResource9* buffer, UINT length)
	{
		void* shadow = calloc(length ? length : 1, 1);
		if (!shadow)
			return false;

		BufferEntry entry;
		entry.shadow = shadow;
		entry.length = length;
		entry.lockCount = 0;
		entry.dirtyBegin = 0;
		entry.dirtyEnd = 0;
		entry.uploadAll = false;
		entry.uploadFailed = false;
		g_buffers[buffer] = entry;

		HookSlot(buffer, BufferSlot_Lock, (void*)&Hook_Buffer_Lock);
		HookSlot(buffer, BufferSlot_Unlock, (void*)&Hook_Buffer_Unlock);
		return true;
	}

	// call with the lock held: undo RegisterBuffer
	void UnregisterBuffer(IDirect3DResource9* buffer)
	{
		std::unordered_map<void*, BufferEntry>::iterator it = g_buffers.find(buffer);
		if (it != g_buffers.end())
		{
			free(it->second.shadow);
			g_buffers.erase(it);
		}
	}

	// call with the lock held: undo RegisterTexture (the twin is released right away, nobody else holds it)
	void UnregisterTexture(IDirect3DBaseTexture9* sys)
	{
		std::unordered_map<void*, TextureEntry>::iterator it = g_textures.find(sys);
		if (it != g_textures.end())
		{
			if (it->second.vram)
				it->second.vram->Release();
			g_textures.erase(it);
		}
	}
}

// -------------------------------------------------------------------------------------------------
// public interface
// -------------------------------------------------------------------------------------------------

namespace managed
{
	void Initialize()
	{
		InitializeCriticalSection(&g_lock);
	}

	void SetEnabled(bool enabled)
	{
		g_enabled = enabled;
	}

	bool Enabled()
	{
		return g_enabled;
	}

	HRESULT CreateTexture(IDirect3DDevice9Ex* device, PFN_CreateTexture create, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, IDirect3DTexture9** ppTexture)
	{
		// automatic mipmaps only exist on the GPU side; the system copy is the top level alone
		bool autoGenMips = (usage & D3DUSAGE_AUTOGENMIPMAP) != 0;
		IDirect3DTexture9* sys = nullptr;
		HRESULT hr = create(device, width, height, autoGenMips ? 1 : levels, usage & ~D3DUSAGE_AUTOGENMIPMAP, format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
		if (FAILED(hr) || !sys)
		{
			Log("managed: CreateTexture %ux%u levels %u usage 0x%lx format %d: system copy failed: 0x%08lx", width, height, levels, usage, format, hr);
			return FAILED(hr) ? hr : E_FAIL;
		}

		IDirect3DTexture9* vram = nullptr;
		hr = create(device, width, height, levels, usage, format, D3DPOOL_DEFAULT, &vram, nullptr);
		if (FAILED(hr) || !vram)
		{
			Log("managed: CreateTexture %ux%u levels %u usage 0x%lx format %d: DEFAULT twin failed: 0x%08lx", width, height, levels, usage, format, hr);
			sys->Release();
			return FAILED(hr) ? hr : E_FAIL;
		}

		{
			Guard guard;
			ReleasePending();
			RegisterTexture(sys, vram, D3DRTYPE_TEXTURE);
			HookSlot(sys, TextureSlot_Unlock, (void*)&Hook_Texture_UnlockRect);
			HookSlot(sys, TextureSlot_AddDirty, (void*)&Hook_Texture_AddDirtyRect);
			IDirect3DSurface9* surface = nullptr;
			if (SUCCEEDED(sys->GetSurfaceLevel(0, &surface)) && surface)
			{
				HookSlot(surface, SurfaceSlot_UnlockRect, (void*)&Hook_Surface_UnlockRect);
				surface->Release();
			}
		}
		if (!AttachSentinel(sys, Kind_Texture))
		{
			Guard guard;
			UnregisterTexture(sys);
			sys->Release();
			return E_FAIL;
		}

		if (Verbose())
			Log("managed: texture %ux%u levels %u usage 0x%lx format %d", width, height, levels, usage, format);
		*ppTexture = sys;
		return D3D_OK;
	}

	HRESULT CreateCubeTexture(IDirect3DDevice9Ex* device, PFN_CreateCubeTexture create, UINT edgeLength, UINT levels, DWORD usage, D3DFORMAT format, IDirect3DCubeTexture9** ppTexture)
	{
		bool autoGenMips = (usage & D3DUSAGE_AUTOGENMIPMAP) != 0;
		IDirect3DCubeTexture9* sys = nullptr;
		HRESULT hr = create(device, edgeLength, autoGenMips ? 1 : levels, usage & ~D3DUSAGE_AUTOGENMIPMAP, format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
		if (FAILED(hr) || !sys)
		{
			Log("managed: CreateCubeTexture %u levels %u usage 0x%lx format %d: system copy failed: 0x%08lx", edgeLength, levels, usage, format, hr);
			return FAILED(hr) ? hr : E_FAIL;
		}

		IDirect3DCubeTexture9* vram = nullptr;
		hr = create(device, edgeLength, levels, usage, format, D3DPOOL_DEFAULT, &vram, nullptr);
		if (FAILED(hr) || !vram)
		{
			Log("managed: CreateCubeTexture %u levels %u usage 0x%lx format %d: DEFAULT twin failed: 0x%08lx", edgeLength, levels, usage, format, hr);
			sys->Release();
			return FAILED(hr) ? hr : E_FAIL;
		}

		{
			Guard guard;
			ReleasePending();
			RegisterTexture(sys, vram, D3DRTYPE_CUBETEXTURE);
			HookSlot(sys, TextureSlot_Unlock, (void*)&Hook_Cube_UnlockRect);
			HookSlot(sys, TextureSlot_AddDirty, (void*)&Hook_Cube_AddDirtyRect);
			IDirect3DSurface9* surface = nullptr;
			if (SUCCEEDED(sys->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 0, &surface)) && surface)
			{
				HookSlot(surface, SurfaceSlot_UnlockRect, (void*)&Hook_Surface_UnlockRect);
				surface->Release();
			}
		}
		if (!AttachSentinel(sys, Kind_Texture))
		{
			Guard guard;
			UnregisterTexture(sys);
			sys->Release();
			return E_FAIL;
		}

		if (Verbose())
			Log("managed: cube texture %u levels %u usage 0x%lx format %d", edgeLength, levels, usage, format);
		*ppTexture = sys;
		return D3D_OK;
	}

	HRESULT CreateVolumeTexture(IDirect3DDevice9Ex* device, PFN_CreateVolumeTexture create, UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format, IDirect3DVolumeTexture9** ppTexture)
	{
		bool autoGenMips = (usage & D3DUSAGE_AUTOGENMIPMAP) != 0;
		IDirect3DVolumeTexture9* sys = nullptr;
		HRESULT hr = create(device, width, height, depth, autoGenMips ? 1 : levels, usage & ~D3DUSAGE_AUTOGENMIPMAP, format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
		if (FAILED(hr) || !sys)
		{
			Log("managed: CreateVolumeTexture %ux%ux%u levels %u usage 0x%lx format %d: system copy failed: 0x%08lx", width, height, depth, levels, usage, format, hr);
			return FAILED(hr) ? hr : E_FAIL;
		}

		IDirect3DVolumeTexture9* vram = nullptr;
		hr = create(device, width, height, depth, levels, usage, format, D3DPOOL_DEFAULT, &vram, nullptr);
		if (FAILED(hr) || !vram)
		{
			Log("managed: CreateVolumeTexture %ux%ux%u levels %u usage 0x%lx format %d: DEFAULT twin failed: 0x%08lx", width, height, depth, levels, usage, format, hr);
			sys->Release();
			return FAILED(hr) ? hr : E_FAIL;
		}

		{
			Guard guard;
			ReleasePending();
			RegisterTexture(sys, vram, D3DRTYPE_VOLUMETEXTURE);
			HookSlot(sys, TextureSlot_Unlock, (void*)&Hook_VolumeTexture_UnlockBox);
			HookSlot(sys, TextureSlot_AddDirty, (void*)&Hook_VolumeTexture_AddDirtyBox);
			IDirect3DVolume9* volume = nullptr;
			if (SUCCEEDED(sys->GetVolumeLevel(0, &volume)) && volume)
			{
				HookSlot(volume, VolumeSlot_UnlockBox, (void*)&Hook_Volume_UnlockBox);
				volume->Release();
			}
		}
		if (!AttachSentinel(sys, Kind_Texture))
		{
			Guard guard;
			UnregisterTexture(sys);
			sys->Release();
			return E_FAIL;
		}

		if (Verbose())
			Log("managed: volume texture %ux%ux%u levels %u usage 0x%lx format %d", width, height, depth, levels, usage, format);
		*ppTexture = sys;
		return D3D_OK;
	}

	HRESULT CreateVertexBuffer(IDirect3DDevice9Ex* device, PFN_CreateVertexBuffer create, UINT length, DWORD usage, DWORD fvf, IDirect3DVertexBuffer9** ppBuffer)
	{
		IDirect3DVertexBuffer9* buffer = nullptr;
		HRESULT hr = create(device, length, usage, fvf, D3DPOOL_DEFAULT, &buffer, nullptr);
		if (FAILED(hr) || !buffer)
		{
			Log("managed: CreateVertexBuffer %u bytes usage 0x%lx fvf 0x%lx: DEFAULT buffer failed: 0x%08lx", length, usage, fvf, hr);
			return FAILED(hr) ? hr : E_FAIL;
		}

		{
			Guard guard;
			ReleasePending();
			if (!RegisterBuffer(buffer, length))
			{
				Log("managed: CreateVertexBuffer %u bytes: out of memory for the system copy", length);
				buffer->Release();
				return E_OUTOFMEMORY;
			}
		}
		if (!AttachSentinel(buffer, Kind_Buffer))
		{
			Guard guard;
			UnregisterBuffer(buffer);
			buffer->Release();
			return E_FAIL;
		}

		if (Verbose())
			Log("managed: vertex buffer %u bytes usage 0x%lx fvf 0x%lx", length, usage, fvf);
		*ppBuffer = buffer;
		return D3D_OK;
	}

	HRESULT CreateIndexBuffer(IDirect3DDevice9Ex* device, PFN_CreateIndexBuffer create, UINT length, DWORD usage, D3DFORMAT format, IDirect3DIndexBuffer9** ppBuffer)
	{
		IDirect3DIndexBuffer9* buffer = nullptr;
		HRESULT hr = create(device, length, usage, format, D3DPOOL_DEFAULT, &buffer, nullptr);
		if (FAILED(hr) || !buffer)
		{
			Log("managed: CreateIndexBuffer %u bytes usage 0x%lx format %d: DEFAULT buffer failed: 0x%08lx", length, usage, format, hr);
			return FAILED(hr) ? hr : E_FAIL;
		}

		{
			Guard guard;
			ReleasePending();
			if (!RegisterBuffer(buffer, length))
			{
				Log("managed: CreateIndexBuffer %u bytes: out of memory for the system copy", length);
				buffer->Release();
				return E_OUTOFMEMORY;
			}
		}
		if (!AttachSentinel(buffer, Kind_Buffer))
		{
			Guard guard;
			UnregisterBuffer(buffer);
			buffer->Release();
			return E_FAIL;
		}

		if (Verbose())
			Log("managed: index buffer %u bytes usage 0x%lx format %d", length, usage, format);
		*ppBuffer = buffer;
		return D3D_OK;
	}

	IDirect3DBaseTexture9* ResolveTexture(IDirect3DDevice9Ex* device, IDirect3DBaseTexture9* texture)
	{
		Guard guard;
		if (!g_pendingRelease.empty())
			ReleasePending();

		std::unordered_map<void*, TextureEntry>::iterator it = g_textures.find(texture);
		if (it == g_textures.end())
			return texture;

		TextureEntry& entry = it->second;
		if (entry.dirty)
		{
			MarkWholeDirty(texture, entry.type);
			HRESULT hr = device->UpdateTexture(texture, entry.vram);
			if (SUCCEEDED(hr))
			{
				entry.dirty = false;
				entry.uploadFailed = false;
				++g_uploads;
			}
			else if (!entry.uploadFailed)
			{
				// stays dirty: the game may have it locked right now, the next SetTexture tries again
				Log("managed: UpdateTexture failed: 0x%08lx (resource type %d)", hr, entry.type);
				entry.uploadFailed = true;
			}
		}
		return entry.vram;
	}

	void OnReset(IDirect3DDevice9Ex* device)
	{
		(void)device;
		Guard guard;
		ReleasePending();

		unsigned textures = 0;
		for (std::unordered_map<void*, TextureEntry>::iterator it = g_textures.begin(); it != g_textures.end(); ++it)
		{
			it->second.dirty = true;
			++textures;
		}

		unsigned buffers = 0;
		for (std::unordered_map<void*, BufferEntry>::iterator it = g_buffers.begin(); it != g_buffers.end(); ++it)
		{
			BufferEntry& entry = it->second;
			if (entry.lockCount != 0)
			{
				entry.uploadAll = true;
				continue;
			}
			UploadBufferRange(static_cast<IDirect3DResource9*>(it->first), entry, 0, entry.length);
			++buffers;
		}

		Log("managed: after Reset %u textures are uploaded again on first use, %u buffers were uploaded again (%u uploads so far)", textures, buffers, g_uploads);
	}
}
