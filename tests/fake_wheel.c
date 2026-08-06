/*
 * fake_wheel - a T150 shaped kernel input device, for CI.
 *
 * probe_dinput only reaches its interesting half once DirectInput has found
 * a wheel, so a runner with no hardware exercises the first twenty lines and
 * stops. Everything that has actually gone wrong in that tool lives past
 * that point: the object enumeration, the properties read from inside the
 * enumeration callback, and the arithmetic that turns an object into a
 * position in the state struct.
 *
 * uinput closes that gap. This creates a device carrying the T150's real USB
 * ids with the axes and buttons the wheel has, which Wine's bus driver picks
 * up like any other joystick, and holds it open until it is killed. It moves
 * nothing: the point is to be found and described, not to be driven.
 *
 * Linux only, and not part of any build. CI compiles it where it needs it.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <linux/uinput.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * The wheel as the probe expects to meet it: steering, three pedals and the
 * hat on the absolute axes, thirteen buttons on the keys. The exact codes
 * matter only in that they are the ones a joystick uses, because that is
 * what makes the bus driver treat this as a game controller.
 */
static const int abs_codes[] = {
	ABS_X,		/* steering */
	ABS_Y,		/* brake */
	ABS_Z,		/* clutch */
	ABS_RZ,		/* accelerator */
	ABS_HAT0X,
	ABS_HAT0Y
};

static const int key_codes[] = {
	BTN_TRIGGER, BTN_THUMB, BTN_THUMB2, BTN_TOP, BTN_TOP2, BTN_PINKIE,
	BTN_BASE, BTN_BASE2, BTN_BASE3, BTN_BASE4, BTN_BASE5, BTN_BASE6,
	BTN_DEAD
};

int
main(void)
{
	struct uinput_setup setup;
	struct uinput_abs_setup abs;
	size_t i;
	int fd;

	if ((fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK)) < 0) {
		perror("/dev/uinput");
		return 1;
	}

	if (ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
	    ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) {
		perror("UI_SET_EVBIT");
		return 1;
	}

	for (i = 0; i < sizeof(abs_codes) / sizeof(abs_codes[0]); i++) {
		memset(&abs, 0, sizeof(abs));
		abs.code = abs_codes[i];
		abs.absinfo.minimum = abs_codes[i] >= ABS_HAT0X ? -1 : 0;
		abs.absinfo.maximum = abs_codes[i] >= ABS_HAT0X ? 1 : 65535;

		if (ioctl(fd, UI_SET_ABSBIT, abs_codes[i]) < 0 ||
		    ioctl(fd, UI_ABS_SETUP, &abs) < 0) {
			perror("UI_ABS_SETUP");
			return 1;
		}
	}

	for (i = 0; i < sizeof(key_codes) / sizeof(key_codes[0]); i++)
		if (ioctl(fd, UI_SET_KEYBIT, key_codes[i]) < 0) {
			perror("UI_SET_KEYBIT");
			return 1;
		}

	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_USB;
	setup.id.vendor = 0x044f;
	setup.id.product = 0xb677;
	setup.id.version = 1;
	(void)snprintf(setup.name, sizeof(setup.name),
	    "Thrustmaster T150 Racing Wheel");

	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 ||
	    ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("UI_DEV_CREATE");
		return 1;
	}

	/*
	 * The device lives exactly as long as this process holds the
	 * descriptor, so say it is ready and then do nothing at all.
	 */
	printf("fake wheel created\n");
	(void)fflush(stdout);

	for (;;)
		pause();
}
