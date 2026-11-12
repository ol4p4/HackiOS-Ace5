/*
 * The first CA bridge probe, inside SpringBoard: a CAMetalLayer band added to
 * the live window, drawn by Turnip and presented through CoreAnimation.
 *
 * It answers one question the standalone app cannot: can a process that CoreAnimation
 * itself owns -- the window server's own client, not a test program -- acquire a
 * CA drawable, import its IOSurface into Turnip and put pixels on the panel? It
 * does not composite SpringBoard's layers and it does not touch CA's renderer;
 * those are the next stage, chosen after this probe reports.
 *
 * Enabled only by the boot argument vkbridge=1 (or the marker file
 * /redwood/etc/vkbridge). Everything else boots exactly as before.
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>

#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "wsi_redwood.h"
#include "redwood_vk_bridge.h"

#define RWVKB_BUNDLE "/redwood/lib/RedwoodMetal.bundle"
#define RWVKB_MTL_CREATE_DEVICE_UNSLID 0x181a34b3cUL
#define RWVKB_MTL_ADD_DEVICE_UNSLID    0x181a1875cUL
#define RWVKB_BAND_HEIGHT 240.0
#define RWVKB_INTERVALS 120

static void
rwb_say(const char *format, ...) __printflike(1, 2);

static void
rwb_say(const char *format, ...)
{
	char line[512];
	va_list args;

	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	fprintf(stderr, "vkbridge: %s\n", line);
}

/* The same device answer the standalone app uses: Metal's own discovery first,
 * then the bundle's device built on IOAcceleratorES (SpringBoard may have had
 * CA's device removed by inject.c's keep_metal_from_core_animation). */
static id<MTLDevice>
rwb_make_device(void)
{
	id<MTLDevice> found = MTLCreateSystemDefaultDevice();

	if (found != nil) {
		rwb_say("device: Metal's own discovery gave %s",
		    class_getName(object_getClass(found)));
		return found;
	}
	rwb_say("device: discovery gave nil, building one from the bundle");
	NSBundle *bundle = [NSBundle bundleWithPath:
	    [NSString stringWithUTF8String:RWVKB_BUNDLE]];
	if (bundle == nil) {
		rwb_say("FAILED: no bundle at %s", RWVKB_BUNDLE);
		return nil;
	}
	Class cls = [bundle classNamed:@"RedwoodMetalDevice"];
	if (cls == Nil)
		cls = [bundle principalClass];
	if (cls == Nil) {
		rwb_say("FAILED: the bundle gave up no class");
		return nil;
	}
	io_service_t service = IOServiceGetMatchingService(0,
	    IOServiceMatching("IOAcceleratorES"));
	if (service == IO_OBJECT_NULL) {
		rwb_say("FAILED: no IOAcceleratorES service");
		return nil;
	}
	id device = ((id (*)(id, SEL, io_service_t))objc_msgSend)([cls alloc],
	    sel_getUid("initWithAcceleratorPort:"), service);
	if (device == nil) {
		rwb_say("FAILED: -initWithAcceleratorPort: gave nil");
		return nil;
	}
	uintptr_t slide = (uintptr_t)&MTLCreateSystemDefaultDevice -
	    RWVKB_MTL_CREATE_DEVICE_UNSLID;
	void (*add_device)(id) = (void (*)(id))(slide + RWVKB_MTL_ADD_DEVICE_UNSLID);
	@try {
		add_device(device);
	} @catch (NSException *exception) {
		rwb_say("FAILED: MTLAddDevice threw %s: %s",
		    [[exception name] UTF8String], [[exception reason] UTF8String]);
		return nil;
	}
	id<MTLDevice> system = MTLCreateSystemDefaultDevice();
	if (system == nil) {
		rwb_say("FAILED: MTLAddDevice took the device and Metal still has none");
		return nil;
	}
	rwb_say("device: built in this process, %s",
	    class_getName(object_getClass(system)));
	return system;
}

@interface RWVKBView : UIView
@end
@implementation RWVKBView
+ (Class)layerClass
{
	return [CAMetalLayer class];
}
@end

/* The overlay's own window, so it does not ride a transient SpringBoard window
 * (the first key window appears during boot and is replaced when the home
 * screen comes up; a sublayer of it is then composited nowhere). */
@interface RWVKBViewController : UIViewController
@end
@implementation RWVKBViewController
- (void)loadView
{
	self.view = [[RWVKBView alloc] initWithFrame:CGRectMake(0, 0,
	    [UIScreen mainScreen].bounds.size.width, RWVKB_BAND_HEIGHT)];
}
@end

@interface RWVKBOverlay : NSObject
@property(nonatomic, strong) CAMetalLayer *layer;
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@property(nonatomic, strong) UIWindow *window;
@property(nonatomic) BOOL failed;
@end

/* SpringBoard's key window, or the first one it has. nil means "not up yet". */
static UIWindow *
rwb_key_window(void)
{
	for (UIWindow *candidate in [UIApplication sharedApplication].windows)
		if (candidate.isKeyWindow)
			return candidate;
	return [UIApplication sharedApplication].windows.firstObject;
}

@implementation RWVKBOverlay {
	struct rw_wsi *_wsi;
	struct rwb *_bridge;
	uint64_t _frame;
	double _last_time;
	double _intervals[RWVKB_INTERVALS];
	unsigned _interval_count;
	unsigned _interval_next;
	dispatch_source_t _timer;
}

- (void)fail:(const char *)why
{
	rwb_say("FAILED: %s", why != NULL ? why : "(no sentence)");
	self.failed = YES;
	if (_timer != NULL) {
		dispatch_source_cancel(_timer);
		_timer = NULL;
	}
}

- (BOOL)start
{
	char why[192];
	UIWindowScene *scene = nil;
	UIWindow *window = rwb_key_window();

	if (window == nil) {
		rwb_say("no SpringBoard window yet");
		return NO;
	}
	if (@available(iOS 13.0, *)) {
		for (UIScene *candidate in [UIApplication sharedApplication].connectedScenes)
			if (candidate.activationState == UISceneActivationStateForegroundActive) {
				scene = (UIWindowScene *)candidate;
				break;
			}
		if (scene == nil) {
			rwb_say("no foreground-active scene yet");
			return NO;
		}
	}
	self.device = rwb_make_device();
	if (self.device == nil) {
		[self fail:"no Metal device"];
		return NO;
	}
	self.queue = [self.device newCommandQueue];
	if (self.queue == nil) {
		[self fail:"the device gave no command queue"];
		return NO;
	}
	/*
	 * Our own window, at a level above the status bar and below nothing else:
	 * a sublayer of SpringBoard's first window is composited nowhere once the
	 * home screen replaces that window (measured 2026-11-12: frames presented
	 * for 1,300+ ticks while the panel showed the plain home screen).
	 */
	self.window = [[UIWindow alloc] initWithFrame:CGRectMake(0, 0,
	    [UIScreen mainScreen].bounds.size.width, RWVKB_BAND_HEIGHT)];
	if (@available(iOS 13.0, *))
		self.window.windowScene = scene;
	self.window.windowLevel = UIWindowLevelNormal + 1;
	self.window.userInteractionEnabled = NO;
	self.window.backgroundColor = [UIColor clearColor];
	RWVKBViewController *controller = [[RWVKBViewController alloc] init];
	self.window.rootViewController = controller;
	self.window.hidden = NO;
	CALayer *host = controller.view.layer;
	if (host == nil) {
		[self fail:"the overlay view has no layer"];
		return NO;
	}
	self.layer = (CAMetalLayer *)host;
	self.layer.device = self.device;
	self.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	self.layer.framebufferOnly = NO;
	self.layer.contentsScale = [UIScreen mainScreen].scale;
	[self.window layoutIfNeeded];
	controller.view.frame = self.window.bounds;
	self.layer.frame = controller.view.bounds;
	rwb_say("geometry: window %.0fx%.0f at %.0f,%.0f; view %.0fx%.0f; "
	    "layer %.0fx%.0f at %.0f,%.0f pos %.0f,%.0f scale %.1f",
	    self.window.bounds.size.width, self.window.bounds.size.height,
	    self.window.frame.origin.x, self.window.frame.origin.y,
	    controller.view.bounds.size.width, controller.view.bounds.size.height,
	    self.layer.bounds.size.width, self.layer.bounds.size.height,
	    self.layer.frame.origin.x, self.layer.frame.origin.y,
	    self.layer.position.x, self.layer.position.y,
	    (double)self.layer.contentsScale);
	rwb_say("overlay window %.0fx%.0f level %.1f, scene %s, layer %s, scale %.1f",
	    self.window.frame.size.width, self.window.frame.size.height,
	    (double)self.window.windowLevel,
	    scene != nil ? class_getName([scene class]) : "none",
	    class_getName([host class]), (double)self.layer.contentsScale);

	if (rw_wsi_open(&_wsi, self.layer, self.device, self.queue, why, sizeof(why)) != 0) {
		[self fail:why];
		return NO;
	}
	_bridge = rwb_open(self.device, why, sizeof(why));
	if (_bridge == NULL) {
		[self fail:why];
		return NO;
	}
	_last_time = CACurrentMediaTime();
	/*
	 * The loop runs on its own queue, not SpringBoard's main thread: the same
	 * overlay measured ~65 ms between ticks on a CADisplayLink there, and the
	 * isolation boot (no gate, no app) kept that number, so the main thread's
	 * work is the first suspect, not the gate.
	 */
	dispatch_queue_t queue = dispatch_queue_create("com.redwood.vkbridge", NULL);
	_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
	if (_timer == NULL) {
		[self fail:"no dispatch timer"];
		return NO;
	}
	dispatch_source_set_timer(_timer, dispatch_time(DISPATCH_TIME_NOW, 0),
	    (uint64_t)(16.6666667 * NSEC_PER_MSEC), NSEC_PER_MSEC);
	dispatch_source_set_event_handler(_timer, ^{
		[self tick:nil];
	});
	dispatch_resume(_timer);
	rwb_say("display timer started (60 Hz, own queue)");
	return YES;
}

- (void)tick:(CADisplayLink *)link
{
	char why[192];
	double now = CACurrentMediaTime();
	double interval = now - _last_time;
	uint32_t phase = (uint32_t)(_frame * 3u);

	_last_time = now;
	if (_frame > 0) {
		_intervals[_interval_next] = interval;
		_interval_next = (_interval_next + 1) % RWVKB_INTERVALS;
		if (_interval_count < RWVKB_INTERVALS)
			_interval_count++;
	}
	/*
	 * A band that is unmistakably not the system UI: green sweeping through
	 * red, never a system colour at rest. The alpha is opaque on purpose --
	 * a partly transparent surface would hide an import/format mistake.
	 */
	uint32_t bgra = 0xff000000u | ((phase & 0xffu) << 16) |
	    (((255u - (phase & 0xffu)) & 0xffu) << 8) | 0x20u;
	if (rwb_present_clear(_bridge, _wsi, bgra, why, sizeof(why)) != 0) {
		if (_frame < 5 || (_frame % 60) == 0)
			rwb_say("frame %llu: %s", _frame, why);
	} else if (_frame < 5 || (_frame % 60) == 0) {
		rwb_say("frame %llu: presented, interval %.3f ms",
		    _frame, interval * 1000.0);
	}
	if (_frame > 0 && _frame % RWVKB_INTERVALS == 0 && _interval_count > 0) {
		double copy[RWVKB_INTERVALS];

		memcpy(copy, _intervals, sizeof(copy));
		for (unsigned i = 1; i < _interval_count; i++) {
			double value = copy[i];
			unsigned j = i;

			while (j > 0 && copy[j - 1] > value) {
				copy[j] = copy[j - 1];
				j--;
			}
			copy[j] = value;
		}
		rwb_say("interval median %.3f ms over %u frames",
		    copy[_interval_count / 2] * 1000.0, _interval_count);
	}
	_frame++;
}
@end

static BOOL
rwb_boot_arg(const char *needle)
{
	char args[2048];
	size_t length = sizeof(args);

	if (sysctlbyname("kern.bootargs", args, &length, NULL, 0) != 0)
		return NO;
	args[sizeof(args) - 1] = '\0';
	return strstr(args, needle) != NULL;
}

static BOOL
rwb_wanted(void)
{
	if (rwb_boot_arg("vkbridge=1")) {
		rwb_say("wanted by kern.bootargs");
		return YES;
	}
	if (access("/redwood/etc/vkbridge", F_OK) == 0) {
		rwb_say("wanted by /redwood/etc/vkbridge");
		return YES;
	}
	return NO;
}

static void
rwb_try_start(unsigned attempt)
{
	static RWVKBOverlay *overlay;

	if (attempt > 120) {
		rwb_say("gave up waiting for a SpringBoard window");
		return;
	}
	if (overlay == nil)
		overlay = [[RWVKBOverlay alloc] init];
	if (overlay.failed) {
		rwb_say("not retrying after a failed start");
		return;
	}
	if (rwb_key_window() == nil) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
		    dispatch_get_main_queue(), ^{
			rwb_try_start(attempt + 1);
		});
		return;
	}
	if ([overlay start])
		return;
	if (overlay.failed)
		return;                 /* the reason is already on the console */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
	    dispatch_get_main_queue(), ^{
		rwb_try_start(attempt + 1);
	});
}

void
redwood_vk_overlay_maybe_start(void)
{
	/*
	 * SpringBoard's stdio is not the console: its standard output goes
	 * nowhere unless inject_sb.c points it somewhere, and our probe writes
	 * to stderr. Point both at /dev/console before the first line, or a
	 * failed boot reports nothing at all.
	 */
	int console = open("/dev/console", O_WRONLY | O_NOCTTY);

	if (console >= 0) {
		dup2(console, STDERR_FILENO);
		if (console != STDERR_FILENO)
			close(console);
	}
	setvbuf(stderr, NULL, _IONBF, 0);
	if (!rwb_wanted())
		return;
	if (rwb_boot_arg("vkbridge_nowait=1")) {
		rw_wsi_wait_for_present(0);
		rwb_say("present wait disabled (vkbridge_nowait=1)");
	}
	rwb_say("armed (pid %d); waiting for SpringBoard's window", getpid());
	/*
	 * SpringBoard's UI is not up when this constructor runs; the first window
	 * appears several seconds later on this port. Poll on the main queue.
	 */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5 * NSEC_PER_SEC)),
	    dispatch_get_main_queue(), ^{
		rwb_try_start(0);
	});
}
