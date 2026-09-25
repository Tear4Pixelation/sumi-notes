// agent-pointer - absolute pointer injection over zwlr_virtual_pointer_v1.
//
// Exists because wlrctl(1), the usual tool for this protocol, exposes only
// relative `move` and a click that cannot be decomposed into press/release.
// Write is a drawing application: a stroke *is* a button-down, a path of motion
// events, and a button-up, so a tool that cannot hold a button down cannot
// exercise the app at all. The protocol itself has both absolute motion and
// button state; only wlrctl's CLI lacks them.
//
// Absolute coordinates matter for the same practical reason: the caller is
// reading pixel positions off a screenshot, and relative motion would mean
// tracking cursor state across process invocations.
//
// `click` takes coordinates rather than relying on a previous `move`: each run
// of this program creates a virtual pointer and destroys it on exit, and the
// cursor position does not survive that. A separate `move` then `click` clicks
// wherever the cursor happened to default to, which looks like "clicks are
// ignored" - toolbar buttons never fire while canvas drags work, because a drag
// is one invocation. Keep position and button in the same run.
//
// Usage:
//   agent-pointer move X Y
//   agent-pointer click [X Y] [left|right|middle]
//   agent-pointer press|release [left|right|middle]
//   agent-pointer drag X1 Y1 X2 Y2 [steps] [left|right|middle]
//   agent-pointer stroke X1 Y1 X2 Y2 [X3 Y3 ...]  (@MS between points: pause with the button held)
//   agent-pointer scroll [X Y] DY [DX]   (DY/DX in wheel notches)
//   agent-pointer hold X Y               (keep a pointer alive until killed)
//
// Coordinates are pixels in the output's own space.

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat *seat = NULL;
static struct wl_output *output = NULL;
static struct zwlr_virtual_pointer_manager_v1 *pointer_manager = NULL;
static struct zwlr_virtual_pointer_v1 *pointer = NULL;
static uint32_t screen_width = 0, screen_height = 0;

// Motion events between a press and a release need to be spread over time or
// the compositor coalesces them and the application sees a teleport rather than
// a drag. 8ms is roughly a 120Hz frame.
#define STEP_DELAY_MS 8
#define DEFAULT_DRAG_STEPS 24

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void sleep_ms(long ms) {
	struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
	nanosleep(&ts, NULL);
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh) {
	(void)data; (void)wl_output; (void)refresh;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		screen_width = (uint32_t)width;
		screen_height = (uint32_t)height;
	}
}
static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
		int32_t pw, int32_t ph, int32_t sub, const char *make, const char *model,
		int32_t tr) {
	(void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)sub;(void)make;(void)model;(void)tr;
}
static void output_done(void *d, struct wl_output *o) { (void)d;(void)o; }
static void output_scale(void *d, struct wl_output *o, int32_t f) { (void)d;(void)o;(void)f; }

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	(void)data; (void)version;
	if (strcmp(interface, wl_seat_interface.name) == 0 && !seat) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0 && !output) {
		output = wl_registry_bind(registry, name, &wl_output_interface, 2);
		wl_output_add_listener(output, &output_listener, NULL);
	} else if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		pointer_manager = wl_registry_bind(registry, name,
			&zwlr_virtual_pointer_manager_v1_interface, 1);
	}
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n) {
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static uint32_t button_code(const char *name) {
	if (!name || strcmp(name, "left") == 0) return BTN_LEFT;
	if (strcmp(name, "right") == 0) return BTN_RIGHT;
	if (strcmp(name, "middle") == 0) return BTN_MIDDLE;
	fprintf(stderr, "agent-pointer: unknown button '%s'\n", name);
	exit(1);
}

// motion_absolute takes integer x/y scaled against an integer extent, so
// passing the output size as the extent would round every position to a whole
// pixel - and the app under test is being checked for exactly that defect.
// Scale both sides by SUBPIXEL so the compositor receives 1/256 px positions,
// the same resolution wl_fixed carries to the client.
#define SUBPIXEL 256
static void do_move(double x, double y) {
	zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(),
		(uint32_t)(x * SUBPIXEL), (uint32_t)(y * SUBPIXEL),
		screen_width * SUBPIXEL, screen_height * SUBPIXEL);
	zwlr_virtual_pointer_v1_frame(pointer);
}

static void do_button(uint32_t code, bool pressed) {
	zwlr_virtual_pointer_v1_button(pointer, now_ms(), code,
		pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(pointer);
}

static void flush(struct wl_display *display) { wl_display_flush(display); }

// A wheel notch.  The continuous value is what a touchpad-aware client reads
// and the discrete value is what becomes an X11 button press; libinput's
// convention is 15 units per notch, so send both and keep them consistent.
#define WHEEL_UNITS_PER_NOTCH 15.0

static void do_wheel(uint32_t axis, double notches) {
	zwlr_virtual_pointer_v1_axis_discrete(pointer, now_ms(), axis,
		wl_fixed_from_double(notches * WHEEL_UNITS_PER_NOTCH), (int32_t)notches);
}

// Distinguishes a coordinate from a button name in the positional arguments.
static bool is_number(const char *s) {
	if (!s || !*s) return false;
	if (*s == '-' || *s == '+') s++;
	return *s >= '0' && *s <= '9';
}

// Interpolate between two points, emitting one motion per step. Used for both
// drag and stroke so a drawing app receives a continuous path.
static void glide(struct wl_display *display, double x1, double y1,
		double x2, double y2, int steps) {
	for (int i = 1; i <= steps; i++) {
		double t = (double)i / steps;
		do_move(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t);
		flush(display);
		sleep_ms(STEP_DELAY_MS);
	}
}

static void usage(void) {
	fprintf(stderr,
		"usage: agent-pointer <action> ...\n"
		"  move X Y\n"
		"  click [X Y] [left|right|middle]\n"
		"  press|release [left|right|middle]\n"
		"  drag X1 Y1 X2 Y2 [steps] [left|right|middle]\n"
		"  stroke X1 Y1 X2 Y2 [X3 Y3 ...]   (@MS: pause, button held)\n"
		"  scroll [X Y] DY [DX]\n"
		"  hold X Y                (keep a pointer alive until killed)\n");
	exit(1);
}

int main(int argc, char **argv) {
	if (argc < 2) usage();

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "agent-pointer: cannot connect to WAYLAND_DISPLAY=%s\n",
			getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);   // bind globals
	wl_display_roundtrip(display);   // let wl_output report its mode

	if (!pointer_manager) {
		fprintf(stderr, "agent-pointer: compositor does not support "
			"zwlr_virtual_pointer_manager_v1\n");
		return 1;
	}
	if (screen_width == 0 || screen_height == 0) {
		// motion_absolute needs an extent to scale against; without a mode we
		// would silently map every coordinate to the top-left corner.
		fprintf(stderr, "agent-pointer: no output mode reported; cannot map "
			"absolute coordinates\n");
		return 1;
	}

	pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
		pointer_manager, seat);

	const char *action = argv[1];

	if (strcmp(action, "move") == 0) {
		if (argc < 4) usage();
		do_move(atof(argv[2]), atof(argv[3]));

	} else if (strcmp(action, "click") == 0) {
		// "click X Y [button]" or "click [button]".
		const char *btn = NULL;
		if (argc >= 4 && argv[2][0] >= '0' && argv[2][0] <= '9') {
			do_move(atof(argv[2]), atof(argv[3]));
			flush(display);
			// The application needs to process the motion before the button, or
			// a widget that tracks a hovered item never sees the cursor arrive.
			sleep_ms(40);
			btn = argc > 4 ? argv[4] : NULL;
		} else {
			btn = argc > 2 ? argv[2] : NULL;
		}
		uint32_t code = button_code(btn);
		do_button(code, true);
		flush(display);
		sleep_ms(30);
		do_button(code, false);

	} else if (strcmp(action, "press") == 0) {
		do_button(button_code(argc > 2 ? argv[2] : NULL), true);

	} else if (strcmp(action, "release") == 0) {
		do_button(button_code(argc > 2 ? argv[2] : NULL), false);

	} else if (strcmp(action, "drag") == 0) {
		if (argc < 6) usage();
		double x1 = atof(argv[2]), y1 = atof(argv[3]);
		double x2 = atof(argv[4]), y2 = atof(argv[5]);
		// Trailing args are "[steps] [button]" in either order-free form: a
		// number is the step count, a name is the button.  Middle-button drag
		// is how canvas panning is exercised, so it has to be expressible.
		int steps = DEFAULT_DRAG_STEPS;
		uint32_t code = BTN_LEFT;
		for (int i = 6; i < argc; i++) {
			if (is_number(argv[i])) steps = atoi(argv[i]);
			else code = button_code(argv[i]);
		}
		if (steps < 1) steps = 1;
		do_move(x1, y1);
		flush(display);
		sleep_ms(30);
		do_button(code, true);
		flush(display);
		sleep_ms(30);
		glide(display, x1, y1, x2, y2, steps);
		do_button(code, false);

	} else if (strcmp(action, "stroke") == 0) {
		// Polyline drag: press at the first point, glide through the rest.  An
		// "@MS" token pauses that long with the button still down, which is how
		// Write's hold-to-snap is triggered (a held pen sends no motion at all).
		int n = 0;
		for (int i = 2; i < argc; i++)
			if (argv[i][0] != '@') n++;
		if (n < 4 || n % 2 != 0 || argv[2][0] == '@') usage();
		double px = atof(argv[2]), py = atof(argv[3]);
		do_move(px, py);
		flush(display);
		sleep_ms(30);
		do_button(BTN_LEFT, true);
		flush(display);
		sleep_ms(30);
		for (int i = 4; i < argc; i += 2) {
			if (argv[i][0] == '@') {
				flush(display);
				sleep_ms(atoi(argv[i] + 1));
				i -= 1;  // a pause is one token, not a pair
				continue;
			}
			if (i + 1 >= argc) usage();
			double nx = atof(argv[i]), ny = atof(argv[i + 1]);
			// One step per pixel of travel, so a stroke arrives at the density a
			// real pen delivers (1-2 px per sample) whatever the waypoint spacing.
			// A fixed step count either flooded a short segment with sub-pixel
			// steps that the X server coalesces into one motion, or starved a
			// long one.
			double dist = hypot(nx - px, ny - py);
			int steps = dist < 1.0 ? 1 : (int)ceil(dist);
			glide(display, px, py, nx, ny, steps);
			px = nx; py = ny;
		}
		do_button(BTN_LEFT, false);

	} else if (strcmp(action, "hold") == 0) {
		// Park a virtual pointer at X Y and keep it alive until killed.
		//
		// Every other action destroys its pointer on exit, which leaves the
		// compositor - and therefore Xwayland - with no pointer at all.  Write
		// runs as an X11 client, so once that happens XTEST (xdotool) has
		// nothing to drive: keystrokes still arrive but wheel and motion do
		// not, and Xwayland logs an enter/leave count assertion.  Holding one
		// pointer for the life of the session keeps a pointer always present.
		if (argc < 4) usage();
		do_move(atof(argv[2]), atof(argv[3]));
		flush(display);
		wl_display_roundtrip(display);
		for (;;) sleep_ms(3600000);

	} else if (strcmp(action, "scroll") == 0) {
		// "scroll X Y DY [DX]" or "scroll DY [DX]".  DY/DX are wheel notches.
		//
		// Position belongs in the same invocation for the reason `click` does,
		// and doubly so here: Xwayland delivers the wheel to whatever is under
		// the pointer, and an application reading the wheel against its last
		// known pointer position (as Write does) sees the default corner.
		//
		// axis_discrete is what makes this a *wheel* rather than a touchpad
		// swipe.  Xwayland turns discrete steps into X11 buttons 4-7; a bare
		// axis event with no discrete step is a continuous scroll and an X11
		// client - which Write is, inside cage - never sees it at all.
		int i = 2;
		if (argc >= 5 && is_number(argv[2]) && is_number(argv[3]) && is_number(argv[4])) {
			do_move(atof(argv[2]), atof(argv[3]));
			flush(display);
			sleep_ms(40);
			i = 4;
		}
		if (argc <= i) usage();
		double dy = atof(argv[i]);
		double dx = argc > i + 1 ? atof(argv[i + 1]) : 0.0;
		zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
		if (dy != 0.0)
			do_wheel(WL_POINTER_AXIS_VERTICAL_SCROLL, dy);
		if (dx != 0.0)
			do_wheel(WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx);
		zwlr_virtual_pointer_v1_frame(pointer);

	} else {
		usage();
	}

	wl_display_flush(display);
	wl_display_roundtrip(display);
	zwlr_virtual_pointer_v1_destroy(pointer);
	wl_display_disconnect(display);
	return 0;
}
