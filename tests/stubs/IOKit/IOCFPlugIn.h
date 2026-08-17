/*
 * Minimal, enough to syntax check the sources that name it. Nothing here does
 * anything. See the README beside this file.
 */
#ifndef T150_STUB_IOCFPLUGIN_H
#define T150_STUB_IOCFPLUGIN_H
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>

typedef void *LPVOID;

typedef struct IOCFPlugInInterfaceStruct {
	IOReturn (*QueryInterface)(void *, CFUUIDBytes, LPVOID *);
} IOCFPlugInInterface;

#define kIOCFPlugInInterfaceID ((CFUUIDRef)0)
#define S_OK 0

IOReturn IOCreatePlugInInterfaceForService(io_service_t, CFUUIDRef, CFUUIDRef,
    IOCFPlugInInterface ***, SInt32 *);
IOReturn IODestroyPlugInInterface(IOCFPlugInInterface **);

#endif /* T150_STUB_IOCFPLUGIN_H */
