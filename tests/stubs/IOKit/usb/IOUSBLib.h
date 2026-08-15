/*
 * Minimal, enough to syntax check the sources that name it. Nothing here
 * does anything. See the README beside this file.
 */
#ifndef T150_STUB_IOUSBLIB_H
#define T150_STUB_IOUSBLIB_H
#include <IOKit/IOKitLib.h>
#include <stdint.h>
typedef struct {
	uint8_t bmRequestType, bRequest;
	uint16_t wValue, wIndex, wLength;
	void *pData;
	uint32_t wLenDone;
} IOUSBDevRequest;
typedef struct IOUSBDeviceStruct500 {
	int (*Release)(void *);
	IOReturn (*DeviceRequest)(void *, IOUSBDevRequest *);
	IOReturn (*USBDeviceOpen)(void *);
	IOReturn (*USBDeviceClose)(void *);
} IOUSBDeviceInterface500;

#endif /* T150_STUB_IOUSBLIB_H */
