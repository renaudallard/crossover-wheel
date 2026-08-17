/*
 * Minimal, enough to syntax check the sources that name it. Nothing here
 * does anything, and none of these values is the real one: what is being
 * checked is that the sources parse and are warning clean, never behaviour.
 * See the README beside this file.
 *
 * Each is guarded, because the HID stub beside this one already names some of
 * them and a redefinition is a warning, which strict turns into an error.
 */
#ifndef T150_STUB_IORETURN_H
#define T150_STUB_IORETURN_H
#include <IOKit/IOKitLib.h>

#ifndef kIOReturnError
#define kIOReturnError	((IOReturn)0x2bc)
#endif
#ifndef kIOReturnNoMemory
#define kIOReturnNoMemory	((IOReturn)0x2bd)
#endif
#ifndef kIOReturnNoResources
#define kIOReturnNoResources	((IOReturn)0x2be)
#endif
#ifndef kIOReturnBadArgument
#define kIOReturnBadArgument	((IOReturn)0x2c2)
#endif
#ifndef kIOReturnExclusiveAccess
#define kIOReturnExclusiveAccess	((IOReturn)0x2c5)
#endif
#ifndef kIOReturnUnsupported
#define kIOReturnUnsupported	((IOReturn)0x2c7)
#endif
#ifndef kIOReturnNoDevice
#define kIOReturnNoDevice	((IOReturn)0x2c8)
#endif
#ifndef kIOReturnNotPrivileged
#define kIOReturnNotPrivileged	((IOReturn)0x2cb)
#endif
#ifndef kIOReturnNotPermitted
#define kIOReturnNotPermitted	((IOReturn)0x2ce)
#endif
#ifndef kIOReturnNotOpen
#define kIOReturnNotOpen	((IOReturn)0x2cf)
#endif
#ifndef kIOReturnNotResponding
#define kIOReturnNotResponding	((IOReturn)0x2d1)
#endif
#ifndef kIOReturnTimeout
#define kIOReturnTimeout	((IOReturn)0x2d6)
#endif
#ifndef kIOReturnAborted
#define kIOReturnAborted	((IOReturn)0x2d7)
#endif
#ifndef kIOReturnUnderrun
#define kIOReturnUnderrun	((IOReturn)0x2d8)
#endif
#ifndef kIOReturnOverrun
#define kIOReturnOverrun	((IOReturn)0x2d9)
#endif
#ifndef kIOReturnBusy
#define kIOReturnBusy	((IOReturn)0x2d0)
#endif
#ifndef kIOReturnOffline
#define kIOReturnOffline	((IOReturn)0x2da)
#endif
#ifndef kIOReturnNotAttached
#define kIOReturnNotAttached	((IOReturn)0x2c9)
#endif

#endif /* T150_STUB_IORETURN_H */
