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

#define AGENT_LABEL	@"it.allard.t150d"

@interface T150Menu : NSObject <NSApplicationDelegate, NSWindowDelegate,
    NSMenuDelegate>
@property (strong) NSStatusItem *item;
@property (strong) NSTask *daemon;
@property (strong) NSWindow *setup;
@property (strong) NSPopUpButton *bottles;
@property (strong) NSTextField *prefix;
@property (strong) NSButton *install;
@property (strong) NSTextView *out;
@property (strong) NSMenuItem *statusLine;
@property (strong) NSMenuItem *runItem;
@property (strong) NSMenuItem *loginItem;
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
 * Where the tools were told to go. The text field only exists while the setup
 * window is open, so this must not read it blind: everything else here can be
 * asked for the daemon's path before that window has ever been made.
 */
- (NSString *)prefixPath
{
	NSString *typed = self.prefix.stringValue;

	if (typed.length > 0)
		return [typed stringByExpandingTildeInPath];

	return [NSHomeDirectory() stringByAppendingPathComponent:@".local"];
}

- (NSString *)installedDaemon
{
	NSString *p = [[self prefixPath]
	    stringByAppendingPathComponent:@"bin/t150d"];

	if ([[NSFileManager defaultManager] isExecutableFileAtPath:p])
		return p;

	/* Not installed yet: the bundled one still works. */
	return [self resource:@"t150d"];
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

	NSMenuItem *s = [[NSMenuItem alloc] initWithTitle:@"Setup…"
	    action:@selector(openSetup:) keyEquivalent:@""];
	s.target = self;
	[m addItem:s];

	NSMenuItem *q = [[NSMenuItem alloc] initWithTitle:@"Quit"
	    action:@selector(quit:) keyEquivalent:@"q"];
	q.target = self;
	[m addItem:q];

	m.delegate = self;
	self.item.menu = m;
	self.wheelSeen = [self wheelPresent];
	[self refresh];

	/*
	 * Nothing installed yet means the person just double clicked this to
	 * install, so show them the window rather than a menu bar icon they
	 * have to find.
	 */
	if ([self findBottles].count > 0 &&
	    ![[NSFileManager defaultManager] isExecutableFileAtPath:
	    [[self prefixPath] stringByAppendingPathComponent:@"bin/t150d"]])
		[self openSetup:nil];
}

- (void)refresh
{
	BOOL running = self.daemon != nil && self.daemon.isRunning;
	NSString *wheel = self.wheelSeen ? @"wheel connected"
	    : @"no wheel found";

	self.item.button.title = running ? @"◉ T150" : @"○ T150";

	if (running && self.clientConnected)
		self.statusLine.title = [NSString stringWithFormat:
		    @"%@ · daemon running · a game is connected",
		    wheel];
	else if (running)
		self.statusLine.title = [NSString stringWithFormat:
		    @"%@ · daemon running", wheel];
	else
		self.statusLine.title = [NSString stringWithFormat:
		    @"%@ · daemon stopped", wheel];

	self.runItem.title = running ? @"Stop the daemon"
	    : @"Start the daemon";
	self.loginItem.state = [self loginEnabled] ? NSControlStateValueOn
	    : NSControlStateValueOff;
}

/*
 * Looked up only when the menu is about to be shown. refresh runs on every
 * line the daemon prints, and asking a subprocess whether the wheel is there
 * once per log line would be a spawn per line for a status nobody is looking
 * at.
 */
- (void)menuWillOpen:(NSMenu *)menu
{
	(void)menu;
	self.wheelSeen = [self wheelPresent];
	[self refresh];
}

/*
 * Asking IOKit whether the wheel is on the bus, which needs no device open
 * and no root. Either product id counts: the boot one means it is plugged in
 * but not yet switched, which the daemon does by itself once it runs.
 */
- (BOOL)wheelPresent
{
	NSTask *t = [[NSTask alloc] init];
	NSPipe *p = [NSPipe pipe];

	t.executableURL = [NSURL fileURLWithPath:@"/usr/sbin/ioreg"];
	t.arguments = @[ @"-r", @"-n", @"T150", @"-l" ];
	t.standardOutput = p;
	t.standardError = [NSFileHandle fileHandleWithNullDevice];
	if (![t launchAndReturnError:NULL])
		return NO;

	NSData *d = [p.fileHandleForReading readDataToEndOfFile];
	[t waitUntilExit];

	return d.length > 0;
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

	t.executableURL = [NSURL fileURLWithPath:[self installedDaemon]];
	t.arguments = @[ @"-v", @"-w" ];
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
		@"ProgramArguments" : @[ [self installedDaemon], @"-w" ],
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
		[NSApp activateIgnoringOtherApps:YES];
		[self.setup makeKeyAndOrderFront:nil];
		return;
	}

	NSWindow *w = [[NSWindow alloc]
	    initWithContentRect:NSMakeRect(0, 0, 560, 420)
	    styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
	    backing:NSBackingStoreBuffered defer:NO];
	w.title = @"crossover-wheel setup";
	w.delegate = self;
	w.releasedWhenClosed = NO;

	NSView *v = w.contentView;

	NSTextField *l1 = [NSTextField labelWithString:
	    @"Which CrossOver bottle is the game in?"];
	l1.frame = NSMakeRect(20, 372, 400, 20);
	[v addSubview:l1];

	self.bottles = [[NSPopUpButton alloc]
	    initWithFrame:NSMakeRect(20, 342, 300, 26) pullsDown:NO];
	[self.bottles addItemsWithTitles:[self findBottles]];
	[v addSubview:self.bottles];

	NSTextField *l2 = [NSTextField labelWithString:
	    @"Where should the command line tools go?"];
	l2.frame = NSMakeRect(20, 306, 400, 20);
	[v addSubview:l2];

	self.prefix = [[NSTextField alloc]
	    initWithFrame:NSMakeRect(20, 278, 300, 24)];
	self.prefix.stringValue = [NSHomeDirectory()
	    stringByAppendingPathComponent:@".local"];
	[v addSubview:self.prefix];

	self.install = [NSButton buttonWithTitle:@"Install"
	    target:self action:@selector(runInstall:)];
	self.install.frame = NSMakeRect(440, 276, 100, 30);
	self.install.keyEquivalent = @"\r";
	[v addSubview:self.install];

	NSScrollView *sc = [[NSScrollView alloc]
	    initWithFrame:NSMakeRect(20, 20, 520, 240)];
	sc.hasVerticalScroller = YES;
	sc.borderType = NSBezelBorder;

	self.out = [[NSTextView alloc]
	    initWithFrame:sc.contentView.bounds];
	self.out.editable = NO;
	self.out.font = [NSFont monospacedSystemFontOfSize:11
	    weight:NSFontWeightRegular];
	self.out.autoresizingMask = NSViewWidthSizable;
	sc.documentView = self.out;
	[v addSubview:sc];

	self.setup = w;
	[w center];
	[NSApp activateIgnoringOtherApps:YES];
	[w makeKeyAndOrderFront:nil];

	if ([self findBottles].count == 0)
		[self say:@"No CrossOver bottles found. Make one in CrossOver "
		    "first, install the game in it, then come back.\n"];
	else
		[self say:@"Ready. Pick the bottle your game is in and press "
		    "Install.\n\n"];
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

	t.executableURL = [NSURL fileURLWithPath:@"/bin/sh"];
	t.arguments = @[ [self resource:@"install.sh"],
	    @"-b", self.bottles.titleOfSelectedItem,
	    @"-p", self.prefix.stringValue ];
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
			[weak say:st == 0 ?
			    @"\nDone. Start the daemon from the menu bar, then "
			    "start your game.\n" :
			    @"\nThat did not work. Nothing was half done: the "
			    "installer checks as it goes and stops at the "
			    "first thing it cannot verify.\n"];
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
	if (self.out == nil || s.length == 0)
		return;

	[self.out.textStorage.mutableString appendString:s];
	[self.out scrollRangeToVisible:
	    NSMakeRange(self.out.string.length, 0)];
}

- (void)windowWillClose:(NSNotification *)n
{
	(void)n;
	self.setup = nil;
	self.out = nil;
}

- (void)quit:(id)sender
{
	(void)sender;

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
