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
typedef struct CFRunLoopSource *CFRunLoopSourceRef;
typedef const struct CFArray *CFArrayRef;
typedef const struct CFUUID *CFUUIDRef;
typedef struct { unsigned char b[16]; } CFUUIDBytes;
typedef int32_t SInt32;
CFUUIDBytes CFUUIDGetUUIDBytes(CFUUIDRef);
typedef const struct CFDictionary *CFDictionaryRef;
typedef const struct CFData *CFDataRef;
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
#define kCFNumberLongType 10
#define kCFNumberSInt32Type 3
#define kCFRunLoopDefaultMode ((CFStringRef)0)
#define kCFRunLoopRunHandledSource 4
#define kCFRunLoopRunFinished 1
#define kCFRunLoopRunTimedOut 3
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
typedef unsigned long CFTypeID;
CFTypeID CFGetTypeID(CFTypeRef);
Boolean CFStringGetCString(CFStringRef, char *, CFIndex, uint32_t);
const void *CFDictionaryGetValue(CFDictionaryRef, const void *);
#define kCFStringEncodingUTF8 0x08000100
CFTypeID CFNumberGetTypeID(void);
CFTypeID CFStringGetTypeID(void);
CFTypeID CFDataGetTypeID(void);
CFTypeID CFArrayGetTypeID(void);
CFTypeID CFDictionaryGetTypeID(void);
CFIndex CFArrayGetCount(CFArrayRef);
const void *CFArrayGetValueAtIndex(CFArrayRef, CFIndex);
CFIndex CFDataGetLength(CFDataRef);
const uint8_t *CFDataGetBytePtr(CFDataRef);
CFRunLoopRef CFRunLoopGetCurrent(void);
void CFRunLoopAddSource(CFRunLoopRef, CFRunLoopSourceRef, CFStringRef);
void CFRunLoopStop(CFRunLoopRef);
void CFRunLoopRemoveSource(CFRunLoopRef, CFRunLoopSourceRef, CFStringRef);
CFRunLoopRunResult CFRunLoopRunInMode(CFStringRef, CFTimeInterval, Boolean);

#endif /* T150_STUB_COREFOUNDATION_H */
