/*
 * Minimal, enough to syntax check the sources that name it. Nothing here
 * does anything. See the README beside this file.
 */
#ifndef T150_STUB_IOUSBLIB_H
#define T150_STUB_IOUSBLIB_H
#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>
#include <stdint.h>

/* The USB family's own returns, which probe_ioreturn_str names. */
#ifndef kIOUSBPipeStalled
#define kIOUSBPipeStalled		((IOReturn)0xe000404f)
#endif
#ifndef kIOUSBTransactionTimeout
#define kIOUSBTransactionTimeout	((IOReturn)0xe0004051)
#endif
#ifndef kIOUSBTransactionReturned
#define kIOUSBTransactionReturned	((IOReturn)0xe0004050)
#endif
#ifndef kIOUSBUnknownPipeErr
#define kIOUSBUnknownPipeErr		((IOReturn)0xe0004061)
#endif
#ifndef kIOUSBNoAsyncPortErr
#define kIOUSBNoAsyncPortErr		((IOReturn)0xe0004060)
#endif
typedef struct {
	uint8_t bmRequestType, bRequest;
	uint16_t wValue, wIndex, wLength;
	void *pData;
	uint32_t wLenDone;
} IOUSBDevRequest;

/* The same with the two timeouts, which is what the mode switch uses. */
typedef struct {
	uint8_t bmRequestType, bRequest;
	uint16_t wValue, wIndex, wLength;
	void *pData;
	uint32_t wLenDone;
	uint32_t noDataTimeout, completionTimeout;
} IOUSBDevRequestTO;

#define kIOUSBDeviceUserClientTypeID	((CFUUIDRef)0)
#define kIOUSBDeviceInterfaceID500	((CFUUIDRef)0)
#define kUSBReEnumerateReleaseDeviceMask 0x20000000
#define kUSBReEnumerateCaptureDeviceMask 0x40000000
#define kIOUSBFindInterfaceDontCare	0xffff
#define kUSBOut				0
#define kUSBIn				1
#define kUSBInterrupt			3
#define kIOUSBInterfaceUserClientTypeID	((CFUUIDRef)0)
#define kIOUSBInterfaceInterfaceID500	((CFUUIDRef)0)


typedef struct IOUSBInterfaceStruct500 {
	int (*Release)(void *);
	IOReturn (*USBInterfaceOpen)(void *);
	IOReturn (*USBInterfaceClose)(void *);
	IOReturn (*GetNumEndpoints)(void *, uint8_t *);
	IOReturn (*GetPipeProperties)(void *, uint8_t, uint8_t *, uint8_t *,
	    uint8_t *, uint16_t *, uint8_t *);
	IOReturn (*WritePipeTO)(void *, uint8_t, void *, uint32_t, uint32_t,
	    uint32_t);
	IOReturn (*ReadPipeAsyncTO)(void *, uint8_t, void *, uint32_t, uint32_t,
	    uint32_t, void *, void *);
	IOReturn (*ReadPipeAsync)(void *, uint8_t, void *, uint32_t, void *,
	    void *);
	IOReturn (*WritePipe)(void *, uint8_t, void *, uint32_t);
	IOReturn (*ClearPipeStall)(void *, uint8_t);
	IOReturn (*CreateInterfaceAsyncEventSource)(void *, CFRunLoopSourceRef *);
	CFRunLoopSourceRef (*GetInterfaceAsyncEventSource)(void *);
	IOReturn (*AbortPipe)(void *, uint8_t);
} IOUSBInterfaceInterface500;
typedef struct {
	uint16_t bInterfaceClass, bInterfaceSubClass;
	uint16_t bInterfaceProtocol, bAlternateSetting;
} IOUSBFindInterfaceRequest;

typedef struct IOUSBDeviceStruct500 {
	int (*Release)(void *);
	IOReturn (*DeviceRequest)(void *, IOUSBDevRequest *);
	IOReturn (*DeviceRequestTO)(void *, IOUSBDevRequestTO *);
	IOReturn (*USBDeviceReEnumerate)(void *, uint32_t);
	IOReturn (*CreateInterfaceIterator)(void *, IOUSBFindInterfaceRequest *,
	    io_iterator_t *);
	IOReturn (*SetConfiguration)(void *, uint8_t);
	IOReturn (*GetNumberOfConfigurations)(void *, uint8_t *);
	IOReturn (*USBDeviceOpen)(void *);
	IOReturn (*USBDeviceOpenSeize)(void *);
	IOReturn (*USBDeviceClose)(void *);
} IOUSBDeviceInterface500;

#endif /* T150_STUB_IOUSBLIB_H */
