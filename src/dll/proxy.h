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
 */
int	t150_client_start(void);
void	t150_client_stop(void);
int	t150_client_online(void);

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
	uint16_t		 slots;		/* one bit per slot in use */
	DWORD			 gain;		/* last DIPROP_FFGAIN seen */
	DWORD			 autocenter;	/* last DIPROP_AUTOCENTER seen */
	int			 pedal_swap;	/* present Y as gas, Rz as brake */
	int			 pedal_invert;	/* pedals rest at zero, not max */
	int			 ranges_stale;	/* re-ask dinput before mirroring */
	DWORD			 df_size;	/* dwDataSize of the game's data format */
	LONG			 range_min[2];	/* dinput's effective range, Y then Rz */
	LONG			 range_max[2];
};

HRESULT	t150_device_wrap(IDirectInputDevice8W *inner, int wide, void **out);

/* Take and give back a slot. Returns -1 when the wheel has no room left. */
int	t150_device_slot_alloc(struct t150_device *dev);
void	t150_device_slot_free(struct t150_device *dev, int slot);

HRESULT	t150_effect_create(struct t150_device *dev, REFGUID guid,
	    const DIEFFECT *params, IDirectInputEffect **out);

/*
 * Fold a DIEFFECT into the normalized model, honouring only the fields the
 * flags name and leaving the rest of ef alone. Exposed rather than kept
 * static because it is the fiddliest code in the proxy, and the only part
 * that can be checked without a wheel.
 */
void	t150_effect_convert(struct t150_effect *ef, const DIEFFECT *p,
	    DWORD flags);

/*
 * Which of our effect kinds a DirectInput effect GUID is, or
 * T150_EFFECT_NONE for one we do not recognise.
 */
uint8_t	t150_kind_from_guid(REFGUID guid);

/* Everything the proxy prints goes through here, and only when asked. */
void	t150_log(const char *fmt, ...);

#endif /* T150_PROXY_H */
