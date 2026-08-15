/*
 * Minimal CoreFoundation, enough to syntax check the sources that name it.
 * Nothing here does anything. See the README beside this file.
 */
#ifndef T150_STUB_COREFOUNDATION_H
#define T150_STUB_COREFOUNDATION_H

#include <stddef.h>
#include <stdint.h>

typedef long CFIndex;
typedef unsigned char Boolean;
typedef const void *CFTypeRef;
typedef const struct CFString *CFStringRef;
typedef struct CFDict *CFMutableDictionaryRef;
typedef const struct CFNumber *CFNumberRef;
typedef const struct CFSet *CFSetRef;
typedef struct CFRunLoop *CFRunLoopRef;
typedef const void *CFAllocatorRef;
typedef int CFNumberType;
typedef int32_t CFRunLoopRunResult;
typedef double CFTimeInterval;

/* The Carbon integer names the IOKit headers still use. */
typedef uint32_t UInt32;
typedef uint16_t UInt16;
typedef uint8_t UInt8;
typedef int32_t SInt32;

#define TRUE 1
#define kCFAllocatorDefault ((CFAllocatorRef)0)
#define kCFNumberIntType 9
#define kCFNumberSInt32Type 3
#define kCFRunLoopDefaultMode ((CFStringRef)0)
#define kCFRunLoopRunHandledSource 4
#define CFSTR(s) ((CFStringRef)(s))

/*
 * The two callback structures a dictionary is created with. Only their
 * addresses are ever passed, so an empty shape of the right name is enough.
 */
struct cbk { int x; };
struct cbv { int x; };
#define kCFTypeDictionaryKeyCallBacks (*(const struct cbk *)0)
#define kCFTypeDictionaryValueCallBacks (*(const struct cbv *)0)

CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef, CFIndex,
    const struct cbk *, const struct cbv *);
void CFDictionarySetValue(CFMutableDictionaryRef, const void *, const void *);
CFNumberRef CFNumberCreate(CFAllocatorRef, CFNumberType, const void *);
Boolean CFNumberGetValue(CFNumberRef, CFNumberType, void *);
void CFRelease(CFTypeRef);
CFIndex CFSetGetCount(CFSetRef);
void CFSetGetValues(CFSetRef, const void **);
CFRunLoopRef CFRunLoopGetCurrent(void);
CFRunLoopRunResult CFRunLoopRunInMode(CFStringRef, CFTimeInterval, Boolean);

#endif /* T150_STUB_COREFOUNDATION_H */
