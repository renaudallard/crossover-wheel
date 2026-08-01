/*
 * main.c - the proxy's entry points and the wrapped IDirectInput8.
 *
 * Two doors lead into dinput8 and a game may use either. Most call the
 * exported DirectInput8Create; SDL never does, and goes through COM with
 * CoCreateInstance(CLSID_DirectInput8), which the loader resolves to an
 * absolute path in system32. Both are wrapped here, which is why this DLL
 * belongs in system32 rather than beside a game.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>
#include <string.h>

#include "proxy.h"

void	t150_client_init_lock(void);
void	t150_client_free_lock(void);

/*
 * What this DLL exports. Declared here because the compiler is right to
 * complain otherwise, and because the .def file is the only other place the
 * list appears: the two should be read together.
 */
HRESULT WINAPI DirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID *,
	    LPUNKNOWN);
HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, LPVOID *);
HRESULT WINAPI DllCanUnloadNow(void);
HRESULT WINAPI DllRegisterServer(void);
HRESULT WINAPI DllUnregisterServer(void);
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID);

static HMODULE self_module;
static HMODULE real;
static CRITICAL_SECTION load_lock;

static HRESULT (WINAPI *real_create)(HINSTANCE, DWORD, REFIID, LPVOID *,
	    LPUNKNOWN);
static HRESULT (WINAPI *real_getclass)(REFCLSID, REFIID, LPVOID *);
static HRESULT (WINAPI *real_canunload)(void);
static HRESULT (WINAPI *real_regserver)(void);
static HRESULT (WINAPI *real_unregserver)(void);

/*
 * Load the real implementation, which lives beside us under another name.
 *
 * It has to be another name: the loader keys modules on their base name, so
 * asking for dinput8.dll from inside dinput8.dll hands us back ourselves and
 * the first forwarded call becomes an infinite recursion. The installer is
 * what puts a copy of CrossOver's builtin there.
 */
static int
load_real(void)
{
	WCHAR path[MAX_PATH];
	DWORD n;
	int ok = 0;

	EnterCriticalSection(&load_lock);

	if (real != NULL) {
		LeaveCriticalSection(&load_lock);
		return 0;
	}

	n = GetModuleFileNameW(self_module, path, MAX_PATH);
	if (n > 0 && n < MAX_PATH) {
		WCHAR *slash = wcsrchr(path, L'\\');

		if (slash != NULL &&
		    (size_t)(slash - path) + 20 < MAX_PATH) {
			(void)wcscpy(slash + 1, L"dinput8_orig.dll");
			real = LoadLibraryW(path);
		}
	}
	if (real == NULL)
		real = LoadLibraryW(L"dinput8_orig.dll");
	if (real == NULL) {
		t150_log("cannot load dinput8_orig.dll, nothing will work\n");
		goto out;
	}

	real_create = (void *)GetProcAddress(real, "DirectInput8Create");
	real_getclass = (void *)GetProcAddress(real, "DllGetClassObject");
	real_canunload = (void *)GetProcAddress(real, "DllCanUnloadNow");
	real_regserver = (void *)GetProcAddress(real, "DllRegisterServer");
	real_unregserver = (void *)GetProcAddress(real, "DllUnregisterServer");

	if (real_create == NULL && real_getclass == NULL) {
		t150_log("dinput8_orig.dll has neither entry point\n");
		goto out;
	}
	ok = 1;

out:
	LeaveCriticalSection(&load_lock);

	return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */

struct dinput_wrap {
	const void	*vtbl;
	IDirectInput8W	*inner;
	LONG		 refs;
	int		 wide;
};

static struct dinput_wrap *
di_from(IDirectInput8W *p)
{
	return (struct dinput_wrap *)p;
}

#define DI_INNER(self) (di_from(self)->inner)

/*
 * Enumeration is where the wheel has to be smuggled in. Wine only lists a
 * device under DIEDFL_FORCEFEEDBACK when its descriptor carries a PID
 * collection, and the wheel's does not, so the game would never see it.
 */
struct enum_ctx {
	BOOL (WINAPI *cb)(const void *, void *);
	void	*ref;
	int	 wide;
	int	 saw_wheel;
	int	 wheel_only;
	BOOL	 stopped;
};

static BOOL WINAPI
enum_thunk(const void *inst, void *ref)
{
	const DIDEVICEINSTANCEW *w = inst;
	struct enum_ctx *ctx = ref;
	union {
		DIDEVICEINSTANCEW w;
		DIDEVICEINSTANCEA a;
	} copy;
	int wheel = t150_is_wheel(&w->guidProduct);

	if (wheel)
		ctx->saw_wheel = 1;
	if (ctx->wheel_only && !wheel)
		return DIENUM_CONTINUE;

	if (!wheel) {
		BOOL r = ctx->cb(inst, ctx->ref);

		if (r == DIENUM_STOP)
			ctx->stopped = TRUE;
		return r;
	}

	/*
	 * Hand the game a copy with a force feedback driver named, because a
	 * game that checks guidFFDriver before bothering with the device
	 * would otherwise skip it.
	 */
	memset(&copy, 0, sizeof(copy));
	if (w->dwSize > sizeof(copy))
		return ctx->cb(inst, ctx->ref);
	memcpy(&copy, inst, w->dwSize);

	if (ctx->wide) {
		if (copy.w.dwSize >= sizeof(DIDEVICEINSTANCEW))
			copy.w.guidFFDriver = IID_IDirectInputEffect;
	} else {
		if (copy.a.dwSize >= sizeof(DIDEVICEINSTANCEA))
			copy.a.guidFFDriver = IID_IDirectInputEffect;
	}

	return ctx->cb(&copy, ctx->ref);
}

static HRESULT WINAPI
di_QueryInterface(IDirectInput8W *self, REFIID iid, void **out)
{
	if (out == NULL)
		return E_POINTER;
	if (IsEqualGUID(iid, &IID_IUnknown) ||
	    IsEqualGUID(iid, &IID_IDirectInput8W) ||
	    IsEqualGUID(iid, &IID_IDirectInput8A)) {
		IDirectInput8_AddRef(self);
		*out = self;
		return S_OK;
	}

	return IDirectInput8_QueryInterface(DI_INNER(self), iid, out);
}

static ULONG WINAPI
di_AddRef(IDirectInput8W *self)
{
	return (ULONG)InterlockedIncrement(&di_from(self)->refs);
}

static ULONG WINAPI
di_Release(IDirectInput8W *self)
{
	struct dinput_wrap *d = di_from(self);
	LONG r = InterlockedDecrement(&d->refs);

	if (r == 0) {
		IDirectInput8_Release(d->inner);
		free(d);
	}

	return (ULONG)r;
}

static HRESULT WINAPI
di_CreateDevice(IDirectInput8W *self, REFGUID guid,
    LPDIRECTINPUTDEVICE8W *out, LPUNKNOWN outer)
{
	struct dinput_wrap *d = di_from(self);
	DIDEVICEINSTANCEW info;
	IDirectInputDevice8W *inner;
	HRESULT hr;

	if (out == NULL)
		return E_POINTER;

	hr = IDirectInput8_CreateDevice(d->inner, guid, &inner, outer);
	if (FAILED(hr))
		return hr;

	/*
	 * Only the wheel is wrapped, and only when the daemon is there to
	 * answer. Everything else is handed over untouched, so nothing else
	 * in the bottle pays for this DLL being installed.
	 */
	memset(&info, 0, sizeof(info));
	info.dwSize = sizeof(info);
	if (SUCCEEDED(IDirectInputDevice8_GetDeviceInfo(inner, &info)) &&
	    t150_is_wheel(&info.guidProduct) && t150_client_start() == 0) {
		void *wrapped;

		if (SUCCEEDED(t150_device_wrap(inner, d->wide, &wrapped))) {
			*out = wrapped;
			t150_log("wrapped the wheel\n");
			return hr;
		}
	}

	*out = inner;

	return hr;
}

static HRESULT WINAPI
di_EnumDevices(IDirectInput8W *self, DWORD type, LPDIENUMDEVICESCALLBACKW cb,
    LPVOID ref, DWORD flags)
{
	struct dinput_wrap *d = di_from(self);
	struct enum_ctx ctx;
	HRESULT hr;

	if (cb == NULL)
		return E_POINTER;

	memset(&ctx, 0, sizeof(ctx));
	ctx.cb = (void *)cb;
	ctx.ref = ref;
	ctx.wide = d->wide;

	hr = IDirectInput8_EnumDevices(d->inner, type, (void *)enum_thunk, &ctx,
	    flags);
	if (FAILED(hr))
		return hr;

	/*
	 * If the game asked for force feedback devices and the wheel was not
	 * among them, enumerate again without the filter and offer just the
	 * wheel. Anything else the second pass finds is dropped, so the game
	 * sees exactly what it asked for plus the one device we can drive.
	 */
	if ((flags & DIEDFL_FORCEFEEDBACK) && !ctx.saw_wheel && !ctx.stopped &&
	    t150_client_start() == 0) {
		ctx.wheel_only = 1;
		hr = IDirectInput8_EnumDevices(d->inner, type,
		    (void *)enum_thunk, &ctx, flags & ~DIEDFL_FORCEFEEDBACK);
	}

	return hr;
}

static HRESULT WINAPI
di_GetDeviceStatus(IDirectInput8W *self, REFGUID guid)
{
	return IDirectInput8_GetDeviceStatus(DI_INNER(self), guid);
}

static HRESULT WINAPI
di_RunControlPanel(IDirectInput8W *self, HWND owner, DWORD flags)
{
	return IDirectInput8_RunControlPanel(DI_INNER(self), owner, flags);
}

static HRESULT WINAPI
di_Initialize(IDirectInput8W *self, HINSTANCE inst, DWORD ver)
{
	return IDirectInput8_Initialize(DI_INNER(self), inst, ver);
}

static HRESULT WINAPI
di_FindDevice(IDirectInput8W *self, REFGUID guid, LPCWSTR name, LPGUID out)
{
	return IDirectInput8_FindDevice(DI_INNER(self), guid, name, out);
}

static HRESULT WINAPI
di_EnumDevicesBySemantics(IDirectInput8W *self, LPCWSTR user,
    LPDIACTIONFORMATW fmt, LPDIENUMDEVICESBYSEMANTICSCBW cb, LPVOID ref,
    DWORD flags)
{
	return IDirectInput8_EnumDevicesBySemantics(DI_INNER(self), user, fmt,
	    cb, ref, flags);
}

static HRESULT WINAPI
di_ConfigureDevices(IDirectInput8W *self, LPDICONFIGUREDEVICESCALLBACK cb,
    LPDICONFIGUREDEVICESPARAMSW params, DWORD flags, LPVOID ref)
{
	return IDirectInput8_ConfigureDevices(DI_INNER(self), cb, params, flags,
	    ref);
}

static const IDirectInput8WVtbl dinput_vtbl = {
	di_QueryInterface,
	di_AddRef,
	di_Release,
	di_CreateDevice,
	di_EnumDevices,
	di_GetDeviceStatus,
	di_RunControlPanel,
	di_Initialize,
	di_FindDevice,
	di_EnumDevicesBySemantics,
	di_ConfigureDevices
};

static HRESULT
wrap_dinput(void *inner, REFIID iid, void **out)
{
	struct dinput_wrap *d;

	if ((d = calloc(1, sizeof(*d))) == NULL) {
		IDirectInput8_Release((IDirectInput8W *)inner);
		return E_OUTOFMEMORY;
	}

	d->vtbl = &dinput_vtbl;
	d->inner = inner;
	d->refs = 1;
	d->wide = IsEqualGUID(iid, &IID_IDirectInput8W);
	*out = d;

	return DI_OK;
}

/* ------------------------------------------------------------------ */

/*
 * The COM door. SDL takes this one, so a proxy that only exported
 * DirectInput8Create would be invisible to every SDL game in the bottle.
 */
struct factory_wrap {
	const void	*vtbl;
	IClassFactory	*inner;
	LONG		 refs;
};

static struct factory_wrap *
cf_from(IClassFactory *p)
{
	return (struct factory_wrap *)p;
}

static HRESULT WINAPI
cf_QueryInterface(IClassFactory *self, REFIID iid, void **out)
{
	if (out == NULL)
		return E_POINTER;
	if (IsEqualGUID(iid, &IID_IUnknown) ||
	    IsEqualGUID(iid, &IID_IClassFactory)) {
		IClassFactory_AddRef(self);
		*out = self;
		return S_OK;
	}
	*out = NULL;

	return E_NOINTERFACE;
}

static ULONG WINAPI
cf_AddRef(IClassFactory *self)
{
	return (ULONG)InterlockedIncrement(&cf_from(self)->refs);
}

static ULONG WINAPI
cf_Release(IClassFactory *self)
{
	struct factory_wrap *f = cf_from(self);
	LONG r = InterlockedDecrement(&f->refs);

	if (r == 0) {
		IClassFactory_Release(f->inner);
		free(f);
	}

	return (ULONG)r;
}

static HRESULT WINAPI
cf_CreateInstance(IClassFactory *self, IUnknown *outer, REFIID iid, void **out)
{
	struct factory_wrap *f = cf_from(self);
	HRESULT hr;
	void *inner;

	if (out == NULL)
		return E_POINTER;
	*out = NULL;

	hr = IClassFactory_CreateInstance(f->inner, outer, iid, &inner);
	if (FAILED(hr))
		return hr;

	if (IsEqualGUID(iid, &IID_IDirectInput8W) ||
	    IsEqualGUID(iid, &IID_IDirectInput8A))
		return wrap_dinput(inner, iid, out);

	*out = inner;

	return hr;
}

static HRESULT WINAPI
cf_LockServer(IClassFactory *self, BOOL lock)
{
	return IClassFactory_LockServer(cf_from(self)->inner, lock);
}

static const IClassFactoryVtbl factory_vtbl = {
	cf_QueryInterface,
	cf_AddRef,
	cf_Release,
	cf_CreateInstance,
	cf_LockServer
};

/* ------------------------------------------------------------------ */

HRESULT WINAPI
DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid, LPVOID *out,
    LPUNKNOWN outer)
{
	HRESULT hr;
	void *inner;

	if (out == NULL)
		return E_POINTER;
	*out = NULL;

	if (load_real() != 0 || real_create == NULL)
		return DIERR_OLDDIRECTINPUTVERSION;

	hr = real_create(inst, version, iid, &inner, outer);
	if (FAILED(hr))
		return hr;

	if (IsEqualGUID(iid, &IID_IDirectInput8W) ||
	    IsEqualGUID(iid, &IID_IDirectInput8A))
		return wrap_dinput(inner, iid, out);

	*out = inner;

	return hr;
}

HRESULT WINAPI
DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID *out)
{
	struct factory_wrap *f;
	IClassFactory *inner;
	HRESULT hr;

	if (out == NULL)
		return E_POINTER;
	*out = NULL;

	if (load_real() != 0 || real_getclass == NULL)
		return CLASS_E_CLASSNOTAVAILABLE;

	hr = real_getclass(clsid, iid, (void **)&inner);
	if (FAILED(hr))
		return hr;

	/* Only the DirectInput 8 factory is worth wrapping. */
	if (!IsEqualGUID(clsid, &CLSID_DirectInput8) ||
	    !IsEqualGUID(iid, &IID_IClassFactory)) {
		*out = inner;
		return hr;
	}

	if ((f = calloc(1, sizeof(*f))) == NULL) {
		IClassFactory_Release(inner);
		return E_OUTOFMEMORY;
	}
	f->vtbl = &factory_vtbl;
	f->inner = inner;
	f->refs = 1;
	*out = f;

	return hr;
}

HRESULT WINAPI
DllCanUnloadNow(void)
{
	if (load_real() != 0 || real_canunload == NULL)
		return S_FALSE;

	return real_canunload();
}

HRESULT WINAPI
DllRegisterServer(void)
{
	if (load_real() != 0 || real_regserver == NULL)
		return E_FAIL;

	return real_regserver();
}

HRESULT WINAPI
DllUnregisterServer(void)
{
	if (load_real() != 0 || real_unregserver == NULL)
		return E_FAIL;

	return real_unregserver();
}

BOOL WINAPI
DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;

	switch (reason) {
	case DLL_PROCESS_ATTACH:
		self_module = inst;
		(void)DisableThreadLibraryCalls(inst);
		InitializeCriticalSection(&load_lock);
		t150_client_init_lock();
		break;
	case DLL_PROCESS_DETACH:
		/*
		 * Nothing that blocks or unloads: the loader lock is held
		 * here, so waiting on the keepalive thread would deadlock.
		 * The daemon's watchdog releases the wheel on its own, which
		 * is exactly the case it exists for.
		 */
		break;
	}

	return TRUE;
}
