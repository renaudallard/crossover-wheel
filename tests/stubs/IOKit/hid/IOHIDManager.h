#ifndef T150_STUB_IOHID_H
#define T150_STUB_IOHID_H
#include <CoreFoundation/CoreFoundation.h>
typedef int IOReturn;
typedef uint32_t IOOptionBits;
typedef struct IOHIDManager *IOHIDManagerRef;
typedef struct IOHIDDevice *IOHIDDeviceRef;
typedef int IOHIDReportType;
#define kIOReturnSuccess 0
#define kIOReturnNoDevice 0x2c0
#define kIOReturnNotOpen 0x2cd
#define kIOReturnOffline 0x2ed
#define kIOReturnNotAttached 0x2c9
#define kIOReturnNotResponding 0x2ef
#define kIOReturnExclusiveAccess 0x2c5
#define kIOHIDOptionsTypeNone 0
#define kIOHIDReportTypeOutput 1
#define kIOHIDVendorIDKey "VendorID"
#define kIOHIDProductIDKey "ProductID"
#define kIOHIDPrimaryUsagePageKey "PrimaryUsagePage"
#define kIOHIDPrimaryUsageKey "PrimaryUsage"
IOHIDManagerRef IOHIDManagerCreate(CFAllocatorRef, IOOptionBits);
IOReturn IOHIDManagerOpen(IOHIDManagerRef, IOOptionBits);
IOReturn IOHIDManagerClose(IOHIDManagerRef, IOOptionBits);
void IOHIDManagerSetDeviceMatching(IOHIDManagerRef, CFMutableDictionaryRef);
void IOHIDManagerScheduleWithRunLoop(IOHIDManagerRef, CFRunLoopRef, CFStringRef);
void IOHIDManagerUnscheduleFromRunLoop(IOHIDManagerRef, CFRunLoopRef, CFStringRef);
CFSetRef IOHIDManagerCopyDevices(IOHIDManagerRef);
IOReturn IOHIDDeviceOpen(IOHIDDeviceRef, IOOptionBits);
IOReturn IOHIDDeviceClose(IOHIDDeviceRef, IOOptionBits);
IOReturn IOHIDDeviceSetReport(IOHIDDeviceRef, IOHIDReportType, CFIndex, const uint8_t *, CFIndex);

#endif /* T150_STUB_IOHID_H */
