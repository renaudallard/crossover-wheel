/*
 * t150menu.m - the menu bar item and the graphical installer.
 *
 * This is deliberately thin. Every decision that can go wrong, which file is
 * CrossOver's builtin and which is the placeholder beside it, what to verify
 * after copying, what to keep a copy of, lives in install.sh, and install.sh
 * is checked against a synthetic CrossOver tree on every build machine. This
 * file lists bottles, runs that script, and shows what it said. Reimplementing
 * the install here would move the dangerous part somewhere nothing tests.
 *
 * The same rule applies to the daemon. This does not speak the daemon's
 * protocol to ask how it is doing: t150d serves one client at a time and a
 * second connection can displace the first, so a status display that
 * connected would throw a game off the wheel mid race. It owns the daemon as
 * a child process instead and reads its -v output, which says more than the
 * protocol would have.
 *
 * The app bundle carries install.sh, the binaries, the man pages and the
 * proxy DLL in Resources, so it is self contained: install.sh resolves
 * everything relative to itself and needs no argument to find them.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Cocoa/Cocoa.h>
#import <CommonCrypto/CommonDigest.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "mac/bootswitch.h"
#include "t150/t150.h"

#define AGENT_LABEL	@"it.allard.t150d"

@interface T150Menu : NSObject <NSApplicationDelegate, NSWindowDelegate,
    NSMenuDelegate>
@property (strong) NSStatusItem *item;
@property (strong) NSTask *daemon;
@property (strong) NSWindow *setup;
@property (strong) NSPopUpButton *bottles;
@property (strong) NSButton *install;
@property (strong) NSTextView *out;
@property (strong) NSMutableString *logBuf;
@property (strong) NSMenuItem *statusLine;
@property (strong) NSMenuItem *runItem;
@property (strong) NSMenuItem *loginItem;
@property (strong) NSTimer *watch;
@property (assign) BOOL clientConnected;
@property (assign) BOOL wheelSeen;
@end

@implementation T150Menu

/* Everything the bundle carries lives beside install.sh. */
- (NSString *)resource:(NSString *)name
{
	return [[[NSBundle mainBundle] resourcePath]
	    stringByAppendingPathComponent:name];
}

- (NSString *)bottleRoot
{
	return [NSHomeDirectory() stringByAppendingPathComponent:
	    @"Library/Application Support/CrossOver/Bottles"];
}

/*
 * The daemon this runs, which is always the one inside this bundle.
 *
 * Not a copy installed somewhere on the PATH, deliberately. This application
 * ships the daemon it was built against, so running that one means the two
 * always match and there is no version of this that depends on a separate
 * install having happened. It is also why the menu never installs command
 * line tools: nothing here needs them.
 */
- (NSString *)daemonPath
{
	return [self resource:@"t150d"];
}

/* Where t150d publishes its port, and takes its single instance lock. */
- (NSString *)endpointPath
{
	return [NSHomeDirectory() stringByAppendingPathComponent:
	    @"Library/Application Support/t150ffb/endpoint"];
}

/*
 * Whether a daemon that is not our child is already running.
 *
 * "Start at login" and this menu are two different ways to get one, and until
 * the daemon took a lock they could not see each other: the menu said the
 * daemon was stopped while one was driving a game, offered to start it, and
 * starting it took the wheel away from that game. t150d holds a lock on the
 * endpoint for its whole life, so asking who holds it is the only answer that
 * cannot race, and F_GETLK asks without taking it.
 */
- (BOOL)daemonElsewhere
{
	NSString *lock = [[self endpointPath] stringByAppendingString:@".lock"];
	struct flock fl;
	int fd;
	BOOL held = NO;

	if (self.daemon != nil && self.daemon.isRunning)
		return NO;

	if ((fd = open(lock.fileSystemRepresentation, O_RDONLY)) == -1)
		return NO;

	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	if (fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type != F_UNLCK)
		held = YES;
	(void)close(fd);

	return held;
}

/*
 * A bottle is a directory with a cxbottle.conf in it, which is the same test
 * install.sh makes. Anything else in that folder is not a bottle.
 */
- (NSArray<NSString *> *)findBottles
{
	NSFileManager *fm = [NSFileManager defaultManager];
	NSMutableArray *found = [NSMutableArray array];
	NSString *root = [self bottleRoot];

	for (NSString *name in [[fm contentsOfDirectoryAtPath:root error:NULL]
	    sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)]) {
		NSString *conf = [[root stringByAppendingPathComponent:name]
		    stringByAppendingPathComponent:@"cxbottle.conf"];

		if ([fm fileExistsAtPath:conf])
			[found addObject:name];
	}

	return found;
}

#pragma mark - the menu

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
	(void)n;

	/* Accessory: a menu bar item with no Dock icon and no main window. */
	[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

	self.item = [[NSStatusBar systemStatusBar]
	    statusItemWithLength:NSVariableStatusItemLength];

	NSMenu *m = [[NSMenu alloc] init];

	/*
	 * Enable items ourselves. Cocoa's automatic enabling asks the target
	 * whether it responds to the action, which is always yes here, so it
	 * would override the one item whose enabled state carries meaning:
	 * the daemon row, which is greyed when the daemon running is not ours
	 * to stop.
	 */
	m.autoenablesItems = NO;

	self.statusLine = [[NSMenuItem alloc] initWithTitle:@"" action:NULL
	    keyEquivalent:@""];
	self.statusLine.enabled = NO;
	[m addItem:self.statusLine];
	[m addItem:[NSMenuItem separatorItem]];

	self.runItem = [[NSMenuItem alloc] initWithTitle:@"Start the daemon"
	    action:@selector(toggleDaemon:) keyEquivalent:@""];
	self.runItem.target = self;
	[m addItem:self.runItem];

	self.loginItem = [[NSMenuItem alloc] initWithTitle:@"Start at login"
	    action:@selector(toggleLogin:) keyEquivalent:@""];
	self.loginItem.target = self;
	[m addItem:self.loginItem];

	[m addItem:[NSMenuItem separatorItem]];

	/*
	 * The two things the wheel itself keeps, which no game can ask for.
	 * DirectInput has no property for either, so on Windows they live in
	 * Thrustmaster's control panel and here they live in this menu.
	 */
	NSMenuItem *rot = [[NSMenuItem alloc] initWithTitle:@"Rotation"
	    action:NULL keyEquivalent:@""];
	rot.submenu = [self buildRotationMenu];
	[m addItem:rot];

	NSMenuItem *ac = [[NSMenuItem alloc] initWithTitle:@"Centring spring"
	    action:NULL keyEquivalent:@""];
	ac.submenu = [self buildAutocentreMenu];
	[m addItem:ac];

	[m addItem:[NSMenuItem separatorItem]];

	NSMenuItem *s = [[NSMenuItem alloc] initWithTitle:
	    @"Install into a bottle…"
	    action:@selector(openSetup:) keyEquivalent:@""];
	s.target = self;
	[m addItem:s];

	NSMenuItem *lg = [[NSMenuItem alloc] initWithTitle:@"Copy the log"
	    action:@selector(copyLog:) keyEquivalent:@""];
	lg.target = self;
	[m addItem:lg];

	NSMenuItem *u = [[NSMenuItem alloc] initWithTitle:@"Check for updates…"
	    action:@selector(checkForUpdates:) keyEquivalent:@""];
	u.target = self;
	[m addItem:u];

	NSMenuItem *q = [[NSMenuItem alloc] initWithTitle:@"Quit"
	    action:@selector(quit:) keyEquivalent:@"q"];
	q.target = self;
	[m addItem:q];

	m.delegate = self;
	self.item.menu = m;
	[self leaveBootMode];
	self.wheelSeen = [self wheelPresent];
	[self refresh];

	/*
	 * Two seconds is slower than the daemon's own scan and fast enough
	 * that a wheel is out of boot mode long before somebody has launched
	 * a game. The block holds self weakly, because the timer holds the
	 * block and the run loop holds the timer.
	 */
	__weak __typeof__(self) weak = self;

	self.watch = [NSTimer scheduledTimerWithTimeInterval:2.0 repeats:YES
	    block:^(NSTimer *t) {
		(void)t;
		[weak watchWheel];
	}];

	/*
	 * Nothing installed yet means the person just double clicked this to
	 * install, so show them the window rather than a menu bar icon they
	 * have to find.
	 *
	 * Deliberately not conditional on having found a bottle. It was, and
	 * that was the worst possible way round: a machine where the bottles
	 * could not be listed showed no window at all, so an installer that
	 * had just reported success was followed by nothing happening and no
	 * way to find out why. Not finding a bottle is exactly when somebody
	 * needs to be told something.
	 */
	if (![[NSUserDefaults standardUserDefaults] boolForKey:@"installedOnce"])
		[self openSetup:nil];

	/*
	 * Every launch, and quietly: it says nothing at all unless there is
	 * something newer than this.
	 */
	[self lookForUpdate:NO];
}

/*
 * Three states, one silhouette. The glyphs are template images, which is what
 * lets macOS tint them for a light or a dark menu bar without two sets: the
 * name ending in Template is the convention that asks for that, and setting
 * it explicitly costs nothing if the name ever changes.
 *
 * Falls back to text if the bundle has no icons, so a build without them is
 * still usable rather than invisible.
 */
- (void)showGlyph:(BOOL)running
{
	NSString *name;

	if (!running || !self.wheelSeen)
		name = @"t150-idleTemplate";
	else if (self.clientConnected)
		name = @"t150-activeTemplate";
	else
		name = @"t150-connectedTemplate";

	NSImage *img = [NSImage imageNamed:name];

	if (img == nil) {
		self.item.button.image = nil;
		self.item.button.title = running ? @"◉ T150" : @"○ T150";
		return;
	}

	img.template = YES;
	self.item.button.title = @"";
	self.item.button.image = img;
}

- (void)refresh
{
	BOOL mine = self.daemon != nil && self.daemon.isRunning;
	BOOL elsewhere = !mine && [self daemonElsewhere];
	BOOL running = mine || elsewhere;
	NSString *wheel = self.wheelSeen ? @"wheel connected"
	    : @"no wheel found";

	[self showGlyph:running];

	if (elsewhere)
		self.statusLine.title = [NSString stringWithFormat:
		    @"%@ · daemon running, started elsewhere", wheel];
	else if (running && self.clientConnected)
		self.statusLine.title = [NSString stringWithFormat:
		    @"%@ · daemon running · a game is connected",
		    wheel];
	else if (running)
		self.statusLine.title = [NSString stringWithFormat:
		    @"%@ · daemon running", wheel];
	else
		self.statusLine.title = [NSString stringWithFormat:
		    @"%@ · daemon stopped", wheel];

	/*
	 * A daemon somebody else started is not ours to stop, and offering to
	 * start a second one is offering to take the wheel from whatever the
	 * first is driving. Say so rather than pretending either is possible.
	 */
	self.runItem.enabled = !elsewhere;
	self.runItem.title = elsewhere ? @"The daemon is already running"
	    : (running ? @"Stop the daemon" : @"Start the daemon");
	self.loginItem.state = [self loginEnabled] ? NSControlStateValueOn
	    : NSControlStateValueOff;
}

/*
 * Sampled again here as well as on the watch timer, so a menu that is opened
 * between two ticks states what is true now. refresh itself runs on every
 * line the daemon prints, and asking IOKit once per log line would be a
 * registry walk per line for a status nobody is looking at.
 */
- (void)menuWillOpen:(NSMenu *)menu
{
	(void)menu;
	self.wheelSeen = [self wheelPresent];
	[self refresh];
}

/*
 * Take the wheel out of boot mode, if that is where it is.
 *
 * Every T-series wheel shares the boot identity 044f:b65d, whose product
 * string is "Thrustmaster FFB Wheel" and names no model, and only the mode
 * switch reveals the T150's own b677 (RESEARCH.md, the enumeration table). A
 * plug-in, a sleep and a wake all put it back there.
 *
 * That matters to a game and not only to this daemon. DirectInput builds a
 * device's identity out of the vendor and product ids, so a wheel enumerated
 * at b65d is a different device from the same wheel at b677: it comes up as
 * "Thrustmaster FFB Wheel" instead of the T150, and every button a person
 * mapped against the other one is against a device the game no longer sees.
 * A game started in the window before the switch therefore costs its owner
 * the whole controller configuration.
 *
 * The daemon does this too, on its own scan, but only while it is running.
 * This is the part that is always running, so it is the one that can close
 * the window. It is cheap: with the wheel in firmware mode there is nothing
 * at the boot id and this is one registry lookup. It refuses to switch a
 * T-series wheel that is not a T150, because the value means something else
 * entirely to a T300RS or a TMX.
 */
- (void)leaveBootMode
{
	uint8_t model = 0;

	(void)t150_boot_switch(T150_VID, T150_PID_BOOT, T150_SWITCH_VALUE,
	    &model);
}

/*
 * Watch the bus rather than waiting to be asked.
 *
 * The wheel used to be looked for only at launch and when the menu was about
 * to open, so the icon stated something that had been true whenever it was
 * last sampled: a wheel unplugged mid session still read as connected until
 * somebody clicked. Worse, a wheel sitting in boot mode stayed there until
 * the daemon was started, which is the state a game must not enumerate it in.
 */
- (void)watchWheel
{
	BOOL was = self.wheelSeen;

	[self leaveBootMode];
	self.wheelSeen = [self wheelPresent];

	if (self.wheelSeen != was)
		[self refresh];
}

/*
 * Whether the wheel is on the bus, asked the same way the daemon asks: by
 * vendor and product id, through the IOKit helper t150boot and the daemon
 * already share. It needs no device open and no root.
 *
 * This used to run ioreg and look for a registry entry named T150, which was
 * a guess and was wrong. The menu said no wheel found while a game was being
 * driven by that very wheel, which is worse than saying nothing: it invites
 * somebody to go hunting for a fault that is not there.
 *
 * Both product ids count. The boot one means it is plugged in but not yet
 * switched, which leaveBootMode above puts right on the next tick of the
 * watch timer, and which the daemon also does on its own scan.
 */
- (BOOL)wheelPresent
{
	io_service_t s;

	if ((s = t150_usb_find(T150_VID, T150_PID_FIRMWARE)) != IO_OBJECT_NULL) {
		IOObjectRelease(s);
		return YES;
	}
	if ((s = t150_usb_find(T150_VID, T150_PID_BOOT)) != IO_OBJECT_NULL) {
		IOObjectRelease(s);
		return YES;
	}

	return NO;
}

#pragma mark - what the wheel keeps for itself

/*
 * Both of these are the wheel's own settings rather than anything a game
 * sends, so they are applied with t150ctl, which writes them without taking
 * the wheel from whatever is using it, and remembered so the daemon can put
 * them back after a replug. The wheel forgets both when it is unplugged.
 */
static const int rotations[] = { 270, 360, 540, 720, 900, 1080 };

/* Named rather than numbered: 0 to 10000 means nothing to a person. */
static const int springs[] = { 0, 2500, 5000, 7500, 10000 };
static NSString * const springNames[] = { @"Off", @"Light", @"Medium",
    @"Firm", @"Full" };

/*
 * The wheel's default is its widest, which A49 measured: setting 1080
 * explicitly changed nothing, because that is where it powers up. So there is
 * no separate "leave it alone" to offer. A row that did nothing was worse
 * than none, since picking it moved the tick while the wheel stayed where it
 * was, and a wheel keeps a range until it is unplugged.
 */
- (int)storedRotation
{
	NSInteger v = [[NSUserDefaults standardUserDefaults]
	    integerForKey:@"rotation"];

	return v > 0 ? (int)v : T150_RANGE_MAX;
}

- (int)storedSpring
{
	return (int)[[NSUserDefaults standardUserDefaults]
	    integerForKey:@"autocentre"];
}

- (NSMenu *)buildRotationMenu
{
	NSMenu *sub = [[NSMenu alloc] init];
	unsigned i;

	for (i = 0; i < sizeof(rotations) / sizeof(rotations[0]); i++) {
		NSString *title = rotations[i] == (int)T150_RANGE_MAX ?
		    [NSString stringWithFormat:@"%d degrees, the wheel's own",
		    rotations[i]] :
		    [NSString stringWithFormat:@"%d degrees", rotations[i]];
		NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title
		    action:@selector(pickRotation:) keyEquivalent:@""];
		it.target = self;
		it.tag = rotations[i];
		it.state = [self storedRotation] == rotations[i] ?
		    NSControlStateValueOn : NSControlStateValueOff;
		[sub addItem:it];
	}

	return sub;
}

- (NSMenu *)buildAutocentreMenu
{
	NSMenu *sub = [[NSMenu alloc] init];
	unsigned i;

	for (i = 0; i < sizeof(springs) / sizeof(springs[0]); i++) {
		NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:springNames[i]
		    action:@selector(pickSpring:) keyEquivalent:@""];
		it.target = self;
		it.tag = springs[i];
		it.state = [self storedSpring] == springs[i] ?
		    NSControlStateValueOn : NSControlStateValueOff;
		[sub addItem:it];
	}

	NSMenuItem *note = [[NSMenuItem alloc] initWithTitle:
	    @"For games that send no force feedback" action:NULL
	    keyEquivalent:@""];
	note.enabled = NO;
	[sub addItem:[NSMenuItem separatorItem]];
	[sub addItem:note];

	return sub;
}

/* t150ctl, from this bundle, so what runs matches what shipped. */
- (BOOL)wheelSetting:(NSString *)command value:(int)v
{
	NSTask *t = [[NSTask alloc] init];

	t.executableURL = [NSURL fileURLWithPath:[self resource:@"t150ctl"]];
	t.arguments = @[ command, [NSString stringWithFormat:@"%d", v] ];
	t.standardOutput = [NSFileHandle fileHandleWithNullDevice];
	t.standardError = [NSFileHandle fileHandleWithNullDevice];
	if (![t launchAndReturnError:NULL])
		return NO;
	[t waitUntilExit];

	return t.terminationStatus == 0;
}

- (void)pickRotation:(NSMenuItem *)sender
{
	int deg = (int)sender.tag;

	[[NSUserDefaults standardUserDefaults] setInteger:deg
	    forKey:@"rotation"];

	/* Every row sends its number, including the wheel's own maximum. */
	if (![self wheelSetting:@"range" value:deg])
		[self note:@"The wheel did not take that rotation. Is it "
		    "plugged in and out of boot mode?"];

	[self tick:sender.menu tag:deg];
	[self refresh];
}

- (void)pickSpring:(NSMenuItem *)sender
{
	int v = (int)sender.tag;

	[[NSUserDefaults standardUserDefaults] setInteger:v
	    forKey:@"autocentre"];

	if (![self wheelSetting:@"autocenter" value:v])
		[self note:@"The wheel did not take that. Is it plugged in "
		    "and out of boot mode?"];

	[self tick:sender.menu tag:v];
	[self refresh];
}

/*
 * Move the tick to the chosen row. Rebuilding the submenu from the item that
 * owns it was the first attempt and reached the wrong item: a submenu knows
 * its parent menu, not its own position in it. Setting the states directly
 * needs neither. Separators and the explanatory line carry no action, which
 * is what tells them apart from a row with a tag of zero that is a real
 * choice.
 */
- (void)tick:(NSMenu *)menu tag:(NSInteger)tag
{
	for (NSMenuItem *it in menu.itemArray)
		it.state = (it.action != NULL && it.tag == tag) ?
		    NSControlStateValueOn : NSControlStateValueOff;
}

- (void)note:(NSString *)text
{
	NSAlert *a = [[NSAlert alloc] init];

	a.messageText = text;
	[a addButtonWithTitle:@"OK"];
	[a runModal];
}

#pragma mark - the daemon

- (void)toggleDaemon:(id)sender
{
	(void)sender;

	if (self.daemon != nil && self.daemon.isRunning) {
		[self.daemon terminate];
		return;
	}
	[self startDaemon];
}

/*
 * Started as a child so its -v output belongs to this process. That is where
 * the status line comes from, and it is why this never has to connect to the
 * daemon and risk displacing a game.
 */
- (void)startDaemon
{
	NSTask *t = [[NSTask alloc] init];
	NSPipe *p = [NSPipe pipe];
	NSError *err = nil;

	/*
	 * Both settings go to the daemon as well as to the wheel. The wheel
	 * forgets them when it is unplugged, and the daemon is the only thing
	 * that notices a replug and can put them back.
	 */
	NSMutableArray *args = [@[ @"-v", @"-w" ] mutableCopy];
	int deg = [self storedRotation], spring = [self storedSpring];

	[args addObjectsFromArray:@[ @"-r",
	    [NSString stringWithFormat:@"%d", deg] ]];
	if (spring > 0)
		[args addObjectsFromArray:@[ @"-a",
		    [NSString stringWithFormat:@"%d", spring] ]];

	t.executableURL = [NSURL fileURLWithPath:[self daemonPath]];
	t.arguments = args;
	t.standardOutput = p;
	t.standardError = p;

	__weak __typeof__(self) weak = self;
	p.fileHandleForReading.readabilityHandler = ^(NSFileHandle *fh) {
		NSData *d = fh.availableData;

		/*
		 * Empty means end of file, and a handler left in place after
		 * that is called again immediately and forever. Clearing it is
		 * what ends the read.
		 */
		if (d.length == 0) {
			fh.readabilityHandler = nil;
			return;
		}

		NSString *s = [[NSString alloc] initWithData:d
		    encoding:NSUTF8StringEncoding];
		dispatch_async(dispatch_get_main_queue(), ^{
			[weak appendLog:s];
		});
	};

	t.terminationHandler = ^(NSTask *task) {
		(void)task;
		dispatch_async(dispatch_get_main_queue(), ^{
			weak.daemon = nil;
			weak.clientConnected = NO;
			[weak refresh];
		});
	};

	if (![t launchAndReturnError:&err]) {
		[self say:[NSString stringWithFormat:
		    @"cannot start the daemon: %@\n", err.localizedDescription]];
		return;
	}

	self.daemon = t;
	self.clientConnected = NO;
	[self refresh];
}

/*
 * The daemon says when a game arrives and when it goes. Reading its own words
 * rather than asking it keeps the one-client rule intact.
 */
- (void)appendLog:(NSString *)s
{
	if ([s containsString:@"said hello"])
		self.clientConnected = YES;
	if ([s containsString:@"client went away"] ||
	    [s containsString:@"safe state: shutting down"])
		self.clientConnected = NO;

	[self say:s];
	[self refresh];
}

#pragma mark - start at login

- (NSString *)agentPath
{
	return [NSHomeDirectory() stringByAppendingPathComponent:
	    [NSString stringWithFormat:@"Library/LaunchAgents/%@.plist",
	    AGENT_LABEL]];
}

/* The login item has to carry the same settings the menu does. */
- (NSArray *)loginArguments
{
	NSMutableArray *a = [@[ [self daemonPath], @"-w" ] mutableCopy];
	int deg = [self storedRotation], spring = [self storedSpring];

	[a addObjectsFromArray:@[ @"-r",
	    [NSString stringWithFormat:@"%d", deg] ]];
	if (spring > 0)
		[a addObjectsFromArray:@[ @"-a",
		    [NSString stringWithFormat:@"%d", spring] ]];

	return a;
}

- (BOOL)loginEnabled
{
	return [[NSFileManager defaultManager]
	    fileExistsAtPath:[self agentPath]];
}

- (void)toggleLogin:(id)sender
{
	(void)sender;
	NSString *path = [self agentPath];

	if ([self loginEnabled]) {
		[[NSFileManager defaultManager] removeItemAtPath:path
		    error:NULL];
		[self say:@"start at login turned off\n"];
		[self refresh];
		return;
	}

	NSDictionary *plist = @{
		@"Label" : AGENT_LABEL,
		@"ProgramArguments" : [self loginArguments],
		@"RunAtLoad" : @YES,
		/*
		 * Restarted if it dies, which is what makes this worth
		 * having: an unplugged wheel does not end the daemon, it
		 * waits, so this only fires on a real failure.
		 */
		@"KeepAlive" : @YES,
		@"StandardErrorPath" : [NSHomeDirectory()
		    stringByAppendingPathComponent:@"Library/Logs/t150d.log"],
	};

	[[NSFileManager defaultManager] createDirectoryAtPath:
	    [path stringByDeletingLastPathComponent]
	    withIntermediateDirectories:YES attributes:nil error:NULL];

	if ([plist writeToFile:path atomically:YES])
		[self say:@"start at login turned on, from the next login\n"];
	else
		[self say:@"could not write the login item\n"];

	[self refresh];
}

#pragma mark - the setup window

- (void)openSetup:(id)sender
{
	(void)sender;

	if (self.setup != nil) {
		/* A bottle may have been made since this was last opened. */
		[self.bottles removeAllItems];
		[self.bottles addItemsWithTitles:[self findBottles]];
		[NSApp activateIgnoringOtherApps:YES];
		[self.setup makeKeyAndOrderFront:nil];
		return;
	}

	NSWindow *w = [[NSWindow alloc]
	    initWithContentRect:NSMakeRect(0, 0, 620, 460)
	    styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	    NSWindowStyleMaskResizable)
	    backing:NSBackingStoreBuffered defer:NO];
	w.title = @"crossover-wheel";
	w.delegate = self;
	w.releasedWhenClosed = NO;
	w.titlebarAppearsTransparent = NO;
	w.minSize = NSMakeSize(520, 380);

	/*
	 * A material behind the content rather than a flat fill. macOS draws
	 * its own current look into this, so the window follows whatever the
	 * system does rather than whatever was fashionable when it was
	 * written, which is the complaint that prompted it.
	 */
	NSVisualEffectView *bg = [[NSVisualEffectView alloc]
	    initWithFrame:w.contentView.bounds];
	bg.material = NSVisualEffectMaterialWindowBackground;
	bg.blendingMode = NSVisualEffectBlendingModeBehindWindow;
	bg.state = NSVisualEffectStateFollowsWindowActiveState;
	bg.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	w.contentView = bg;

	NSView *v = w.contentView;

	/*
	 * Every colour here is a semantic one rather than a literal. The log
	 * view was left at its default, which is black on white, and on a
	 * machine in dark mode that is black text the reader cannot see. A
	 * system colour is two values, and macOS picks the right one.
	 */
	NSTextField *l1 = [NSTextField labelWithString:
	    @"Which CrossOver bottle is the game in?"];
	l1.font = [NSFont systemFontOfSize:[NSFont systemFontSize]
	    weight:NSFontWeightSemibold];
	l1.frame = NSMakeRect(24, 408, 420, 22);
	[v addSubview:l1];

	self.bottles = [[NSPopUpButton alloc]
	    initWithFrame:NSMakeRect(24, 372, 340, 26) pullsDown:NO];
	[self.bottles addItemsWithTitles:[self findBottles]];
	[v addSubview:self.bottles];

	self.install = [NSButton buttonWithTitle:@"Install"
	    target:self action:@selector(runInstall:)];
	self.install.frame = NSMakeRect(486, 370, 110, 32);
	self.install.bezelStyle = NSBezelStyleRounded;
	self.install.keyEquivalent = @"\r";
	[v addSubview:self.install];

	NSTextField *l2 = [NSTextField labelWithString:
	    @"The proxy goes into that bottle. Nothing else is touched."];
	l2.frame = NSMakeRect(24, 344, 520, 18);
	l2.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
	l2.textColor = [NSColor secondaryLabelColor];
	[v addSubview:l2];

	NSScrollView *sc = [[NSScrollView alloc]
	    initWithFrame:NSMakeRect(24, 24, 572, 300)];
	sc.hasVerticalScroller = YES;
	sc.borderType = NSNoBorder;
	sc.drawsBackground = YES;
	sc.backgroundColor = [NSColor textBackgroundColor];
	sc.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	sc.wantsLayer = YES;
	sc.layer.cornerRadius = 8;

	self.out = [[NSTextView alloc] initWithFrame:sc.contentView.bounds];
	self.out.editable = NO;
	self.out.drawsBackground = YES;
	self.out.backgroundColor = [NSColor textBackgroundColor];
	self.out.textColor = [NSColor labelColor];
	self.out.font = [NSFont monospacedSystemFontOfSize:11
	    weight:NSFontWeightRegular];
	self.out.textContainerInset = NSMakeSize(8, 8);
	self.out.autoresizingMask = NSViewWidthSizable;
	sc.documentView = self.out;
	[v addSubview:sc];

	self.setup = w;
	[w center];
	[NSApp activateIgnoringOtherApps:YES];
	[w makeKeyAndOrderFront:nil];

	if (self.logBuf.length > 0)
		[self.out.textStorage appendAttributedString:
		    [[NSAttributedString alloc] initWithString:self.logBuf
		    attributes:@{
			NSForegroundColorAttributeName : [NSColor labelColor],
			NSFontAttributeName : self.out.font }]];

	if ([self findBottles].count == 0) {
		/*
		 * Say where it looked. An empty list means either there are no
		 * bottles or this could not read the folder, and those need
		 * different answers from the person reading it.
		 */
		[self say:[NSString stringWithFormat:
		    @"No CrossOver bottles found in:\n  %@\n\n"
		    "If your bottles are there, macOS may be refusing this app "
		    "access to the folder: look in System Settings, Privacy "
		    "and Security, Files and Folders. If they are somewhere "
		    "else, or there are none yet, make one in CrossOver and "
		    "install the game into it first.\n", [self bottleRoot]]];
	} else {
		[self say:@"Ready. Pick the bottle your game is in and press "
		    "Install.\n\n"];
	}
}

/*
 * Everything the installer does is install.sh's, run from Resources so it
 * finds the binaries, the man pages and the proxy beside itself exactly as it
 * does from an extracted release.
 */
- (void)runInstall:(id)sender
{
	(void)sender;

	if (self.bottles.numberOfItems == 0) {
		[self say:@"There is no bottle to install into.\n"];
		return;
	}

	NSTask *t = [[NSTask alloc] init];
	NSPipe *p = [NSPipe pipe];
	NSError *err = nil;

	/*
	 * The bottle and nothing else. The command line tools are not this
	 * application's business: it carries the daemon it runs, so a person
	 * who only ever uses the menu bar never needs anything on their PATH.
	 * install.sh is still there for anyone who wants them.
	 */
	t.executableURL = [NSURL fileURLWithPath:@"/bin/sh"];
	t.arguments = @[ [self resource:@"install.sh"],
	    @"-b", self.bottles.titleOfSelectedItem,
	    @"--no-binaries", @"--no-app" ];
	t.currentDirectoryURL = [NSURL fileURLWithPath:
	    [[NSBundle mainBundle] resourcePath]];
	t.standardOutput = p;
	t.standardError = p;

	__weak __typeof__(self) weak = self;
	p.fileHandleForReading.readabilityHandler = ^(NSFileHandle *fh) {
		NSData *d = fh.availableData;

		if (d.length == 0) {		/* end of file, see startDaemon */
			fh.readabilityHandler = nil;
			return;
		}

		NSString *s = [[NSString alloc] initWithData:d
		    encoding:NSUTF8StringEncoding];
		dispatch_async(dispatch_get_main_queue(), ^{
			[weak say:s];
		});
	};

	t.terminationHandler = ^(NSTask *task) {
		int st = task.terminationStatus;

		dispatch_async(dispatch_get_main_queue(), ^{
			weak.install.enabled = YES;
			if (st == 0)
				[[NSUserDefaults standardUserDefaults]
				    setBool:YES forKey:@"installedOnce"];
			/*
			 * The commonest failure is macOS refusing this app
			 * access to CrossOver.app, which the script explains
			 * in its own output. Repeating it here would be
			 * noise; what this adds is that nothing was left half
			 * done, which is the thing somebody looking at an
			 * error most wants to know.
			 */
			[weak say:st == 0 ?
			    @"\nDone. Start the daemon from the menu bar, then "
			    "start your game.\n" :
			    @"\nThat did not work, and nothing was left half "
			    "done: the installer checks as it goes and stops "
			    "at the first thing it cannot verify.\n"];
			[weak refresh];
		});
	};

	self.install.enabled = NO;
	[self say:@"\n"];
	if (![t launchAndReturnError:&err]) {
		self.install.enabled = YES;
		[self say:[NSString stringWithFormat:@"cannot run the "
		    "installer: %@\n", err.localizedDescription]];
	}
}

- (void)say:(NSString *)s
{
	if (s.length == 0)
		return;

	/*
	 * Kept whether or not a window is open to show it. Everything the
	 * daemon prints used to go straight into the setup window's text view,
	 * which does not exist once that window is closed, so a person running
	 * from the menu bar had no log at all and no way to send one. Every
	 * real diagnosis in this project came out of a log somebody mailed.
	 *
	 * Bounded, because this runs for as long as the application does and
	 * the daemon is talkative under -v. The oldest half goes.
	 */
	if (self.logBuf == nil)
		self.logBuf = [NSMutableString string];
	[self.logBuf appendString:s];
	if (self.logBuf.length > 400000)
		[self.logBuf deleteCharactersInRange:
		    NSMakeRange(0, self.logBuf.length - 200000)];

	if (self.out == nil)
		return;

	/*
	 * Appending through the storage keeps the view's typing attributes,
	 * which do not include a colour, so anything added this way falls
	 * back to black. Say the colour with the text.
	 */
	NSDictionary *attrs = @{
		NSForegroundColorAttributeName : [NSColor labelColor],
		NSFontAttributeName : self.out.font,
	};

	[self.out.textStorage appendAttributedString:
	    [[NSAttributedString alloc] initWithString:s attributes:attrs]];
	[self.out scrollRangeToVisible:
	    NSMakeRange(self.out.string.length, 0)];
}

- (void)windowWillClose:(NSNotification *)n
{
	(void)n;
	self.setup = nil;
	self.out = nil;
}

/*
 * Onto the clipboard rather than into a window, because the useful thing to do
 * with this log is paste it into a mail to somebody who can read it.
 */
- (void)copyLog:(id)sender
{
	(void)sender;
	NSPasteboard *pb = [NSPasteboard generalPasteboard];
	NSString *log = self.logBuf;

	if (log.length == 0) {
		[self note:@"There is nothing in the log yet. It fills up "
		    "while the daemon is running."];
		return;
	}

	[pb clearContents];
	[pb setString:log forType:NSPasteboardTypeString];
	[self note:[NSString stringWithFormat:@"%lu lines copied. Paste them "
	    "into a mail.", (unsigned long)[[log
	    componentsSeparatedByString:@"\n"] count] - 1]];
}

#pragma mark - updates

/*
 * Asks the releases API what the newest version is, and can install it.
 *
 * The download it fetches carries no quarantine flag, because that is set by
 * whatever downloads a file and this is not a browser. So an update installed
 * here needs none of the right-click Open and drag-to-Applications the first
 * install did. That is the whole reason for doing it in place rather than
 * opening the releases page.
 */
- (void)checkForUpdates:(id)sender
{
	[self lookForUpdate:sender != nil];
}

/*
 * Quiet when it runs itself at launch and talkative when a person asks.
 * Telling somebody they are up to date is an answer to a question; saying it
 * unprompted every time the application starts is noise, and noise trains
 * people to dismiss the dialog that matters.
 */
- (void)lookForUpdate:(BOOL)announce
{
	NSString *mine = [[NSBundle mainBundle]
	    objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
	NSURL *api = [NSURL URLWithString:@"https://api.github.com/repos/"
	    "renaudallard/crossover-wheel/releases/latest"];
	NSURLRequest *req = [NSURLRequest requestWithURL:api
	    cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
	    timeoutInterval:15];

	[[[NSURLSession sharedSession] dataTaskWithRequest:req
	    completionHandler:^(NSData *d, NSURLResponse *r, NSError *e) {
		(void)r;
		NSString *latest = nil;
		NSString *dmg = nil, *sums = nil;

		if (d != nil) {
			id j = [NSJSONSerialization JSONObjectWithData:d
			    options:0 error:NULL];
			if ([j isKindOfClass:[NSDictionary class]]) {
				latest = [j objectForKey:@"tag_name"];
				for (id a in [j objectForKey:@"assets"]) {
					NSString *n = [a objectForKey:@"name"];
					NSString *u = [a objectForKey:
					    @"browser_download_url"];

					if ([n hasSuffix:@".dmg"])
						dmg = u;
					else if ([n isEqualToString:@"SHA256SUMS"])
						sums = u;
				}
			}
		}
		if ([latest hasPrefix:@"v"])
			latest = [latest substringFromIndex:1];

		dispatch_async(dispatch_get_main_queue(), ^{
			if (!announce && (latest.length == 0 ||
			    [latest isEqualToString:mine]))
				return;
			[self showUpdate:latest mine:mine dmg:dmg sums:sums
			    error:e];
		});
	}] resume];
}

- (void)showUpdate:(NSString *)latest mine:(NSString *)mine
    dmg:(NSString *)dmg sums:(NSString *)sums error:(NSError *)e
{
	NSAlert *a = [[NSAlert alloc] init];

	if (latest.length == 0) {
		a.messageText = @"Could not check for updates";
		a.informativeText = e != nil ? e.localizedDescription
		    : @"The releases page did not answer with a version.";
		[a addButtonWithTitle:@"OK"];
		[a runModal];
		return;
	}

	/*
	 * String equality rather than an ordering. Versions here are always
	 * the newest tag against the one this was built from, so "different"
	 * is the only question, and comparing them numerically would need a
	 * parser for a format nothing enforces.
	 */
	if ([latest isEqualToString:mine]) {
		a.messageText = [NSString stringWithFormat:
		    @"Up to date (%@)", mine];
		a.informativeText = @"This is the newest release.";
		[a addButtonWithTitle:@"OK"];
		[a runModal];
		return;
	}

	a.messageText = [NSString stringWithFormat:@"Version %@ is available",
	    latest];

	if (dmg == nil) {
		a.informativeText = [NSString stringWithFormat:
		    @"You have %@. That release has no disk image to install "
		    "from, so it has to be done by hand.", mine];
		[a addButtonWithTitle:@"Open the releases page"];
		[a addButtonWithTitle:@"Later"];
		if ([a runModal] == NSAlertFirstButtonReturn)
			[[NSWorkspace sharedWorkspace] openURL:[NSURL
			    URLWithString:@"https://github.com/renaudallard/"
			    "crossover-wheel/releases/latest"]];
		return;
	}

	a.informativeText = [NSString stringWithFormat:
	    @"You have %@. This will download it, check it against the "
	    "published checksum, replace this application and start it again. "
	    "The daemon stops while it happens.", mine];
	[a addButtonWithTitle:@"Update now"];
	[a addButtonWithTitle:@"Later"];

	if ([a runModal] == NSAlertFirstButtonReturn)
		[self downloadUpdate:dmg sums:sums version:latest];
}

static NSString *
sha256_of(NSData *d)
{
	unsigned char out[CC_SHA256_DIGEST_LENGTH];
	NSMutableString *hex = [NSMutableString string];
	unsigned i;

	CC_SHA256(d.bytes, (CC_LONG)d.length, out);
	for (i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
		[hex appendFormat:@"%02x", out[i]];

	return hex;
}

/*
 * Download, then check the bytes against the checksum published beside them
 * before anything is mounted or copied. The checksum comes from the same
 * place as the image, so it proves nothing about who made it: what it catches
 * is a truncated or corrupted download, which is the failure that would
 * otherwise leave a half-written application in /Applications.
 */
- (void)downloadUpdate:(NSString *)dmgURL sums:(NSString *)sumsURL
    version:(NSString *)version
{
	NSURL *u = [NSURL URLWithString:dmgURL];

	[self note:[NSString stringWithFormat:@"Downloading %@. This window "
	    "will close and the application will start again by itself.",
	    version]];

	[[[NSURLSession sharedSession] dataTaskWithURL:u
	    completionHandler:^(NSData *img, NSURLResponse *r, NSError *e) {
		(void)r;

		if (img == nil || img.length == 0) {
			dispatch_async(dispatch_get_main_queue(), ^{
				[self note:e != nil ? e.localizedDescription
				    : @"The download was empty."];
			});
			return;
		}

		NSString *want = nil;

		if (sumsURL != nil) {
			NSData *sd = [NSData dataWithContentsOfURL:
			    [NSURL URLWithString:sumsURL]];
			NSString *txt = sd == nil ? nil : [[NSString alloc]
			    initWithData:sd encoding:NSUTF8StringEncoding];

			for (NSString *line in [txt
			    componentsSeparatedByString:@"\n"]) {
				if ([line containsString:@".dmg"]) {
					NSArray *f = [line
					    componentsSeparatedByString:@" "];
					want = f.firstObject;
					break;
				}
			}
		}

		if (want != nil &&
		    ![[sha256_of(img) lowercaseString]
		    isEqualToString:[want lowercaseString]]) {
			dispatch_async(dispatch_get_main_queue(), ^{
				[self note:@"The download did not match its "
				    "published checksum. Nothing was changed."];
			});
			return;
		}

		dispatch_async(dispatch_get_main_queue(), ^{
			[self applyUpdate:img];
		});
	}] resume];
}

/* Mount the image, take the application out of it, and hand over the swap. */
- (void)applyUpdate:(NSData *)image
{
	NSFileManager *fm = [NSFileManager defaultManager];
	NSString *tmp = [NSTemporaryDirectory()
	    stringByAppendingPathComponent:@"crossover-wheel-update"];
	NSString *dmg = [tmp stringByAppendingPathComponent:@"update.dmg"];
	NSString *mnt = [tmp stringByAppendingPathComponent:@"mnt"];
	NSString *staged = [tmp stringByAppendingPathComponent:
	    @"crossover-wheel.app"];

	[fm removeItemAtPath:tmp error:NULL];
	[fm createDirectoryAtPath:tmp withIntermediateDirectories:YES
	    attributes:nil error:NULL];
	if (![image writeToFile:dmg atomically:YES]) {
		[self note:@"Could not write the download to disk."];
		return;
	}

	if ([self run:@"/usr/bin/hdiutil" args:@[ @"attach", @"-nobrowse",
	    @"-readonly", @"-mountpoint", mnt, dmg ]] != 0) {
		[self note:@"Could not open the downloaded disk image."];
		return;
	}

	BOOL got = [fm copyItemAtPath:[mnt stringByAppendingPathComponent:
	    @"crossover-wheel.app"] toPath:staged error:NULL];

	(void)[self run:@"/usr/bin/hdiutil" args:@[ @"detach", mnt, @"-quiet" ]];

	if (!got) {
		[self note:@"The disk image did not contain the application."];
		return;
	}

	/*
	 * Two checks before anything replaces a working application, and they
	 * answer different questions.
	 *
	 * The signature, verified deeply, says the bundle arrived whole: every
	 * nested file still hashes to what the seal says it should. That is
	 * what catches a download that was truncated or tampered with in
	 * transit.
	 *
	 * The identifier says it is this application rather than some other
	 * one. A valid signature on its own proves only that somebody signed
	 * something, and ad-hoc signatures carry no authority to compare
	 * against, so the identifier is what is left to check and it is worth
	 * checking: replacing crossover-wheel with a correctly signed
	 * something-else should never be one bad URL away.
	 */
	if ([self run:@"/usr/bin/codesign" args:@[ @"--verify", @"--deep",
	    @"--strict", staged ]] != 0) {
		[self note:@"The downloaded application is not correctly "
		    "signed. Nothing was changed."];
		return;
	}

	NSBundle *nb = [NSBundle bundleWithPath:staged];
	NSString *want = [[NSBundle mainBundle] bundleIdentifier];

	if (nb == nil || ![nb.bundleIdentifier isEqualToString:want]) {
		[self note:[NSString stringWithFormat:@"The downloaded "
		    "application identifies itself as %@ rather than %@. "
		    "Nothing was changed.",
		    nb.bundleIdentifier ?: @"nothing", want]];
		return;
	}

	NSTask *t = [[NSTask alloc] init];

	t.executableURL = [NSURL fileURLWithPath:@"/bin/sh"];
	t.arguments = @[ [self resource:@"update.sh"], staged,
	    [[NSBundle mainBundle] bundlePath],
	    [NSString stringWithFormat:@"%d", getpid()] ];
	if (![t launchAndReturnError:NULL]) {
		[self note:@"Could not start the updater."];
		return;
	}

	[self quit:nil];
}

/* A subprocess run for its exit status alone. */
- (int)run:(NSString *)tool args:(NSArray<NSString *> *)args
{
	NSTask *t = [[NSTask alloc] init];

	t.executableURL = [NSURL fileURLWithPath:tool];
	t.arguments = args;
	t.standardOutput = [NSFileHandle fileHandleWithNullDevice];
	t.standardError = [NSFileHandle fileHandleWithNullDevice];
	if (![t launchAndReturnError:NULL])
		return -1;
	[t waitUntilExit];

	return t.terminationStatus;
}

/*
 * Quit is not the only way out. A force quit, a Quit AppleEvent or a logout
 * all end the application without going through the menu item, and the daemon
 * we started is our child: left behind it keeps the wheel, and the next launch
 * cannot see it as its own. Cocoa calls this on every one of those.
 */
- (void)applicationWillTerminate:(NSNotification *)n
{
	(void)n;

	[self.watch invalidate];
	self.watch = nil;

	if (self.daemon != nil && self.daemon.isRunning) {
		[self.daemon terminate];
		[self.daemon waitUntilExit];
	}
}

- (void)quit:(id)sender
{
	(void)sender;

	[self.watch invalidate];
	self.watch = nil;

	/*
	 * Stop the daemon we started rather than orphaning it. Its own
	 * shutdown puts the wheel in a safe state, which is the whole reason
	 * not to just exit.
	 */
	if (self.daemon != nil && self.daemon.isRunning) {
		[self.daemon terminate];
		[self.daemon waitUntilExit];
	}
	[NSApp terminate:nil];
}

@end

int
main(void)
{
	@autoreleasepool {
		NSApplication *app = [NSApplication sharedApplication];
		T150Menu *d = [[T150Menu alloc] init];

		app.delegate = d;
		[app run];
	}

	return 0;
}
