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

@interface RWVKBOverlay : NSObject
@property(nonatomic, strong) CAMetalLayer *layer;
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@property(nonatomic, strong) CADisplayLink *link;
@end

@implementation RWVKBOverlay {
	struct rw_wsi *_wsi;
	struct rwb *_bridge;
	uint64_t _frame;
	double _last_time;
	double _intervals[RWVKB_INTERVALS];
	unsigned _interval_count;
	unsigned _interval_next;
}

- (void)fail:(const char *)why
{
	rwb_say("FAILED: %s", why != NULL ? why : "(no sentence)");
	[self.link invalidate];
	self.link = nil;
}

- (BOOL)start
{
	char why[192];
	UIWindow *window = nil;

	for (UIWindow *candidate in [UIApplication sharedApplication].windows)
		if (candidate.isKeyWindow) {
			window = candidate;
			break;
		}
	if (window == nil)
		window = [UIApplication sharedApplication].windows.firstObject;
	if (window == nil) {
		rwb_say("no SpringBoard window yet");
		return NO;
	}
	CALayer *host = window.rootViewController.view.layer ?: window.layer;
	if (host == nil) {
		rwb_say("the key window has no layer");
		return NO;
	}
	self.device = rwb_make_device();
	if (self.device == nil)
		return NO;
	self.queue = [self.device newCommandQueue];
	if (self.queue == nil) {
		rwb_say("FAILED: the device gave no command queue");
		return NO;
	}
	self.layer = [CAMetalLayer layer];
	self.layer.device = self.device;
	self.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	self.layer.framebufferOnly = NO;
	self.layer.contentsScale = host.contentsScale > 0 ? host.contentsScale : 1.0;
	self.layer.frame = CGRectMake(0, 0, host.bounds.size.width, RWVKB_BAND_HEIGHT);
	self.layer.zPosition = 10000;
	[host addSublayer:self.layer];
	rwb_say("layer added: %.0fx%.0f at scale %.1f",
	    self.layer.frame.size.width, self.layer.frame.size.height,
	    self.layer.contentsScale);

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
	self.link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
	if (@available(iOS 15.0, *))
		self.link.preferredFrameRateRange = CAFrameRateRangeMake(60, 60, 60);
	else
		self.link.preferredFramesPerSecond = 60;
	[self.link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
	rwb_say("display link started");
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
rwb_wanted(void)
{
	char args[2048];
	size_t length = sizeof(args);

	if (sysctlbyname("kern.bootargs", args, &length, NULL, 0) == 0) {
		args[sizeof(args) - 1] = '\0';
		if (strstr(args, "vkbridge=1") != NULL) {
			rwb_say("wanted by kern.bootargs");
			return YES;
		}
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
	if (([UIApplication sharedApplication].windows.count > 0) && [overlay start])
		return;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
	    dispatch_get_main_queue(), ^{
		rwb_try_start(attempt + 1);
	});
}

void
redwood_vk_overlay_maybe_start(void)
{
	if (!rwb_wanted())
		return;
	setvbuf(stderr, NULL, _IONBF, 0);
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
