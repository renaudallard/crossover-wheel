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
	test_constant();
	test_periodic_and_condition();
	test_guids();
	test_wheel_match();

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
