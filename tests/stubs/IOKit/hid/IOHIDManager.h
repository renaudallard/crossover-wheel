#ifndef T150_STUB_IOHID_H
#define T150_STUB_IOHID_H
#include <CoreFoundation/CoreFoundation.h>
typedef int IOReturn;
typedef uint32_t IOOptionBits;
typedef struct IOHIDManager *IOHIDManagerRef;
typedef struct IOHIDDevice *IOHIDDeviceRef;
typedef int IOHIDReportType;
#ifndef kIOReturnSuccess
#define kIOReturnSuccess 0
#endif
#ifndef kIOReturnNoDevice
#define kIOReturnNoDevice ((IOReturn)0x2c0)
#endif
#ifndef kIOReturnNotOpen
#define kIOReturnNotOpen ((IOReturn)0x2cd)
#endif
#ifndef kIOReturnOffline
#define kIOReturnOffline ((IOReturn)0x2ed)
#endif
#ifndef kIOReturnNotAttached
#define kIOReturnNotAttached ((IOReturn)0x2c9)
#endif
#ifndef kIOReturnNotResponding
#define kIOReturnNotResponding ((IOReturn)0x2ef)
#endif
#ifndef kIOReturnExclusiveAccess
#define kIOReturnExclusiveAccess ((IOReturn)0x2c5)
#endif
#define kIOHIDOptionsTypeNone 0
#define kIOHIDReportTypeOutput 1
#define kIOHIDVendorIDKey "VendorID"
#define kIOHIDProductKey "Product"
#define kIOHIDLocationIDKey "LocationID"
#define kIOHIDMaxFeatureReportSizeKey "kIOHIDMaxFeatureReportSizeKey"
#define kIOHIDMaxInputReportSizeKey "kIOHIDMaxInputReportSizeKey"
#define kIOHIDDeviceUsagePairsKey "DeviceUsagePairs"
#define kIOHIDDeviceUsagePageKey "DeviceUsagePage"
#define kIOHIDDeviceUsageKey "DeviceUsage"
#define kIOHIDManufacturerKey "Manufacturer"
#define kIOHIDSerialNumberKey "SerialNumber"
#define kIOHIDReportDescriptorKey "ReportDescriptor"
#define kIOHIDMaxOutputReportSizeKey "MaxOutputReportSize"
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
CFTypeRef IOHIDDeviceGetProperty(IOHIDDeviceRef, CFStringRef);
IOReturn IOHIDDeviceOpen(IOHIDDeviceRef, IOOptionBits);
IOReturn IOHIDDeviceClose(IOHIDDeviceRef, IOOptionBits);
IOReturn IOHIDDeviceSetReport(IOHIDDeviceRef, IOHIDReportType, CFIndex, const uint8_t *, CFIndex);

#endif /* T150_STUB_IOHID_H */
