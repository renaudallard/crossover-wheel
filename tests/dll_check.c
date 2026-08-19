/*
 * dll_check - what the proxy can be checked for without a wheel.
 *
 * Two halves. The first unit tests the DIEFFECT conversion, which is the
 * fiddliest code in the proxy and the part most likely to be quietly wrong:
 * a direction handled badly produces a game that reports success and a wheel
 * that does nothing.
 *
 * The second loads the built DLL the way a game would, with a copy of the
 * real dinput8 beside it under the name the proxy chain-loads, and checks
 * that both doors into it work and that with no daemon listening it stays
 * out of the way.
 *
 * This runs on Windows, in CI. It cannot prove anything about a wheel.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "proxy.h"

static int failures;

static void
fail(const char *what)
{
	fprintf(stderr, "FAIL %s\n", what);
	failures++;
}

static void
check_u32(const char *what, uint32_t got, uint32_t want)
{
	if (got == want)
		return;
	fprintf(stderr, "FAIL %s: want %lu, got %lu\n", what,
	    (unsigned long)want, (unsigned long)got);
	failures++;
}

static void
check_i32(const char *what, int32_t got, int32_t want)
{
	if (got == want)
		return;
	fprintf(stderr, "FAIL %s: want %ld, got %ld\n", what, (long)want,
	    (long)got);
	failures++;
}

#define ALL_PARAMS							\
	(DIEP_DURATION | DIEP_STARTDELAY | DIEP_GAIN | DIEP_DIRECTION |	\
	 DIEP_ENVELOPE | DIEP_TYPESPECIFICPARAMS)

static void
test_direction(void)
{
	struct t150_effect ef;
	DIEFFECT eff;
	LONG dir[2];

	memset(&eff, 0, sizeof(eff));
	eff.dwSize = sizeof(eff);
	eff.rglDirection = dir;

	/*
	 * One axis is the case that matters, because a wheel has one axis and
	 * DirectInput's direction array is degenerate there: the side lives
	 * in the sign of the magnitude. Taking the direction literally would
	 * hand the encoder due north, which projects onto no force at all.
	 */
	memset(&ef, 0, sizeof(ef));
	eff.cAxes = 1;
	eff.dwFlags = DIEFF_CARTESIAN;
	dir[0] = 0;
	t150_effect_convert(&ef, &eff, DIEP_DIRECTION);
	check_u32("one axis is due east", ef.direction, 9000);

	dir[0] = -1;
	t150_effect_convert(&ef, &eff, DIEP_DIRECTION);
	check_u32("one negative axis is due west", ef.direction, 27000);

	/*
	 * Only cartesian is degenerate on one axis. A polar or spherical
	 * angle means the same thing whatever the axis count, and dropping it
	 * puts every force on the same side. SDL sends exactly this shape,
	 * one axis and a real polar angle, once an axis is marked as a force
	 * feedback actuator.
	 */
	eff.dwFlags = DIEFF_POLAR;
	dir[0] = 18000;
	t150_effect_convert(&ef, &eff, DIEP_DIRECTION);
	check_u32("one axis keeps its polar angle", ef.direction, 18000);

	dir[0] = -9000;
	t150_effect_convert(&ef, &eff, DIEP_DIRECTION);
	check_u32("one axis wraps a negative polar angle", ef.direction, 27000);

	eff.dwFlags = DIEFF_SPHERICAL;
	dir[0] = 0;
	t150_effect_convert(&ef, &eff, DIEP_DIRECTION);
	check_u32("one axis keeps its spherical angle", ef.direction, 9000);

	/* No direction at all is still due east rather than due north. */
	eff.dwFlags = DIEFF_POLAR;
	eff.cAxes = 0;
	eff.rglDirection = NULL;
	t150_effect_convert(&ef, &eff, DIEP_DIRECTION);
	check_u32("no axes and no direction is due east", ef.direction, 9000);
	eff.rglDirection = dir;

	/* Polar is already in our units. */
	eff.cAxes = 2;
	eff.dwFlags = DIEFF_POLAR;
	dir[0] = 27000;
	t150_effect_convert(&ef, &eff, DIEP_DIRECTION);
	check_u32("polar passes through", ef.direction, 27000);

	dir[0] = -9000;
	t150_effect_convert(&ef, &eff, DIEP_DIRECTION);
	check_u32("a negative polar angle wraps", ef.direction, 27000);

	/* Cartesian, measured from the axis pointing away from the player. */
	eff.dwFlags = DIEFF_CARTESIAN;
	dir[0] = 1;
	dir[1] = 0;
	t150_effect_convert(&ef, &eff, DIEP_DIRECTION);
	check_u32("cartesian +x is east", ef.direction, 9000);

	dir[0] = -1;
	dir[1] = 0;
	t150_effect_convert(&ef, &eff, DIEP_DIRECTION);
	check_u32("cartesian -x is west", ef.direction, 27000);

	dir[0] = 0;
	dir[1] = -1;
	t150_effect_convert(&ef, &eff, DIEP_DIRECTION);
	check_u32("cartesian -y is north", ef.direction, 0);

	/* Spherical is polar turned a quarter turn. */
	eff.dwFlags = DIEFF_SPHERICAL;
	dir[0] = 0;
	t150_effect_convert(&ef, &eff, DIEP_DIRECTION);
	check_u32("spherical zero is east", ef.direction, 9000);
}

/*
 * A direction survives being read back out and written in again.
 *
 * GetParameters answered in polar whatever the caller asked for, and rewrote
 * the caller's flags to say polar. That breaks the read-modify-write pattern
 * that exists so members the caller does not touch survive: a game filling a
 * DIEFFECT with DIEFF_CARTESIAN, reading everything, changing only the
 * magnitude and writing it back handed direction_of a polar angle in a struct
 * still flagged cartesian. On one axis that is read as a sign, so a force
 * pointing left came back as a large positive number and pushed right.
 *
 * The two halves are t150_direction_out and t150_effect_convert, so the round
 * trip can be checked here without a device.
 */
static void
test_direction_round_trip(void)
{
	static const uint32_t dirs[] = { 0, 4500, 9000, 18000, 22500, 27000, 31500 };
	static const DWORD systems[] = { DIEFF_POLAR, DIEFF_SPHERICAL,
	    DIEFF_CARTESIAN };
	size_t i, j;

	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		for (j = 0; j < sizeof(systems) / sizeof(systems[0]); j++) {
			struct t150_effect ef;
			DIEFFECT p;
			LONG axes[2] = { 0, 0 };
			DWORD naxes = systems[j] == DIEFF_CARTESIAN ? 2 : 1;

			t150_direction_out(dirs[i], systems[j], axes, naxes);

			memset(&ef, 0, sizeof(ef));
			memset(&p, 0, sizeof(p));
			p.dwSize = sizeof(p);
			p.dwFlags = systems[j];
			p.cAxes = naxes;
			p.rglDirection = axes;
			t150_effect_convert(&ef, &p, DIEP_DIRECTION);

			/*
			 * Cartesian on two axes goes through sin, cos and
			 * atan2, so it comes back within a rounding step
			 * rather than exactly. A whole degree is far tighter
			 * than the wheel can tell: t150_dir_sin feeds one
			 * signed byte.
			 */
			if (dirs[i] > ef.direction ?
			    dirs[i] - ef.direction > 100 :
			    ef.direction - dirs[i] > 100) {
				fprintf(stderr, "FAIL direction %lu in system "
				    "0x%lx came back as %lu\n",
				    (unsigned long)dirs[i],
				    (unsigned long)systems[j],
				    (unsigned long)ef.direction);
				failures++;
			}
		}
	}

	/* One axis carries a side and not an angle, so only the side survives. */
	{
		struct t150_effect ef;
		DIEFFECT p;
		LONG axis = 0;

		t150_direction_out(27000, DIEFF_CARTESIAN, &axis, 1);
		if (axis >= 0) {
			fprintf(stderr, "FAIL a westward force is not negative "
			    "on one cartesian axis\n");
			failures++;
		}
		memset(&ef, 0, sizeof(ef));
		memset(&p, 0, sizeof(p));
		p.dwSize = sizeof(p);
		p.dwFlags = DIEFF_CARTESIAN;
		p.cAxes = 1;
		p.rglDirection = &axis;
		t150_effect_convert(&ef, &p, DIEP_DIRECTION);
		check_u32("one cartesian axis keeps its side", ef.direction,
		    27000);
	}
}

/*
 * A DIEFFECT that ends before dwStartDelay is not read past its end.
 *
 * That member is the one DirectInput 6 added, so a caller built against the
 * older headers passes a struct which stops before it, and dwSize is how it
 * says which it has. Both guards, on the way in at t150_effect_convert and on
 * the way out at eff_GetParameters, exist for a memory safety defect a
 * previous audit found, and RESEARCH.md records it as one of two.
 *
 * Every DIEFFECT this file built set dwSize to sizeof(DIEFFECT), so the guard
 * was never taken in its false direction: someone simplifying that function
 * could drop the test, since the flags argument already looks like it names
 * the field, and the whole suite would stay green while a DirectX 5 era game
 * had eight bytes read off the end of its stack.
 *
 * The struct here is deliberately shorter than a DIEFFECT and the bytes past
 * its end are poisoned, so reading them shows up as a value rather than as
 * whatever happened to be there.
 */
/*
 * The whole test rests on a DIEFFECT_DX5 being a DIEFFECT that stops exactly
 * where dwStartDelay begins, so that the poison past the end of the caller's
 * struct lands on that member and nowhere else. Checked against the real
 * header rather than assumed: the first version of this asserted dwStartDelay
 * was the last member and was wrong, because on x86_64 the struct is padded
 * out past it.
 */
_Static_assert(sizeof(DIEFFECT_DX5) == offsetof(DIEFFECT, dwStartDelay),
    "a DIEFFECT_DX5 is no longer a DIEFFECT stopping before dwStartDelay");

static void
test_a_dx5_effect_is_not_read_past_its_end(void)
{
	/*
	 * A caller's struct that really does end early, with everything past
	 * it poisoned. A full sized DIEFFECT with a small dwSize would not do:
	 * the member is still there and still zero, so reading it unguarded
	 * would give the same answer as not reading it and the test would pass
	 * either way.
	 */
	unsigned char raw[sizeof(DIEFFECT) + 4 * sizeof(DWORD)];
	const size_t dx5 = sizeof(DIEFFECT_DX5);
	DIEFFECT *p = (DIEFFECT *)(void *)raw;
	DICONSTANTFORCE cf;
	struct t150_effect out;
	LONG dir = 0;
	DWORD axis = DIJOFS_X;

	memset(&cf, 0, sizeof(cf));
	cf.lMagnitude = 4200;

	memset(raw, 0xee, sizeof(raw));		/* past the end, and visible */
	memset(raw, 0, dx5);			/* the caller's own struct */

	p->dwSize = (DWORD)dx5;
	p->dwFlags = DIEFF_OBJECTOFFSETS | DIEFF_CARTESIAN;
	p->dwDuration = 250000;
	p->dwGain = 6000;
	p->cAxes = 1;
	p->rgdwAxes = &axis;
	p->rglDirection = &dir;
	p->cbTypeSpecificParams = sizeof(cf);
	p->lpvTypeSpecificParams = &cf;

	memset(&out, 0, sizeof(out));
	out.kind = T150_EFFECT_CONSTANT;
	t150_effect_convert(&out, p, ALL_PARAMS);

	check_u32("a DX5 effect still gives its duration", out.duration, 250000);
	check_u32("and its gain", out.gain, 6000);
	check_i32("and its magnitude", out.u.constant.magnitude, 4200);
	/*
	 * The one that matters. Unguarded this reads the poison, because
	 * dwStartDelay sits exactly where the caller's struct stopped.
	 */
	check_u32("and no start delay is read past its end", out.start_delay, 0);

	/* And a full sized one still carries one, so the guard is not a ban. */
	memset(raw, 0, sizeof(raw));
	p->dwSize = sizeof(DIEFFECT);
	p->dwFlags = DIEFF_OBJECTOFFSETS | DIEFF_CARTESIAN;
	p->cAxes = 1;
	p->rgdwAxes = &axis;
	p->rglDirection = &dir;
	p->dwStartDelay = 50000;
	memset(&out, 0, sizeof(out));
	out.kind = T150_EFFECT_CONSTANT;
	t150_effect_convert(&out, p, ALL_PARAMS);
	check_u32("a full sized effect still carries its start delay",
	    out.start_delay, 50000);
}

static void
test_constant(void)
{
	DICONSTANTFORCE cf = { -7500 };
	struct t150_effect ef;
	DIENVELOPE env;
	DIEFFECT eff;
	LONG dir[1] = { 0 };

	memset(&env, 0, sizeof(env));
	env.dwSize = sizeof(env);
	env.dwAttackLevel = 4000;
	env.dwAttackTime = 100000;
	env.dwFadeLevel = 2000;
	env.dwFadeTime = 250000;

	memset(&eff, 0, sizeof(eff));
	eff.dwSize = sizeof(eff);
	eff.dwDuration = 2000000;
	eff.dwStartDelay = 50000;
	eff.dwGain = 7500;
	eff.cAxes = 1;
	eff.dwFlags = DIEFF_CARTESIAN;
	eff.rglDirection = dir;
	eff.lpEnvelope = &env;
	eff.cbTypeSpecificParams = sizeof(cf);
	eff.lpvTypeSpecificParams = &cf;

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_CONSTANT;
	t150_effect_convert(&ef, &eff, ALL_PARAMS);

	check_u32("duration", ef.duration, 2000000);
	check_u32("start delay", ef.start_delay, 50000);
	check_u32("gain", ef.gain, 7500);
	check_i32("magnitude", ef.u.constant.magnitude, -7500);
	check_u32("envelope present", ef.envelope.present, 1);
	check_u32("attack time", ef.envelope.attack_time, 100000);
	check_i32("attack level", ef.envelope.attack_level, 4000);
	check_u32("fade time", ef.envelope.fade_time, 250000);
	check_i32("fade level", ef.envelope.fade_level, 2000);

	/* An infinite duration must survive rather than being clamped. */
	eff.dwDuration = INFINITE;
	t150_effect_convert(&ef, &eff, DIEP_DURATION);
	check_u32("infinite duration", ef.duration, T150_DURATION_INFINITE);

	/* Only the named fields move. */
	eff.dwGain = 1;
	t150_effect_convert(&ef, &eff, DIEP_DURATION);
	check_u32("an unnamed field is left alone", ef.gain, 7500);
}

static void
test_periodic_and_condition(void)
{
	DIPERIODIC per = { 6000, -1000, 9000, 20000 };
	DICONDITION cond;
	struct t150_effect ef;
	DIEFFECT eff;

	memset(&eff, 0, sizeof(eff));
	eff.dwSize = sizeof(eff);
	eff.cbTypeSpecificParams = sizeof(per);
	eff.lpvTypeSpecificParams = &per;

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_SINE;
	t150_effect_convert(&ef, &eff, DIEP_TYPESPECIFICPARAMS);
	check_i32("periodic magnitude", ef.u.periodic.magnitude, 6000);
	check_i32("periodic offset", ef.u.periodic.offset, -1000);
	check_u32("periodic phase", ef.u.periodic.phase, 9000);
	check_u32("periodic period", ef.u.periodic.period, 20000);

	memset(&cond, 0, sizeof(cond));
	cond.lOffset = -2500;
	cond.lPositiveCoefficient = 8000;
	cond.lNegativeCoefficient = -8000;
	cond.dwPositiveSaturation = 9000;
	cond.dwNegativeSaturation = 7000;
	cond.lDeadBand = 500;
	eff.cbTypeSpecificParams = sizeof(cond);
	eff.lpvTypeSpecificParams = &cond;

	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_SPRING;
	t150_effect_convert(&ef, &eff, DIEP_TYPESPECIFICPARAMS);
	check_i32("condition centre", ef.u.condition.center, -2500);
	check_i32("positive coefficient", ef.u.condition.pos_coeff, 8000);
	check_i32("negative coefficient", ef.u.condition.neg_coeff, -8000);
	check_i32("positive saturation", ef.u.condition.pos_saturation, 9000);
	check_i32("negative saturation", ef.u.condition.neg_saturation, 7000);
	check_i32("deadband", ef.u.condition.deadband, 500);

	/* A payload too short to hold the struct is ignored, not read. */
	eff.cbTypeSpecificParams = 4;
	memset(&ef, 0, sizeof(ef));
	ef.kind = T150_EFFECT_SPRING;
	t150_effect_convert(&ef, &eff, DIEP_TYPESPECIFICPARAMS);
	check_i32("a short payload is refused", ef.u.condition.pos_coeff, 0);
}

static void
test_guids(void)
{
	check_u32("constant guid", t150_kind_from_guid(&GUID_ConstantForce),
	    T150_EFFECT_CONSTANT);
	check_u32("sine guid", t150_kind_from_guid(&GUID_Sine),
	    T150_EFFECT_SINE);
	check_u32("spring guid", t150_kind_from_guid(&GUID_Spring),
	    T150_EFFECT_SPRING);
	check_u32("inertia guid", t150_kind_from_guid(&GUID_Inertia),
	    T150_EFFECT_INERTIA);
	check_u32("an unknown guid", t150_kind_from_guid(&GUID_XAxis),
	    T150_EFFECT_NONE);
}

static void
test_wheel_match(void)
{
	GUID g;

	memset(&g, 0, sizeof(g));
	g.Data1 = 0xb677044f;
	if (!t150_is_wheel(&g))
		fail("the wheel's product guid was not recognised");

	g.Data1 = 0xb65d044f;
	if (t150_is_wheel(&g))
		fail("the boot mode product id was taken for the wheel");

	g.Data1 = 0xc294046d;
	if (t150_is_wheel(&g))
		fail("another vendor's wheel was taken for ours");
}

/*
 * The GUID the proxy hands out in place of the one DirectInput derives from
 * the order the wheel arrived in.
 *
 * The last two cases are what the constant was chosen to keep apart. Wine
 * builds a joystick's instance GUID by XORing a fixed one with a small
 * counter, so anything in that family has to answer no however many times the
 * counter has been round; and t150_is_wheel matches on the first word alone,
 * so a constant sharing that word with the product GUID would be one mistake
 * away from being taken for a product.
 */
static void
test_stable_instance(void)
{
	/* dlls/dinput/joystick_hid.c, hid_joystick_guid. */
	static const GUID wine_joystick = { 0x9e573edb, 0x7734, 0x11d2,
	    { 0x8d, 0x4a, 0x23, 0x90, 0x3f, 0xb6, 0xbd, 0xf7 } };
	GUID g;
	unsigned i;

	if (!t150_is_stable_instance(&t150_instance_guid))
		fail("the proxy's own instance guid was not recognised");

	if (t150_is_stable_instance(&GUID_NULL))
		fail("a null guid was taken for the proxy's own");

	for (i = 0; i < 64; i++) {
		g = wine_joystick;
		g.Data1 ^= i;
		if (t150_is_stable_instance(&g))
			fail("a wine joystick instance guid was taken for "
			    "the proxy's own");
	}

	if (t150_is_wheel(&t150_instance_guid))
		fail("the instance guid was taken for a product guid");
}

/*
 * Load the DLL as a game would. The chain-load target has to exist first,
 * so the caller copies the system dinput8 next to it under that name.
 */
static void
test_load(const char *path)
{
	HRESULT (WINAPI *create)(HINSTANCE, DWORD, REFIID, void **, LPUNKNOWN);
	HRESULT (WINAPI *getclass)(REFCLSID, REFIID, void **);
	IDirectInput8W *di = NULL;
	IClassFactory *cf = NULL;
	HMODULE m;

	if ((m = LoadLibraryA(path)) == NULL) {
		fail("the proxy would not load");
		return;
	}

	create = (void *)GetProcAddress(m, "DirectInput8Create");
	getclass = (void *)GetProcAddress(m, "DllGetClassObject");
	if (create == NULL || getclass == NULL) {
		fail("the proxy is missing an export");
		(void)FreeLibrary(m);
		return;
	}

	/* The door most games use. */
	if (FAILED(create(GetModuleHandleW(NULL), DIRECTINPUT_VERSION,
	    &IID_IDirectInput8W, (void **)&di, NULL)) || di == NULL) {
		fail("DirectInput8Create did not chain load");
		(void)FreeLibrary(m);
		return;
	}

	/*
	 * With no daemon listening this must behave exactly like the real
	 * thing. There is no wheel on a build machine either, so the point
	 * is that nothing crashes and nothing is invented.
	 */
	if (FAILED(IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, NULL,
	    NULL, DIEDFL_ATTACHEDONLY)))
		/* A null callback is rejected, which is the correct answer. */
		(void)0;

	IDirectInput8_Release(di);

	/* The door SDL uses. */
	if (FAILED(getclass(&CLSID_DirectInput8, &IID_IClassFactory,
	    (void **)&cf)) || cf == NULL) {
		fail("DllGetClassObject did not chain load");
		(void)FreeLibrary(m);
		return;
	}
	if (FAILED(IClassFactory_CreateInstance(cf, NULL, &IID_IDirectInput8W,
	    (void **)&di)) || di == NULL)
		fail("the class factory made no IDirectInput8");
	else
		IDirectInput8_Release(di);

	IClassFactory_Release(cf);
	(void)FreeLibrary(m);
}

int
main(int argc, char *argv[])
{
	test_direction();
	test_direction_round_trip();
	test_a_dx5_effect_is_not_read_past_its_end();
	test_constant();
	test_periodic_and_condition();
	test_guids();
	test_wheel_match();
	test_stable_instance();

	if (argc > 1)
		test_load(argv[1]);
	else
		printf("dll_check: no dll given, skipped the load test\n");

	if (failures != 0) {
		fprintf(stderr, "dll_check: %d failure(s)\n", failures);
		return 1;
	}

	printf("dll_check: ok\n");

	return 0;
}
