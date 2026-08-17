/*
 * Minimal IOKit, enough to syntax check the sources that name it.
 * Nothing here does anything. See the README beside this file.
 */
#ifndef T150_STUB_IOKITLIB_H
#define T150_STUB_IOKITLIB_H
#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
typedef unsigned int io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_iterator_t;
typedef unsigned int mach_port_t;
typedef int kern_return_t;
typedef int IOReturn;
typedef uint32_t IOOptionBits;
#define IO_OBJECT_NULL ((io_object_t)0)
#define KERN_SUCCESS 0
#define kIOReturnSuccess 0
#define kIOMasterPortDefault ((mach_port_t)0)
#define kIOMainPortDefault ((mach_port_t)0)
/*
 * Field for field from IOKitLib.h, and it has to be: an async read hands the
 * completion a function pointer, and a stub that took void * there would
 * accept any function at all. The whole point of this directory is to catch a
 * signature that no longer matches the framework.
 */
typedef void (*IOAsyncCallback1)(void *refcon, IOReturn result, void *arg0);
kern_return_t IOObjectRelease(io_object_t);
io_service_t IOIteratorNext(io_iterator_t);
CFMutableDictionaryRef IOServiceMatching(const char *);
kern_return_t IOServiceGetMatchingServices(mach_port_t, CFMutableDictionaryRef,
    io_iterator_t *);
#endif /* T150_STUB_IOKITLIB_H */
