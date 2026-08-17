/*
 * proxy.h - internals of the in-bottle DirectInput 8 proxy.
 *
 * The proxy forwards everything it does not care about to the real builtin
 * dinput8, and wraps only the force feedback surface of the one device it
 * recognises. Every other device, and every other call, reaches the builtin
 * untouched and unwrapped, because the cheapest way to avoid breaking a
 * game is to not be in its way.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef T150_PROXY_H
#define T150_PROXY_H

/* The Makefile stamps this from git describe; a bare compile still builds. */
#ifndef T150_PROXY_VERSION
#define T150_PROXY_VERSION "unknown"
#endif

#define DIRECTINPUT_VERSION 0x0800

/* COBJMACROS gets the IClassFactory_* call helpers; dinput.h has its own. */
#define COBJMACROS

#include <windows.h>
#include <objbase.h>
#include <dinput.h>

#include <stdint.h>

#include "t150/effect.h"
#include "t150/proto.h"

/*
 * The daemon connection. One per process, guarded by its own lock, because
 * DirectInput calls arrive from whichever thread the game feels like using.
 * There is no stop: a DLL's only teardown hook is DllMain, which cannot wait
 * on the keepalive thread without deadlocking the loader, and a game on its
 * way out is what the daemon's watchdog is for.
 */
int	t150_client_start(void);

/*
 * Which connection this is, and whether it is up, under one lock rather than
 * two. An effect that uploaded to an older one has to say itself again,
 * because the daemon on the other end of a new connection has never heard of
 * it, and an upload has to know both facts to decide anything. Pass NULL for
 * online when only the generation is wanted.
 */
unsigned int t150_client_state(int *online);

/*
 * Send one frame and wait for its reply. Returns 0 when the daemon answered
 * with OK. A failure here drops the connection rather than retrying, because
 * a half spoken protocol is worse than no force feedback.
 */
int	t150_client_call(uint8_t op, const void *payload, size_t len);

/* The wheel this proxy exists for, matched on the product GUID. */
int	t150_is_wheel(const GUID *product);

/*
 * A wrapped device. The effect objects it hands out point back at it.
 *
 * The vtable pointer is first and must stay first: a game holds this as an
 * IDirectInputDevice8 and the whole thing works by the two layouts agreeing
 * at offset zero.
 */
struct t150_device {
	const void		*vtbl;
	IDirectInputDevice8W	*inner;
	LONG			 refs;
	int			 wide;		/* the game asked for the A or W interface */
	DWORD			 gain;		/* last DIPROP_FFGAIN seen */
	DWORD			 autocenter;	/* last DIPROP_AUTOCENTER seen */
	/*
	 * The connection those two were last told to. A restarted daemon is a
	 * fresh session that starts at full gain and knows nothing of what the
	 * game asked for, and DirectInput gives a game no reason to set a
	 * property twice, so the proxy is the only thing that can say it
	 * again.
	 */
	unsigned int		 prop_gen;
	int			 props_set;
	int			 pedal_swap;	/* present Y as gas, Z as brake */
	int			 pedal_invert;	/* pedals rest at zero, not max */
	int			 pedals_found;	/* both pedal axes are really there */
	int			 ranges_stale;	/* re-ask dinput before mirroring */
	DWORD			 df_size;	/* dwDataSize of the game's data format */
	LONG			 range_min[2];	/* dinput's effective range, Y then Z */
	LONG			 range_max[2];
};

HRESULT	t150_device_wrap(IDirectInputDevice8W *inner, int wide, void **out);

/*
 * Take and give back one of the wheel's effect slots. Returns -1 when there
 * is no room left. Process wide, not per device: see device.c for why.
 */
int	t150_slot_alloc(void);
void	t150_slot_free(int slot);

HRESULT	t150_effect_create(struct t150_device *dev, REFGUID guid,
	    const DIEFFECT *params, IDirectInputEffect **out);

/*
 * Walk the live effect objects a device created, which is what DirectInput's
 * EnumCreatedEffectObjects is for.
 */
HRESULT	t150_effect_enum(struct t150_device *dev,
	    LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref);

/* Mark every effect this process holds as no longer playing. */
void	t150_effect_all_stopped(void);

/*
 * The same for a pause, which remembers what was running, and the continue
 * that puts it back. DISFFC_PAUSE has no wire opcode, so the proxy stops
 * everything for it and only this can tell a continue what to restart.
 */
void	t150_effect_all_paused(void);
void	t150_effect_all_continued(void);

/*
 * Mark every effect this process holds as no longer downloaded, which a
 * device level reset makes true and only the device knows. See upload().
 */
void	t150_effect_all_unloaded(void);

/* Say the device properties again if the daemon on the other end is new. */
void	t150_device_replay_props(struct t150_device *dev);

/*
 * Fold a DIEFFECT into the normalized model, honouring only the fields the
 * flags name and leaving the rest of ef alone. Exposed rather than kept
 * static because it is the fiddliest code in the proxy, and the only part
 * that can be checked without a wheel.
 */
void	t150_effect_convert(struct t150_effect *ef, const DIEFFECT *p,
	    DWORD flags);

/*
 * And the way back: a stored direction in the coordinate system a caller of
 * GetParameters asked for. DirectInput's contract is that the caller names
 * one and the device converts, which is what this is for.
 */
void	t150_direction_out(uint32_t direction, DWORD system, LONG *out,
	    DWORD naxes);

/*
 * Which of our effect kinds a DirectInput effect GUID is, or
 * T150_EFFECT_NONE for one we do not recognise.
 */
uint8_t	t150_kind_from_guid(REFGUID guid);

/* Everything the proxy prints goes through here, and only when asked. */
void	t150_log(const char *fmt, ...);

#endif /* T150_PROXY_H */
