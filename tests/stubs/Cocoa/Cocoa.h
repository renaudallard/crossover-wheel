/*
 * Minimal Cocoa so src/mac/t150menu.m can be syntax checked off the Mac.
 * Only the types, methods and constants that file actually names. Nothing
 * here does anything: it exists so clang can parse and warn.
 */
#ifndef T150_STUB_COCOA_H
#define T150_STUB_COCOA_H

#include <stddef.h>
#include <stdint.h>

typedef long NSInteger;
typedef unsigned long NSUInteger;
typedef double CGFloat;
typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#ifndef nil
#define nil ((id)0)
#endif

typedef struct { CGFloat x, y; } NSPoint;
typedef struct { CGFloat width, height; } NSSize;
typedef struct { NSPoint origin; NSSize size; } NSRect;
typedef struct { NSUInteger location, length; } NSRange;

static inline NSRect NSMakeRect(CGFloat a, CGFloat b, CGFloat c, CGFloat d)
{ NSRect r; r.origin.x = a; r.origin.y = b; r.size.width = c;
  r.size.height = d; return r; }
static inline NSSize NSMakeSize(CGFloat w, CGFloat h)
{ NSSize s; s.width = w; s.height = h; return s; }
static inline NSRange NSMakeRange(NSUInteger l, NSUInteger n)
{ NSRange r; r.location = l; r.length = n; return r; }

#define NSNotFound ((NSUInteger)-1)

typedef NSUInteger NSStringEncoding;
#define NSUTF8StringEncoding ((NSStringEncoding)4)
#define NSASCIIStringEncoding ((NSStringEncoding)1)

/*
 * What makes clang check a format string against its arguments. Real
 * Foundation puts NS_FORMAT_FUNCTION on every one of these; without it the
 * whole -Wformat group is silently off for every +stringWithFormat: in the
 * tree, and a mistake that is -Werror on a Mac passes here. That is the exact
 * failure this stub exists to prevent, so the attribute is not optional.
 */
#define T150_NS_FORMAT(f, a) __attribute__((format(__NSString__, f, a)))

typedef NSUInteger NSDataSearchOptions;
typedef NSUInteger NSJSONReadingOptions;

@class NSString, NSArray, NSDictionary, NSData, NSError, NSURL;
@class NSMenu, NSMenuItem, NSView, NSFont, NSColor, NSImage, NSTimer;
@class NSNotificationCenter, NSStatusBarButton;

typedef NSInteger NSComparisonResult;

@protocol NSObject
@end

@interface NSObject <NSObject>
+ (instancetype)alloc;
- (instancetype)init;
- (BOOL)isEqual:(id)other;
- (BOOL)respondsToSelector:(SEL)s;
- (BOOL)isKindOfClass:(Class)c;
+ (Class)class;
@end

@protocol NSApplicationDelegate <NSObject> @end
@protocol NSWindowDelegate <NSObject> @end
@protocol NSMenuDelegate <NSObject> @end

@interface NSString : NSObject
@property (readonly) NSUInteger length;
@property (readonly) const char *fileSystemRepresentation;
@property (readonly) NSInteger integerValue;
+ (instancetype)stringWithFormat:(NSString *)fmt, ... T150_NS_FORMAT(1, 2);
+ (instancetype)stringWithUTF8String:(const char *)s;
- (instancetype)initWithData:(NSData *)d encoding:(NSStringEncoding)e;
- (instancetype)initWithBytes:(const void *)b length:(NSUInteger)n
    encoding:(NSStringEncoding)e;
- (instancetype)initWithFormat:(NSString *)fmt, ... T150_NS_FORMAT(1, 2);
- (BOOL)isEqualToString:(NSString *)s;
- (BOOL)hasPrefix:(NSString *)s;
- (BOOL)hasSuffix:(NSString *)s;
- (BOOL)containsString:(NSString *)s;
- (NSString *)substringFromIndex:(NSUInteger)i;
- (NSString *)stringByAppendingString:(NSString *)s;
- (NSString *)stringByAppendingPathComponent:(NSString *)s;
- (NSString *)stringByDeletingLastPathComponent;
- (NSString *)stringByStandardizingPath;
- (NSString *)lastPathComponent;
- (NSData *)dataUsingEncoding:(NSStringEncoding)e;
- (NSComparisonResult)localizedCaseInsensitiveCompare:(NSString *)s;
- (NSArray *)componentsSeparatedByString:(NSString *)s;
- (NSString *)lowercaseString;
@end

@interface NSMutableString : NSString
+ (instancetype)string;
- (void)appendString:(NSString *)s;
- (void)setString:(NSString *)s;
- (void)deleteCharactersInRange:(NSRange)r;
- (void)appendFormat:(NSString *)fmt, ... T150_NS_FORMAT(1, 2);
@end

typedef struct {
	unsigned long state;
	__unsafe_unretained id *itemsPtr;
	unsigned long *mutationsPtr;
	unsigned long extra[5];
} NSFastEnumerationState;

@protocol NSFastEnumeration
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)st
    objects:(__unsafe_unretained id *)buf count:(NSUInteger)n;
@end

@interface NSArray<__covariant T> : NSObject <NSFastEnumeration>
@property (readonly) NSUInteger count;
@property (readonly) T firstObject;
+ (instancetype)array;
+ (instancetype)arrayWithObjects:(const T *)objs count:(NSUInteger)n;
- (T)objectAtIndexedSubscript:(NSUInteger)i;
- (T)objectAtIndex:(NSUInteger)i;
- (NSString *)componentsJoinedByString:(NSString *)s;
- (NSArray *)sortedArrayUsingSelector:(SEL)s;
- (BOOL)containsObject:(id)o;
- (id)mutableCopy;
- (BOOL)isEqualToArray:(NSArray *)a;
@end

@interface NSMutableArray<T> : NSArray<T>
+ (instancetype)array;
- (void)addObject:(T)o;
- (void)addObjectsFromArray:(NSArray *)a;
- (void)removeAllObjects;
@end

@interface NSNumber : NSObject
+ (NSNumber *)numberWithBool:(BOOL)v;
+ (NSNumber *)numberWithInteger:(NSInteger)v;
+ (NSNumber *)numberWithInt:(int)v;
+ (NSNumber *)numberWithDouble:(double)v;
@end

@interface NSDictionary : NSObject
+ (instancetype)dictionaryWithContentsOfFile:(NSString *)p;
+ (instancetype)dictionaryWithObjects:(const id *)o forKeys:(const id *)k
    count:(NSUInteger)n;
- (id)objectForKeyedSubscript:(id)k;
- (id)objectForKey:(id)k;
- (BOOL)writeToFile:(NSString *)p atomically:(BOOL)a;
@end

@interface NSData : NSObject
@property (readonly) NSUInteger length;
@property (readonly) const void *bytes;
+ (instancetype)dataWithContentsOfFile:(NSString *)p;
+ (instancetype)dataWithContentsOfURL:(NSURL *)u;
- (BOOL)isEqualToData:(NSData *)d;
- (NSRange)rangeOfData:(NSData *)d options:(NSDataSearchOptions)o
    range:(NSRange)r;
- (BOOL)writeToFile:(NSString *)p atomically:(BOOL)a;
@end

@interface NSError : NSObject
@property (readonly) NSString *localizedDescription;
@end

@interface NSURL : NSObject
+ (instancetype)fileURLWithPath:(NSString *)p;
+ (instancetype)URLWithString:(NSString *)s;
@end

@interface NSBundle : NSObject
+ (NSBundle *)mainBundle;
+ (NSBundle *)bundleWithPath:(NSString *)p;
@property (readonly) NSString *resourcePath;
@property (readonly) NSString *bundlePath;
@property (readonly) NSString *bundleIdentifier;
- (id)objectForInfoDictionaryKey:(NSString *)k;
@end

@interface NSFileManager : NSObject
+ (NSFileManager *)defaultManager;
- (BOOL)fileExistsAtPath:(NSString *)p;
- (BOOL)removeItemAtPath:(NSString *)p error:(NSError **)e;
- (BOOL)copyItemAtPath:(NSString *)a toPath:(NSString *)b error:(NSError **)e;
- (BOOL)moveItemAtPath:(NSString *)a toPath:(NSString *)b error:(NSError **)e;
- (BOOL)createDirectoryAtPath:(NSString *)p
    withIntermediateDirectories:(BOOL)i attributes:(NSDictionary *)a
    error:(NSError **)e;
- (NSArray<NSString *> *)contentsOfDirectoryAtPath:(NSString *)p
    error:(NSError **)e;
@end

@interface NSFileHandle : NSObject
@property (readonly) NSData *availableData;
+ (instancetype)fileHandleForReadingAtPath:(NSString *)p;
+ (instancetype)fileHandleWithNullDevice;
- (void)setReadabilityHandler:(void (^)(NSFileHandle *))h;
- (void (^)(NSFileHandle *))readabilityHandler;
- (NSData *)readDataToEndOfFile;
- (void)closeFile;
@end

@interface NSPipe : NSObject
+ (instancetype)pipe;
@property (readonly) NSFileHandle *fileHandleForReading;
@end

@interface NSTask : NSObject
@property (copy) NSURL *executableURL;
@property (copy) NSArray *arguments;
@property (copy) NSURL *currentDirectoryURL;
@property (strong) id standardOutput;
@property (strong) id standardError;
@property (readonly) BOOL isRunning;
@property (readonly) int terminationStatus;
@property (copy) void (^terminationHandler)(NSTask *);
- (BOOL)launchAndReturnError:(NSError **)e;
- (void)terminate;
- (void)waitUntilExit;
@end

@interface NSTimer : NSObject
+ (NSTimer *)scheduledTimerWithTimeInterval:(double)i repeats:(BOOL)r
    block:(void (^)(NSTimer *))b;
- (void)invalidate;
@end

@interface NSUserDefaults : NSObject
+ (NSUserDefaults *)standardUserDefaults;
- (BOOL)boolForKey:(NSString *)k;
- (NSInteger)integerForKey:(NSString *)k;
- (void)setBool:(BOOL)v forKey:(NSString *)k;
- (void)setInteger:(NSInteger)v forKey:(NSString *)k;
@end

@interface NSNotification : NSObject
@end

@interface NSOperationQueue : NSObject
+ (NSOperationQueue *)mainQueue;
@end

@interface NSNotificationCenter : NSObject
- (void)addObserverForName:(NSString *)n object:(id)o
    queue:(NSOperationQueue *)q usingBlock:(void (^)(NSNotification *))b;
@end

@interface NSJSONSerialization : NSObject
+ (id)JSONObjectWithData:(NSData *)d options:(NSJSONReadingOptions)o
    error:(NSError **)e;
@end

@interface NSURLRequest : NSObject
+ (instancetype)requestWithURL:(NSURL *)u cachePolicy:(NSUInteger)p
    timeoutInterval:(double)t;
@end
#define NSURLRequestReloadIgnoringLocalCacheData ((NSUInteger)1)

@interface NSURLResponse : NSObject
@end

@interface NSURLSessionTask : NSObject
- (void)resume;
@end

@interface NSURLSession : NSObject
+ (NSURLSession *)sharedSession;
- (NSURLSessionTask *)dataTaskWithRequest:(NSURLRequest *)r
    completionHandler:(void (^)(NSData *, NSURLResponse *, NSError *))h;
- (NSURLSessionTask *)dataTaskWithURL:(NSURL *)u
    completionHandler:(void (^)(NSData *, NSURLResponse *, NSError *))h;
@end

/* AppKit */

@interface NSColor : NSObject
+ (NSColor *)secondaryLabelColor;
+ (NSColor *)labelColor;
+ (NSColor *)clearColor;
+ (NSColor *)textBackgroundColor;
@end

@interface NSFont : NSObject
+ (NSFont *)systemFontOfSize:(CGFloat)s weight:(CGFloat)w;
+ (NSFont *)systemFontOfSize:(CGFloat)s;
+ (CGFloat)systemFontSize;
+ (CGFloat)smallSystemFontSize;
+ (NSFont *)monospacedSystemFontOfSize:(CGFloat)s weight:(CGFloat)w;
@end
extern const CGFloat NSFontWeightRegular;
extern const CGFloat NSFontWeightSemibold;
extern NSString * const NSFontAttributeName;
extern NSString * const NSForegroundColorAttributeName;

@interface NSAttributedString : NSObject
- (instancetype)initWithString:(NSString *)s attributes:(NSDictionary *)a;
@end

@interface NSImage : NSObject
+ (NSImage *)imageNamed:(NSString *)n;
- (void)setTemplate:(BOOL)t;
- (void)setSize:(NSSize)s;
@end

@interface CALayer : NSObject
@property CGFloat cornerRadius;
@property BOOL masksToBounds;
@end

@interface NSView : NSObject
- (instancetype)initWithFrame:(NSRect)f;
@property (readonly) NSRect bounds;
@property NSRect frame;
@property BOOL wantsLayer;
@property (strong) CALayer *layer;
@property NSUInteger autoresizingMask;
- (void)addSubview:(NSView *)v;
@end
#define NSViewWidthSizable ((NSUInteger)2)
#define NSViewHeightSizable ((NSUInteger)16)

@interface NSVisualEffectView : NSView
@property NSInteger material;
@property NSInteger blendingMode;
@property NSInteger state;
@end
#define NSVisualEffectMaterialWindowBackground ((NSInteger)12)
#define NSVisualEffectBlendingModeBehindWindow ((NSInteger)0)
#define NSVisualEffectStateFollowsWindowActiveState ((NSInteger)0)

@interface NSTextField : NSView
+ (instancetype)labelWithString:(NSString *)s;
+ (instancetype)labelWithAttributedString:(NSAttributedString *)s;
@property (strong) NSFont *font;
@property (strong) NSColor *textColor;
@end

/* Really an NSMutableAttributedString, which is where the mutators come from. */
@interface NSTextStorage : NSObject
- (void)appendAttributedString:(NSAttributedString *)s;
- (void)deleteCharactersInRange:(NSRange)r;
@property (readonly) NSUInteger length;
@end

@interface NSTextView : NSView
@property (readonly) NSTextStorage *textStorage;
@property BOOL drawsBackground;
@property (strong) NSColor *textColor;
@property NSSize textContainerInset;
@property BOOL editable;
@property (strong) NSFont *font;
@property (strong) NSColor *backgroundColor;
- (void)setString:(NSString *)s;
- (NSString *)string;
- (void)scrollRangeToVisible:(NSRange)r;
@end

@interface NSClipView : NSView
@property (strong) NSColor *backgroundColor;
@property BOOL drawsBackground;
@end

@interface NSScrollView : NSView
@property (strong) NSView *documentView;
@property BOOL hasVerticalScroller;
@property NSUInteger borderType;
@property BOOL drawsBackground;
@property (strong) NSColor *backgroundColor;
@property (readonly) NSClipView *contentView;
@end
#define NSNoBorder ((NSUInteger)0)

@interface NSButton : NSView
+ (instancetype)buttonWithTitle:(NSString *)t target:(id)o action:(SEL)a;
@property NSInteger bezelStyle;
@property BOOL enabled;
@property (copy) NSString *title;
@property (copy) NSString *keyEquivalent;
@end
#define NSBezelStyleRounded ((NSInteger)1)

@interface NSPopUpButton : NSView
- (instancetype)initWithFrame:(NSRect)f pullsDown:(BOOL)p;
- (void)addItemsWithTitles:(NSArray<NSString *> *)t;
- (void)removeAllItems;
- (void)selectItemWithTitle:(NSString *)t;
- (NSInteger)indexOfItemWithTitle:(NSString *)t;
@property (readonly) NSString *titleOfSelectedItem;
@property (readonly) NSInteger numberOfItems;
@end

@interface NSMenuItem : NSObject
- (instancetype)initWithTitle:(NSString *)t action:(SEL)a
    keyEquivalent:(NSString *)k;
+ (NSMenuItem *)separatorItem;
@property (copy) NSString *title;
@property (weak) id target;
@property SEL action;
@property BOOL enabled;
@property BOOL hidden;
@property NSInteger state;
@property (strong) NSMenu *submenu;
@property (readonly) NSMenu *menu;
@property NSInteger tag;
@end
#define NSControlStateValueOn ((NSInteger)1)
#define NSControlStateValueOff ((NSInteger)0)

@interface NSMenu : NSObject
- (instancetype)initWithTitle:(NSString *)t;
@property BOOL autoenablesItems;
@property (weak) id delegate;
- (void)addItem:(NSMenuItem *)i;
@property (readonly) NSArray<NSMenuItem *> *itemArray;
@end

@interface NSStatusBarButton : NSObject
@property (strong) NSImage *image;
@property (copy) NSString *title;
@end

@interface NSStatusItem : NSObject
@property (strong) NSMenu *menu;
@property (readonly) NSStatusBarButton *button;
@end

@interface NSStatusBar : NSObject
+ (NSStatusBar *)systemStatusBar;
- (NSStatusItem *)statusItemWithLength:(CGFloat)l;
@end
#define NSVariableStatusItemLength ((CGFloat)-1)

@interface NSWindow : NSObject
- (instancetype)initWithContentRect:(NSRect)r styleMask:(NSUInteger)m
    backing:(NSUInteger)b defer:(BOOL)d;
@property (copy) NSString *title;
@property (strong) NSView *contentView;
@property (weak) id delegate;
@property BOOL releasedWhenClosed;
@property BOOL titlebarAppearsTransparent;
@property NSSize minSize;
- (void)makeKeyAndOrderFront:(id)s;
- (void)center;
@end
#define NSWindowStyleMaskTitled ((NSUInteger)1)
#define NSWindowStyleMaskClosable ((NSUInteger)2)
#define NSWindowStyleMaskResizable ((NSUInteger)8)
#define NSBackingStoreBuffered ((NSUInteger)2)

@interface NSAlert : NSObject
@property (copy) NSString *messageText;
@property (copy) NSString *informativeText;
- (void)addButtonWithTitle:(NSString *)t;
- (NSInteger)runModal;
@end
#define NSAlertFirstButtonReturn ((NSInteger)1000)

@interface NSPasteboard : NSObject
+ (NSPasteboard *)generalPasteboard;
- (void)clearContents;
- (BOOL)setString:(NSString *)s forType:(NSString *)t;
@end
extern NSString * const NSPasteboardTypeString;

@interface NSApplication : NSObject
+ (NSApplication *)sharedApplication;
@property (weak) id delegate;
- (void)setActivationPolicy:(NSInteger)p;
- (void)activateIgnoringOtherApps:(BOOL)f;
- (void)terminate:(id)s;
- (void)run;
@end
extern NSApplication *NSApp;
#define NSApplicationActivationPolicyAccessory ((NSInteger)1)

@interface NSWorkspace : NSObject
+ (NSWorkspace *)sharedWorkspace;
@property (readonly) NSNotificationCenter *notificationCenter;
- (BOOL)openURL:(NSURL *)u;
@end
extern NSString * const NSWorkspaceDidWakeNotification;

NSString *NSHomeDirectory(void);
NSString *NSTemporaryDirectory(void);

/* dispatch */
typedef void *dispatch_queue_t;
dispatch_queue_t dispatch_get_main_queue(void);
void dispatch_async(dispatch_queue_t q, void (^b)(void));

#endif /* T150_STUB_COCOA_H */
