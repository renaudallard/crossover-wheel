/*
 * bootswitch.h - taking a T-series wheel out of boot mode.
 *
 * Every T-series wheel enumerates at the shared boot product id and reveals
 * its own identity, and its own protocol, only after a pair of vendor control
 * transfers on endpoint 0. Sleep, wake and every replug put it back, so this
 * is not a one-off at install time: anything that wants the wheel has to be
 * able to do it.
 *
 * Two callers want that, for different reasons and with different opinions.
 * t150boot is the tool a person or a LaunchAgent runs, and it has a great
 * many opinions: which model, which switch value, how loud to be, and whether
 * the wheel came back. The daemon has none of those. It only needs the wheel
 * to exist, and it asks in the middle of failing to find one.
 *
 * So the primitives live here and each caller keeps its own policy. Nothing
 * here prints, exits, or decides anything a caller might want to decide
 * differently.
 *
 * macOS only.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef T150_BOOTSWITCH_H
#define T150_BOOTSWITCH_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <stddef.h>
#include <stdint.h>

/* The first match for a vendor and product id, or IO_OBJECT_NULL. */
io_service_t	t150_usb_find(long vid, long pid);

/* A device interface for a service, or NULL. The caller releases the service. */
IOUSBDeviceInterface500 **t150_usb_open(io_service_t svc);

/*
 * Both requests go to endpoint 0 with the device deliberately unopened: a
 * device request needs no interface, and opening one would be refused while
 * macOS's HID driver holds the device.
 */
IOReturn	t150_usb_model(IOUSBDeviceInterface500 **dev, uint8_t *buf,
		    size_t buflen, UInt32 *done);
IOReturn	t150_usb_switch(IOUSBDeviceInterface500 **dev, uint16_t value);

/*
 * The wheel leaves the bus the instant it accepts the switch, so the transfer
 * cannot complete. Which error that surfaces as depends on how far the
 * request had got, and none of them mean failure.
 */
int		t150_usb_left_the_bus(IOReturn r);

/*
 * The whole switch for a caller with no opinions, which is the daemon.
 * Returns 1 when a T150 was found at the boot id and the switch was sent, 0
 * when there was nothing there, and -1 when something was there and could not
 * be switched. Says nothing and waits for nothing: whether the wheel comes
 * back is the caller's next scan to discover.
 */
int		t150_boot_switch(long vid, long boot_pid, uint16_t value);

#endif /* T150_BOOTSWITCH_H */
