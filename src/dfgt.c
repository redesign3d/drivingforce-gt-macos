/*
 * dfgt.c — user-space driver for the Logitech Driving Force GT (046d:c29a) on macOS.
 *
 * No kext, no DriverKit, no entitlements, no root. Opens the HID device *shared*
 * (kIOHIDOptionsTypeNone) so macOS's own joystick support keeps working.
 *
 *   dfgt probe     [--cmd HEX]        element dump + transport self-test
 *   dfgt watch     [--hex] [N]        live decoded input (the control-identification tool)
 *   dfgt ffb       constant <−128..127> | off | autocenter <0..65535> | autocenter-off
 *   dfgt range     <40..900>
 *   dfgt native                       switch to native mode (no-op if already native)
 *   dfgt daemon    [--range D] [--autocenter M] [--socket P] [--verbose]
 *   dfgt status    [--socket P]
 *   dfgt selftest  [--fixture FILE]   decoder + command-builder regression checks
 *
 * FFB commands go to the 7-byte vendor output report (0xFF00/0x02), report ID 0.
 * Protocol bytes: Linux drivers/hid/hid-lg4ff.c (see PLAN.md Appendix A).
 */

#include <IOKit/hid/IOHIDLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DFGT_VID	0x046d
#define DFGT_PID	0xc29a

#define DFGT_CENTER	8192
#define DFGT_AXIS_MAX	16383
#define DFGT_CMD_LEN	7
#define DFGT_RANGE_MIN	40
#define DFGT_RANGE_MAX	900

#define DFGT_RANGE_DEFAULT	900
#define DFGT_AUTOCENTER_DEFAULT	0xaaaa	/* 0xaaaa = ~50% spring; 43690 */

#define DFGT_MAX_CONNS	8

/* ------------------------------------------------------------------ decode */

struct dfgt_state {
	int		valid;
	uint8_t		raw[8];
	unsigned	hat;		/* 0..7 direction, 8 = centred (null) */
	unsigned	buttons;	/* 21-bit mask, bit o = button o (1-based) */
	unsigned	vendor7;	/* raw[3][7:1] — live, meaning TBD (PLAN M1) */
	unsigned	vendor2;	/* raw[5][7:6] */
	unsigned	steer;		/* 0..16383, centre 8192 */
	unsigned	throttle;	/* raw, 255 = released */
	unsigned	brake;		/* raw, 255 = released */
	uint64_t	t_ms;
};

static void dfgt_decode(const uint8_t *r, struct dfgt_state *s)
{
	s->hat      = r[0] & 0x0f;
	s->buttons  = ((unsigned)(r[0] >> 4) | ((unsigned)r[1] << 4) |
	               ((unsigned)r[2] << 12) | ((unsigned)(r[3] & 1) << 20)) & 0x1fffff;
	s->vendor7  = (r[3] >> 1) & 0x7f;
	s->steer    = (unsigned)r[4] | (((unsigned)r[5] & 0x3f) << 8);
	s->vendor2  = (r[5] >> 6) & 0x03;
	s->throttle = r[6];
	s->brake    = r[7];
	memcpy(s->raw, r, 8);
	s->valid = 1;
}

/* Control names, verified by pressing every control one at a time on the wheel
 * and confirmed against two captures (tests/capture-controls.hex,
 * tests/capture-turn-replug.hex). Index o = buttons bit o. */
static const char *const dfgt_button_names[21] = {
	"x", "square", "circle", "triangle",		/* 1-4   face buttons */
	"r1", "l1", "r2", "l2",			/* 5-8   shoulder (right first in each pair) */
	"select", "start", "r3", "l3",		/* 9-12 */
	"shift-up", "shift-down", "dial-press",	/* 13-15 */
	"rocker-plus", "dial-cw", "dial-ccw",	/* 16-18 */
	"rocker-minus", "horn", "ps",			/* 19-21 */
};

/* hat switch: 0=N, 2=E, 4=S, 6=W, odd = diagonals, 8 = centred (null state) */
static const char *hat_name(unsigned hat)
{
	static const char *const n[9] = { "up", "up-right", "right", "down-right",
	                                  "down", "down-left", "left", "up-left", "centred" };
	return hat < 9 ? n[hat] : "?";
}

/* ------------------------------------------------------------ command bytes */

static void cmd_force_off(uint8_t c[DFGT_CMD_LEN])
{
	const uint8_t v[DFGT_CMD_LEN] = { 0x13, 0, 0, 0, 0, 0, 0 };
	memcpy(c, v, DFGT_CMD_LEN);
}

static void cmd_constant(int level, uint8_t c[DFGT_CMD_LEN])
{
	if (level > 127)  level = 127;
	if (level < -128) level = -128;
	if (level == 0) { cmd_force_off(c); return; }
	c[0] = 0x11;			/* slot 1 */
	c[1] = 0x08;
	c[2] = (uint8_t)(level + 0x80);	/* 0x80 = no force */
	c[3] = 0x80;
	c[4] = c[5] = c[6] = 0;
}

static void cmd_autocenter(unsigned m, uint8_t c[DFGT_CMD_LEN])
{
	uint32_t ea, eb;

	if (m > 0xffff) m = 0xffff;
	if (m == 0) {
		const uint8_t off[DFGT_CMD_LEN] = { 0xf5, 0, 0, 0, 0, 0, 0 };
		memcpy(c, off, DFGT_CMD_LEN);
		return;
	}
	if (m <= 0xaaaa) {
		ea = 0x0c * m;
		eb = 0x80 * m;
	} else {
		ea = 0x0c * 0xaaaa + 0x06 * (m - 0xaaaa);
		eb = 0x80 * 0xaaaa + 0xff * (m - 0xaaaa);
	}
	ea >>= 1;				/* non-MOMO wheels */
	c[0] = 0xfe;
	c[1] = 0x0d;
	c[2] = (uint8_t)(ea / 0xaaaa);
	c[3] = (uint8_t)(ea / 0xaaaa);
	c[4] = (uint8_t)(eb / 0xaaaa);
	c[5] = c[6] = 0;
}

static void cmd_autocenter_activate(uint8_t c[DFGT_CMD_LEN])
{
	const uint8_t v[DFGT_CMD_LEN] = { 0x14, 0, 0, 0, 0, 0, 0 };
	memcpy(c, v, DFGT_CMD_LEN);
}

static void cmd_autocenter_off(uint8_t c[DFGT_CMD_LEN])
{
	const uint8_t v[DFGT_CMD_LEN] = { 0xf5, 0, 0, 0, 0, 0, 0 };
	memcpy(c, v, DFGT_CMD_LEN);
}

static void cmd_range(unsigned deg, uint8_t c[DFGT_CMD_LEN])
{
	if (deg < DFGT_RANGE_MIN) deg = DFGT_RANGE_MIN;
	if (deg > DFGT_RANGE_MAX) deg = DFGT_RANGE_MAX;
	c[0] = 0xf8;
	c[1] = 0x81;
	c[2] = (uint8_t)(deg & 0x00ff);
	c[3] = (uint8_t)((deg & 0xff00) >> 8);
	c[4] = c[5] = c[6] = 0;
}

static void cmd_mode_revert_marker(uint8_t c[DFGT_CMD_LEN])
{
	const uint8_t v[DFGT_CMD_LEN] = { 0xf8, 0x0a, 0, 0, 0, 0, 0 };
	memcpy(c, v, DFGT_CMD_LEN);
}

/* ------------------------------------------------------------------ personas
 *
 * Logitech's multimode wheels can be told to re-enumerate as a different model: first
 * "revert mode upon USB reset" (f8 0a …, so a replug is always an escape), then
 * f8 09 <idx> 01 …, after which the device detaches and comes back with the other
 * model's USB PID (Linux identifies the current mode from the PID alone).
 * Bytes from Linux hid-lg4ff.c (lg4ff_mode_switch_ext09_*). The kernel only offers
 * DF-EX / DFP / DFGT for a DFGT; the newer ones are probes. */
struct dfgt_persona {
	const char	*name;
	unsigned	 idx;
	unsigned	 expect_pid;
	const char	*note;
};
static const struct dfgt_persona dfgt_personas[] = {
	{ "dfex", 0x00, 0xc294, "Driving Force (DF-EX): combined pedals, reduced range" },
	{ "dfp",  0x01, 0xc298, "Driving Force Pro" },
	{ "g25",  0x02, 0xc299, "G25" },
	{ "dfgt", 0x03, 0xc29a, "Driving Force GT - native, what this driver targets" },
	{ "g27",  0x04, 0xc29b, "G27" },
	{ "g29",  0x05, 0xc24f, "G29 - the identity GeForce NOW whitelists" },
};
#define DFGT_PERSONA_DFGT	3

static void cmd_mode_switch(unsigned idx, uint8_t c[DFGT_CMD_LEN])
{
	c[0] = 0xf8;
	c[1] = 0x09;
	c[2] = (uint8_t)idx;
	c[3] = 0x01;
	c[4] = (idx == 0x05) ? 0x01 : 0x00;	/* the G29 command carries an extra byte */
	c[5] = 0x00;
	c[6] = 0x00;
}

/* ----------------------------------------------------------------- helpers */

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

static int steer_deg(int steer, unsigned range)
{
	/* unitless axis mapped across the configured range */
	return (int)(((long)((int)steer - DFGT_CENTER) * (long)range) / DFGT_AXIS_MAX);
}

/* --------------------------------------------------------------- HID session */

struct conn {
	int	fd;
	char	buf[512];
	size_t	len;
};

struct dfgt_ctx {
	IOHIDManagerRef	mgr;
	IOHIDDeviceRef	dev;
	uint8_t		inbuf[64];
	struct dfgt_state st;

	unsigned	range;
	unsigned	autocenter;
	unsigned	vid, pid;	/* identity to match: personas change the PID */
	int		addr_override;	/* --vid/--pid given: never route through the daemon */

	int		listen_fd;
	struct conn	conns[DFGT_MAX_CONNS];
	const char	*sock_path;
	int		verbose;
	int		quiet;
	int		writable;	/* 0 = read-only: never touch wheel settings */
	int		quiet_report;	/* watch --hex: raw bytes only */
	int		quiet_startup;	/* don't log device ready/removed */
	volatile sig_atomic_t stop;
};

static struct dfgt_ctx C;

static void say(const char *fmt, ...)
{
	va_list ap;
	if (C.quiet) return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static const char *sock_path_default(void)
{
	const char *e = getenv("DFGT_SOCKET");
	return (e && *e) ? e : "/tmp/dfgt-daemon.sock";
}

static int dfgt_send(struct dfgt_ctx *c, const uint8_t cmd[DFGT_CMD_LEN], const char *what)
{
	uint8_t buf[DFGT_CMD_LEN];
	IOReturn r;

	if (!c->dev) { say("dfgt: no device"); return -1; }
	memcpy(buf, cmd, DFGT_CMD_LEN);
	r = IOHIDDeviceSetReport(c->dev, kIOHIDReportTypeOutput, 0, buf, DFGT_CMD_LEN);
	if (r != kIOReturnSuccess) {
		say("dfgt: %s failed (0x%08x)", what, (unsigned)r);
		return -1;
	}
	if (c->verbose) {
		printf("> %-22s %02x %02x %02x %02x %02x %02x %02x\n", what,
		       cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5], cmd[6]);
		fflush(stdout);
	}
	return 0;
}

static int send_built(struct dfgt_ctx *c, void (*build)(uint8_t *), const char *what)
{
	uint8_t cmd[DFGT_CMD_LEN];

	build(cmd);
	return dfgt_send(c, cmd, what);
}

static const char *ok_payload(const char *reply)
{
	return (reply[0] == 'o' && reply[1] == 'k' && reply[2] == ' ') ? reply + 3 : reply;
}

/* ------------------------------------------------------------------ sockets */

static int sock_listen(const char *path)
{
	int fd;
	struct sockaddr_un sa;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 8) < 0) {
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

static int sock_connect(const char *path)
{
	int fd;
	struct sockaddr_un sa;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Formats the state line, without newline. */
static void state_line(const struct dfgt_state *s, unsigned range, char *out, size_t n)
{
	char pressed[160];
	size_t p = 0;

	pressed[0] = 0;
	for (int b = 0; b < 21; b++) {
		if (!(s->buttons & (1u << b))) continue;
		if (p >= sizeof pressed - 8) break;
		p += (size_t)snprintf(pressed + p, sizeof pressed - p, "%s%s", p ? "," : "",
		                      dfgt_button_names[b]);
	}
	snprintf(out, n,
	         "steer=%u steer_deg=%d throttle_raw=%u throttle=%u brake_raw=%u brake=%u "
	         "hat=%u hat_dir=%s buttons=0x%06x pressed=%s vendor7=0x%02x vendor2=%u t=%llu",
	         s->steer, steer_deg((int)s->steer, range),
	         s->throttle, s->throttle >= 255 ? 0u : (255u - s->throttle) * 100u / 255u,
	         s->brake, s->brake >= 255 ? 0u : (255u - s->brake) * 100u / 255u,
	         s->hat, hat_name(s->hat), s->buttons, pressed[0] ? pressed : "-",
	         s->vendor7, s->vendor2, (unsigned long long)s->t_ms);
}

static void broadcast(struct dfgt_ctx *c, const char *line)
{
	for (int i = 0; i < DFGT_MAX_CONNS; i++) {
		struct conn *k = &c->conns[i];
		if (k->fd < 0) continue;
		if (write(k->fd, line, strlen(line)) < 0 || write(k->fd, "\n", 1) < 0) {
			close(k->fd);
			k->fd = -1;
		}
	}
}

static void publish_state(struct dfgt_ctx *c)
{
	char line[512];

	if (!c->st.valid) return;
	state_line(&c->st, c->range, line, sizeof line);
	broadcast(c, line);
}

static void handle_command(struct dfgt_ctx *c, char *line, char *reply, size_t rn)
{
	unsigned u;
	int v;
	uint8_t cmd[DFGT_CMD_LEN];

	while (*line == ' ' || *line == '\t') line++;
	if (!*line) return;

	if (!strcmp(line, "state")) {
		char s[400];
		if (!c->st.valid) { snprintf(reply, rn, "err no-input-yet"); return; }
		state_line(&c->st, c->range, s, sizeof s);
		snprintf(reply, rn, "ok %s", s);
		return;
	}
	if (!strcmp(line, "ping")) { snprintf(reply, rn, "ok pong"); return; }

	if (sscanf(line, "range %u", &u) == 1) {
		cmd_range(u, cmd);
		if (dfgt_send(c, cmd, "range") == 0) {
			unsigned r = u;
			if (r < DFGT_RANGE_MIN) r = DFGT_RANGE_MIN;
			if (r > DFGT_RANGE_MAX) r = DFGT_RANGE_MAX;
			c->range = r;
			snprintf(reply, rn, "ok range %u", r);
		} else snprintf(reply, rn, "err send-failed");
		return;
	}
	if (sscanf(line, "ffb constant %d", &v) == 1) {
		cmd_constant(v, cmd);
		if (dfgt_send(c, cmd, "constant") == 0) {
			snprintf(reply, rn, "ok constant %d", v);
		} else snprintf(reply, rn, "err send-failed");
		return;
	}
	if (!strcmp(line, "ffb off")) {
		cmd_force_off(cmd);
		if (dfgt_send(c, cmd, "force off") == 0) {
			snprintf(reply, rn, "ok force-off");
		} else snprintf(reply, rn, "err send-failed");
		return;
	}
	if (sscanf(line, "ffb autocenter %u", &u) == 1) {
		cmd_autocenter(u, cmd);
		if (dfgt_send(c, cmd, "autocenter") != 0) { snprintf(reply, rn, "err send-failed"); return; }
		cmd_autocenter_activate(cmd);
		if (dfgt_send(c, cmd, "autocenter on") != 0) { snprintf(reply, rn, "err send-failed"); return; }
		c->autocenter = u;
		snprintf(reply, rn, "ok autocenter %u", u);
		return;
	}
	if (!strcmp(line, "ffb autocenter-off")) {
		cmd_autocenter_off(cmd);
		if (dfgt_send(c, cmd, "autocenter off") == 0) {
			c->autocenter = 0;
			snprintf(reply, rn, "ok autocenter-off");
		} else snprintf(reply, rn, "err send-failed");
		return;
	}
	snprintf(reply, rn, "err unknown-command");
}

static void poll_socket(struct dfgt_ctx *c)
{
	int fd;
	struct conn *k;

	for (;;) {
		fd = accept(c->listen_fd, NULL, NULL);
		if (fd < 0) break;
		fcntl(fd, F_SETFL, O_NONBLOCK);
		for (int i = 0; i < DFGT_MAX_CONNS; i++) {
			if (c->conns[i].fd >= 0) continue;
			c->conns[i].fd = fd;
			c->conns[i].len = 0;
			fd = -1;
			break;
		}
		if (fd >= 0) close(fd);		/* too many clients */
	}
	/* commands from clients; state is streamed on every report and on request */
	for (int i = 0; i < DFGT_MAX_CONNS; i++) {
		k = &c->conns[i];
		if (k->fd < 0) continue;
		for (;;) {
			ssize_t n = read(k->fd, k->buf + k->len, sizeof k->buf - k->len - 1);
			if (n < 0) break;	/* EAGAIN */
			if (n == 0) { close(k->fd); k->fd = -1; k->len = 0; break; }
			k->len += (size_t)n;
			k->buf[k->len] = 0;
			for (;;) {
				char *nl = strchr(k->buf, '\n');
				char reply[512];
				size_t rest;
				if (!nl) break;
				*nl = 0;
				reply[0] = 0;
				handle_command(c, k->buf, reply, sizeof reply);
				if (reply[0]) {
					char out[600];
					snprintf(out, sizeof out, "%s\n", reply);
					if (write(k->fd, out, strlen(out)) < 0) { /* ignore */ }
				}
				rest = k->len - (size_t)(nl + 1 - k->buf);
				memmove(k->buf, nl + 1, rest);
				k->len = rest;
				k->buf[k->len] = 0;
			}
			if (k->len >= sizeof k->buf - 1) k->len = 0;	/* oversized line: drop */
		}
	}
}

/* ------------------------------------------------------------- HID callbacks */

static void on_input_report(void *ctx, IOReturn res, void *sender, IOHIDReportType type,
                            uint32_t rid, uint8_t *report, CFIndex len)
{
	struct dfgt_ctx *c = ctx;
	struct dfgt_state prev = c->st;
	char changed[128] = { 0 };
	size_t p = 0;
	char line[640];

	(void)sender; (void)type; (void)rid;
	if (res != kIOReturnSuccess || len < 8) return;

	dfgt_decode(report, &c->st);
	c->st.t_ms = now_ms();
	if (c->st.valid && prev.valid) {
		struct { const char *n; unsigned a, b; } f[] = {
			{ "hat",     prev.hat,      c->st.hat },
			{ "buttons", prev.buttons,  c->st.buttons },
			{ "vendor7", prev.vendor7,  c->st.vendor7 },
			{ "vendor2", prev.vendor2,  c->st.vendor2 },
			{ "steer",   prev.steer,    c->st.steer },
			{ "throttle",prev.throttle, c->st.throttle },
			{ "brake",   prev.brake,    c->st.brake },
		};
		for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
			if (f[i].a == f[i].b) continue;
			if (p >= sizeof changed - 8) break;
			p += (size_t)snprintf(changed + p, sizeof changed - p, "%s%s", p ? "," : "", f[i].n);
		}
		if (!p) return;		/* duplicate report: nothing actually changed */
	} else {
		snprintf(changed, sizeof changed, "first");
	}

	if (c->listen_fd >= 0) { publish_state(c); return; }	/* daemon: stream to clients */
	if (c->quiet_report == 1) {		/* watch --hex: fixtures */
		printf("%02x %02x %02x %02x %02x %02x %02x %02x\n",
		       report[0], report[1], report[2], report[3],
		       report[4], report[5], report[6], report[7]);
		fflush(stdout);
		return;
	}
	/* a second opener steals report delivery from the first, so the direct CLI must not
	 * print input, and commands must go through the daemon's socket whenever it runs */
	if (c->quiet_report == 2) return;
	state_line(&c->st, c->range, line, sizeof line);
	printf("Δ%-22s %s\n", changed, line);
	fflush(stdout);
}

static void on_matched(void *ctx, IOReturn res, void *sender, IOHIDDeviceRef dev)
{
	struct dfgt_ctx *c = ctx;
	uint8_t cmd[DFGT_CMD_LEN];

	(void)res; (void)sender;
	if (c->dev) {					/* stale ref from a previous plug */
		IOHIDDeviceUnscheduleFromRunLoop(c->dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
		CFRelease(c->dev);
	}
	c->dev = dev;
	CFRetain(c->dev);
	c->st.valid = 0;
	IOHIDDeviceRegisterInputReportCallback(c->dev, c->inbuf, sizeof c->inbuf,
	                                       on_input_report, c);
	IOHIDDeviceScheduleWithRunLoop(c->dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

	if (!c->quiet_startup) {
		if (c->writable)
			say("dfgt: device ready (range %u, autocenter 0x%04x)", c->range, c->autocenter);
		else
			say("dfgt: device ready (read-only)");
	}
	if (!c->writable) return;	/* watch/probe must not touch wheel settings */

	/* always neutralise first: a previous session may have left a force applied */
	send_built(c, cmd_force_off, "force off");
	send_built(c, cmd_autocenter_off, "autocenter off");
	if (c->range) {
		cmd_range(c->range, cmd);
		dfgt_send(c, cmd, "range");
	}
	if (c->autocenter) {
		cmd_autocenter(c->autocenter, cmd);
		dfgt_send(c, cmd, "autocenter");
		send_built(c, cmd_autocenter_activate, "autocenter on");
	}
}

static void on_removed(void *ctx, IOReturn res, void *sender, IOHIDDeviceRef dev)
{
	struct dfgt_ctx *c = ctx;

	(void)res; (void)sender;
	if (dev != c->dev) return;
	IOHIDDeviceUnscheduleFromRunLoop(c->dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	CFRelease(c->dev);
	c->dev = NULL;
	c->st.valid = 0;
	if (!c->quiet_startup) say("dfgt: device removed");
}

static int hid_start(struct dfgt_ctx *c)
{
	CFMutableDictionaryRef match;
	int vid = (int)c->vid, pid = (int)c->pid;
	CFNumberRef nv, np;

	c->mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
	                                  &kCFTypeDictionaryKeyCallBacks,
	                                  &kCFTypeDictionaryValueCallBacks);
	nv = CFNumberCreate(NULL, kCFNumberIntType, &vid);
	np = CFNumberCreate(NULL, kCFNumberIntType, &pid);
	CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), nv);
	CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), np);
	IOHIDManagerSetDeviceMatching(c->mgr, match);
	IOHIDManagerRegisterDeviceMatchingCallback(c->mgr, on_matched, c);
	IOHIDManagerRegisterDeviceRemovalCallback(c->mgr, on_removed, c);
	IOHIDManagerScheduleWithRunLoop(c->mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	if (IOHIDManagerOpen(c->mgr, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
		say("dfgt: IOHIDManagerOpen failed (HID access denied?)");
		return -1;
	}
	return 0;
}

/* Wait for the device to show up (matching callback needs the run loop). */
static int wait_for_device(struct dfgt_ctx *c, double seconds)
{
	double end = CFAbsoluteTimeGetCurrent() + seconds;
	while (!c->dev && CFAbsoluteTimeGetCurrent() < end)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
	return c->dev ? 0 : -1;
}

/* --------------------------------------------------------------- descriptor */

static int descriptor_is_native(IOHIDDeviceRef dev)
{
	CFDataRef d = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDReportDescriptorKey));
	const uint8_t *p;
	CFIndex n;
	/* native: X is 14-bit with logical max 16383 -> 26 ff 3f 46 ff 3f 75 0e */
	static const uint8_t pat[] = { 0x26, 0xff, 0x3f, 0x46, 0xff, 0x3f, 0x75, 0x0e };

	if (!d) return 0;
	p = CFDataGetBytePtr(d);
	n = CFDataGetLength(d);
	if (!p || n <= 0) return 0;
	for (CFIndex i = 0; i + (CFIndex)sizeof pat <= n; i++)
		if (!memcmp(p + i, pat, sizeof pat)) return 1;
	return 0;
}

/* ------------------------------------------------------------------- signals */

static void on_signal(int sig)
{
	(void)sig;
	C.stop = 1;
}

static void cleanup(void)
{
	if (!C.dev) return;
	send_built(&C, cmd_force_off, "force off");
	send_built(&C, cmd_autocenter_off, "autocenter off");
}

/* ---------------------------------------------------------------------- main */

static void dump_elements(IOHIDDeviceRef dev)
{
	CFArrayRef elems = IOHIDDeviceCopyMatchingElements(dev, NULL, kIOHIDOptionsTypeNone);
	CFIndex n;

	if (!elems) { printf("  (no elements)\n"); return; }
	n = CFArrayGetCount(elems);
	printf("  %ld elements:\n", (long)n);
	for (CFIndex i = 0; i < n; i++) {
		IOHIDElementRef e = (IOHIDElementRef)CFArrayGetValueAtIndex(elems, i);
		const char *t = "other";
		if (IOHIDElementGetReportCount(e) == 0) continue;
		switch (IOHIDElementGetType(e)) {
		case kIOHIDElementTypeInput_Misc:      t = "In-Misc"; break;
		case kIOHIDElementTypeInput_Button:    t = "In-Btn"; break;
		case kIOHIDElementTypeInput_Axis:      t = "In-Axis"; break;
		case kIOHIDElementTypeOutput:          t = "Output"; break;
		case kIOHIDElementTypeFeature:         t = "Feature"; break;
		default: break;
		}
		printf("    %-8s page=0x%04x usage=0x%02x rid=%ld size=%2ld count=%3ld log=[%ld,%ld]\n",
		       t, (unsigned)IOHIDElementGetUsagePage(e), (unsigned)IOHIDElementGetUsage(e),
		       (long)IOHIDElementGetReportID(e), (long)IOHIDElementGetReportSize(e),
		       (long)IOHIDElementGetReportCount(e),
		       (long)IOHIDElementGetLogicalMin(e), (long)IOHIDElementGetLogicalMax(e));
	}
	CFRelease(elems);
}

static const char *prop_str(IOHIDDeviceRef dev, CFStringRef key)
{
	static char buf[192];
	CFTypeRef p = IOHIDDeviceGetProperty(dev, key);

	if (!p) return "(none)";
	if (CFGetTypeID(p) == CFStringGetTypeID()) {
		if (!CFStringGetCString((CFStringRef)p, buf, sizeof buf, kCFStringEncodingUTF8))
			snprintf(buf, sizeof buf, "(unprintable)");
		return buf;
	}
	if (CFGetTypeID(p) == CFNumberGetTypeID()) {
		long v = 0;
		CFNumberGetValue((CFNumberRef)p, kCFNumberLongType, &v);
		snprintf(buf, sizeof buf, "%ld (0x%lx)", v, (unsigned long)v);
		return buf;
	}
	if (CFGetTypeID(p) == CFDataGetTypeID()) {
		snprintf(buf, sizeof buf, "<data, %ld bytes>", (long)CFDataGetLength((CFDataRef)p));
		return buf;
	}
	snprintf(buf, sizeof buf, "<cf type %lu>", (unsigned long)CFGetTypeID(p));
	return buf;
}

/* Read the current value of every input element: does not wait for a report, so it answers
   "what is the state right now" (and whether the device is alive at all). Useful for the real
   wheel too, where reports only arrive on change. */
static void print_values(IOHIDDeviceRef dev)
{
	CFArrayRef elems = IOHIDDeviceCopyMatchingElements(dev, NULL, kIOHIDOptionsTypeNone);

	printf("current values (read directly, not from a report):\n");
	for (CFIndex i = 0; elems && i < CFArrayGetCount(elems); i++) {
		IOHIDElementRef e = (IOHIDElementRef)CFArrayGetValueAtIndex(elems, i);
		IOHIDElementType t = IOHIDElementGetType(e);
		IOHIDValueRef v = NULL;

		if (IOHIDElementGetReportCount(e) == 0) continue;
		if (t != kIOHIDElementTypeInput_Misc && t != kIOHIDElementTypeInput_Button &&
		    t != kIOHIDElementTypeInput_Axis) continue;
		if (IOHIDDeviceGetValue(dev, e, &v) == kIOReturnSuccess && v) {
			printf("    page=0x%04x usage=0x%02x value=%ld\n",
			       (unsigned)IOHIDElementGetUsagePage(e),
			       (unsigned)IOHIDElementGetUsage(e),
			       (long)IOHIDValueGetIntegerValue(v));
			CFRelease(v);
		}
	}
	if (elems) CFRelease(elems);
}

static int cmd_probe(int argc, char **argv)
{
	uint8_t custom[DFGT_CMD_LEN];
	size_t custom_len = 0;
	int have_custom = 0;
	int want_values = 0;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--values")) {
			want_values = 1;
		} else if (!strcmp(argv[i], "--cmd") && i + 1 < argc) {
			const char *h = argv[++i];
			for (size_t j = 0; j + 1 < strlen(h) && custom_len < DFGT_CMD_LEN; j += 2) {
				unsigned b;
				if (sscanf(h + j, "%2x", &b) != 1) break;
				custom[custom_len++] = (uint8_t)b;
			}
			have_custom = custom_len == DFGT_CMD_LEN;
		}
	}

	C.verbose = 1;
	C.quiet_startup = 1;
	C.writable = 0;		/* read-only: probe never changes wheel settings */
	C.range = DFGT_RANGE_DEFAULT;
	C.autocenter = 0;
	if (hid_start(&C) < 0) return 1;
	if (wait_for_device(&C, 2.0) < 0) { say("dfgt: no wheel at %04x:%04x", C.vid, C.pid); return 1; }

	printf("device %04x:%04x%s\n", C.vid, C.pid,
	       descriptor_is_native(C.dev) ? " (native mode)" : " (compat mode?)");
	printf("report descriptor: 8B input (no report id), 7B vendor output 0xFF00/0x02, 131B feature\n");
	printf("identity as other clients (GeForce NOW, Steam, games) see it:\n");
	printf("  Product      = %s\n", prop_str(C.dev, CFSTR(kIOHIDProductKey)));
	printf("  VendorID     = %s\n", prop_str(C.dev, CFSTR(kIOHIDVendorIDKey)));
	printf("  ProductID    = %s\n", prop_str(C.dev, CFSTR(kIOHIDProductIDKey)));
	printf("  SerialNumber = %s\n", prop_str(C.dev, CFSTR("SerialNumber")));
	printf("  Transport    = %s\n", prop_str(C.dev, CFSTR(kIOHIDTransportKey)));
	if (want_values) print_values(C.dev);
	else dump_elements(C.dev);
	if (have_custom) {
		dfgt_send(&C, custom, "raw");
	} else {
		send_built(&C, cmd_force_off, "force off (self-test)");
	}
	return 0;
}

static int cmd_watch(int argc, char **argv)
{
	double seconds = 3600.0, end;
	int i;

	C.range = DFGT_RANGE_DEFAULT;	/* display only: watch is read-only */
	C.autocenter = 0;
	C.writable = 0;
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--hex")) C.quiet_report = 1;
		else if (argv[i][0] != '-') seconds = atof(argv[i]);
	}
	C.quiet_startup = 0;	/* log device add/remove, so the capture shows a replug */
	if (hid_start(&C) < 0) return 1;
	if (wait_for_device(&C, 2.0) < 0) { say("dfgt: no wheel at %04x:%04x", C.vid, C.pid); return 1; }
	if (!C.quiet_report) {
		printf("# move one control at a time; each line lists the fields that changed\n");
		printf("# steer: 0..16383 (8192 centre)  throttle/brake: 255 = released  hat: 8 = centred\n");
		printf("# vendor7/vendor2: raw bits of unknown purpose — press the dial, +/- and horn here\n");
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	end = CFAbsoluteTimeGetCurrent() + seconds;
	while (!C.stop && CFAbsoluteTimeGetCurrent() < end)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, false);
	return 0;
}

static int persona_index(const char *name)
{
	for (size_t i = 0; i < sizeof dfgt_personas / sizeof dfgt_personas[0]; i++)
		if (!strcmp(dfgt_personas[i].name, name)) return (int)i;
	return -1;
}

static int cmd_mode(const char *name, int force)
{
	uint8_t c[DFGT_CMD_LEN];
	int idx = persona_index(name);

	if (idx < 0) { say("dfgt: unknown persona '%s' (see: dfgt modes)", name); return 2; }
	if (!force && idx != 0 && idx != 1 && idx != DFGT_PERSONA_DFGT) {
		say("dfgt: the kernel does not offer '%s' for a DFGT (it may not exist in the "
		    "firmware). Re-run with --force to probe anyway.", name);
		return 2;
	}
	C.quiet_startup = 1;		/* writable stays 0: a mode switch must not touch settings */
	if (hid_start(&C) < 0) return 1;
	if (wait_for_device(&C, 2.0) < 0) {
		say("dfgt: no wheel at %04x:%04x (persona switched? use --pid)", C.vid, C.pid);
		return 1;
	}
	if (idx == DFGT_PERSONA_DFGT && descriptor_is_native(C.dev)) {
		printf("already in native mode (dfgt persona, 14-bit steering, separate pedals)\n");
		return 0;
	}
	printf("switching to '%s' (expects pid %04x); the wheel detaches and re-enumerates.\n",
	       name, dfgt_personas[idx].expect_pid);
	cmd_mode_revert_marker(c);
	if (dfgt_send(&C, c, "revert-marker")) return 1;
	cmd_mode_switch(dfgt_personas[idx].idx, c);
	if (dfgt_send(&C, c, "mode switch")) return 1;
	printf("sent. If it does not come back as %04x: replug the wheel (the marker makes a USB\n"
	       "reset revert the persona) or address the new identity with --pid 0x%04x.\n",
	       dfgt_personas[idx].expect_pid, dfgt_personas[idx].expect_pid);
	return 0;
}

static int cmd_modes(void)
{
	printf("personas: f8 09 <idx> 01 ... makes the wheel re-enumerate under a new USB PID\n");
	for (size_t i = 0; i < sizeof dfgt_personas / sizeof dfgt_personas[0]; i++)
		printf("  %-5s idx=0x%02x  pid %04x  %s%s\n", dfgt_personas[i].name,
		       dfgt_personas[i].idx, dfgt_personas[i].expect_pid, dfgt_personas[i].note,
		       (i == 0 || i == 1 || (int)i == DFGT_PERSONA_DFGT) ? "" : "   [needs --force]");
	printf("\nGeForce NOW whitelists 046d:c24f (G29), c262 (G920), c26e (G923) and the PRO wheel.\n"
	       "A 2007-era DFGT firmware is unlikely to carry a G29 persona; `dfgt mode g29` tests it.\n"
	       "See docs/geforce-now.md.\n");
	return 0;
}

static int cmd_native(void)
{
	return cmd_mode("dfgt", 1);
}

static int cmd_ffb_direct(const char *line)
{
	char reply[512] = { 0 };

	C.quiet_startup = 1;
	C.range = DFGT_RANGE_DEFAULT;
	if (hid_start(&C) < 0) return 1;
	if (wait_for_device(&C, 2.0) < 0) { say("dfgt: 046d:c29a not found"); return 1; }
	/* set exactly this effect and leave others alone; daemon startup is what neutralises */
	C.quiet_report = 2;		/* never open a second report stream */
	handle_command(&C, (char *)line, reply, sizeof reply);
	if (strncmp(reply, "ok", 2) != 0) { say("dfgt: %s", reply); return 1; }
	/* give the run loop a moment so the write completes before we exit */
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
	printf("%s\n", ok_payload(reply));
	return 0;
}

static int send_via_daemon(const char *sock, const char *line, char *reply, size_t rn)
{
	int fd = sock_connect(sock);
	ssize_t n;

	if (fd < 0) return -1;
	if (write(fd, line, strlen(line)) < 0 || write(fd, "\n", 1) < 0) { close(fd); return -1; }
	n = read(fd, reply, rn - 1);
	close(fd);
	if (n <= 0) { reply[0] = 0; return -1; }
	reply[n] = 0;
	return 0;
}

static int dispatch_control_command(int argc, char **argv)
{
	char line[256] = { 0 };
	char reply[512] = { 0 };
	const char *sock = sock_path_default();
	size_t p = 0;

	if (argc < 2) { say("dfgt: %s needs an argument (see dfgt help)", argv[0]); return 2; }

	for (int i = 0; i < 2 && i < argc; i++)
		p += (size_t)snprintf(line + p, sizeof line - p, "%s%s", p ? " " : "", argv[i]);
	for (int i = 2; i < argc; i++)
		p += (size_t)snprintf(line + p, sizeof line - p, " %s", argv[i]);

	/* An explicit identity (--vid/--pid) means the caller is addressing a device the daemon does
	   not know about - the ESP32 posing as a G29, say - so never route those through the socket. */
	if (C.addr_override) return cmd_ffb_direct(line);

	if (send_via_daemon(sock, line, reply, sizeof reply) == 0) {
		if (!strncmp(reply, "ok", 2)) { printf("%s\n", ok_payload(reply)); return 0; }
		say("dfgt: %s", reply);
		return 1;
	}
	return cmd_ffb_direct(line);
}

static int cmd_daemon(int argc, char **argv)
{
	C.range = DFGT_RANGE_DEFAULT;
	C.autocenter = DFGT_AUTOCENTER_DEFAULT;
	C.writable = 1;		/* the daemon owns wheel settings */
	C.quiet_report = 2;	/* state goes to socket clients, never to stdout */
	C.sock_path = sock_path_default();
	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--verbose")) C.verbose = 1;
		else if (!strcmp(argv[i], "--range") && i + 1 < argc) C.range = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--autocenter") && i + 1 < argc) C.autocenter = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--socket") && i + 1 < argc) C.sock_path = argv[++i];
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	atexit(cleanup);

	if (hid_start(&C) < 0) return 1;
	if (wait_for_device(&C, 3.0) < 0)
		say("dfgt: no wheel yet — waiting for 046d:c29a (plug it in)");

	C.listen_fd = sock_listen(C.sock_path);
	if (C.listen_fd < 0) { say("dfgt: cannot listen on %s", C.sock_path); return 1; }
	printf("dfgt: daemon up, socket %s, range %u, autocenter 0x%04x\n",
	       C.sock_path, C.range, C.autocenter);
	fflush(stdout);

	while (!C.stop) {
		poll_socket(&C);
		/* ponytail: no force watchdog. The DFGT holds an effect across arbitrary packet
		 * silence (measured), so a client that dies leaves its last force applied until
		 * `dfgt ffb off` or a replug — the wheel's own semantics, not a bug to paper over.
		 * The daemon itself always neutralises on start and on exit. */
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
	}
	cleanup();
	unlink(C.sock_path);
	printf("dfgt: daemon stopped\n");
	return 0;
}

static int cmd_status(int argc, char **argv)
{
	char reply[512] = { 0 };
	const char *sock = sock_path_default();

	for (int i = 0; i < argc; i++)
		if (!strcmp(argv[i], "--socket") && i + 1 < argc) sock = argv[++i];

	if (send_via_daemon(sock, "state", reply, sizeof reply) != 0) {
		say("dfgt: no daemon on %s (start it with: dfgt daemon)", sock);
		return 2;
	}
	if (!strncmp(reply, "ok", 2)) {
		printf("%s", ok_payload(reply));
	} else if (!strncmp(reply, "err no-input-yet", 16)) {
		printf("no input yet — turn the wheel or press a pedal once"
		       " (the wheel only sends reports when something changes)\n");
	} else if (reply[0]) {
		printf("%s", reply);	/* a streamed state line beat our reply here */
	} else {
		say("dfgt: empty reply");
		return 1;
	}
	return 0;
}

/* -------------------------------------------------------------- selftest */

static int failures;
static void expect_bytes(const char *what, const uint8_t *got, const uint8_t *want)
{
	if (memcmp(got, want, DFGT_CMD_LEN)) {
		say("FAIL %s: got %02x %02x %02x %02x %02x %02x %02x", what,
		    got[0], got[1], got[2], got[3], got[4], got[5], got[6]);
		failures++;
	}
}

static void expect_u(const char *what, unsigned got, unsigned want)
{
	if (got != want) { say("FAIL %s: got %u want %u", what, got, want); failures++; }
}

static void selftest_decoder(void)
{
	static const struct { const char *hex; unsigned hat, btn, v7, v2, steer, thr, brk; } cases[] = {
		/* real captures: wheel turning under FFB (vendor7 was 0x2f that session) */
		{ "08 00 00 5e 94 2e ff ff", 8, 0, 0x2f, 0, 11924, 255, 255 },
		{ "08 00 00 5e ff 1f ff ff", 8, 0, 0x2f, 0, 8191, 255, 255 },
		{ "08 00 00 5e 57 1f ff ff", 8, 0, 0x2f, 0, 8023, 255, 255 },
		/* real captures from the control-identification session (vendor7 = 0x3f) */
		{ "08 00 80 7e 83 60 ff ff", 8, 1u << 19, 0x3f, 1, 8323, 255, 255 },	/* horn */
		{ "08 00 80 7e 83 e0 ff ff", 8, 1u << 19, 0x3f, 3, 8323, 255, 255 },	/* horn + vendor2 */
		{ "08 00 10 7e 83 20 ff ff", 8, 1u << 16, 0x3f, 0, 8323, 255, 255 },	/* dial cw pulse */
		{ "08 00 04 7e 83 20 ff ff", 8, 1u << 14, 0x3f, 0, 8323, 255, 255 },	/* dial press */
		{ "08 00 01 7e 0b 20 ff ff", 8, 1u << 12, 0x3f, 0, 8203, 255, 255 },	/* shift up */
		{ "00 00 00 7e 83 20 ff ff", 0, 0, 0x3f, 0, 8323, 255, 255 },		/* d-pad up */
		/* synthetic edge cases */
		{ "f3 ff ff ff 00 00 00 00", 3, 0x1fffff, 0x7f, 0, 0, 0, 0 },
		{ "08 00 00 00 ff 3f 00 00", 8, 0, 0, 0, 16383, 0, 0 },
		{ "08 00 00 00 ff ff 12 34", 8, 0, 0, 3, 16383, 0x12, 0x34 },
	};
	uint8_t r[8];

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		struct dfgt_state s;
		unsigned v[8];
		char what[64];
		if (sscanf(cases[i].hex, "%x %x %x %x %x %x %x %x",
		           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) != 8) {
			say("FAIL parse %s", cases[i].hex);
			failures++;
			continue;
		}
		for (int k = 0; k < 8; k++) r[k] = (uint8_t)v[k];
		memset(&s, 0, sizeof s);
		dfgt_decode(r, &s);
		snprintf(what, sizeof what, "decode %s", cases[i].hex);
		expect_u(what, s.hat, cases[i].hat);
		expect_u(what, s.buttons, cases[i].btn);
		expect_u(what, s.vendor7, cases[i].v7);
		expect_u(what, s.steer, cases[i].steer);
		expect_u(what, s.throttle, cases[i].thr);
		expect_u(what, s.brake, cases[i].brk);
		expect_u(what, s.vendor2, cases[i].v2);
	}
}

static void selftest_names(void)
{
	/* mapping derived from tests/capture-controls.hex (each control pressed once, in a
	 * known order). Bit index is 0-based: button N is bit N-1. */
	static const struct { unsigned bit; const char *name; } expect[] = {
		{ 0, "x" }, { 1, "square" }, { 2, "circle" }, { 3, "triangle" },
		{ 4, "r1" }, { 5, "l1" }, { 6, "r2" }, { 7, "l2" },
		{ 8, "select" }, { 9, "start" },
		{ 10, "r3" }, { 11, "l3" }, { 12, "shift-up" }, { 13, "shift-down" },
		{ 14, "dial-press" }, { 15, "rocker-plus" }, { 16, "dial-cw" },
		{ 17, "dial-ccw" }, { 18, "rocker-minus" }, { 19, "horn" }, { 20, "ps" },
	};

	for (size_t i = 0; i < sizeof expect / sizeof expect[0]; i++) {
		const char *got = dfgt_button_names[expect[i].bit];
		if (!got || strcmp(got, expect[i].name)) {
			say("FAIL name[%u]: got %s want %s", expect[i].bit, got ? got : "(null)",
			    expect[i].name);
			failures++;
		}
	}
	for (unsigned i = 0; i < 21; i++) {
		if (!dfgt_button_names[i]) { say("FAIL name[%u] missing", i); failures++; continue; }
		for (unsigned j = i + 1; j < 21; j++)
			if (dfgt_button_names[j] && !strcmp(dfgt_button_names[i], dfgt_button_names[j])) {
				say("FAIL duplicate name %s at %u/%u", dfgt_button_names[i], i, j);
				failures++;
			}
	}
}

static void selftest_commands(void)
{
	uint8_t c[DFGT_CMD_LEN];
	static const uint8_t off[7]      = { 0x13, 0, 0, 0, 0, 0, 0 };
	static const uint8_t ac_off[7]   = { 0xf5, 0, 0, 0, 0, 0, 0 };
	static const uint8_t ac_on[7]    = { 0x14, 0, 0, 0, 0, 0, 0 };
	static const uint8_t const24[7]  = { 0x11, 0x08, 0x98, 0x80, 0, 0, 0 };
	static const uint8_t const_m24[7]= { 0x11, 0x08, 0x68, 0x80, 0, 0, 0 };
	static const uint8_t const_cl[7] = { 0x11, 0x08, 0xff, 0x80, 0, 0, 0 };
	static const uint8_t r900[7]     = { 0xf8, 0x81, 0x84, 0x03, 0, 0, 0 };
	static const uint8_t r200[7]     = { 0xf8, 0x81, 0xc8, 0x00, 0, 0, 0 };
	static const uint8_t r540[7]     = { 0xf8, 0x81, 0x1c, 0x02, 0, 0, 0 };
	static const uint8_t r_min[7]    = { 0xf8, 0x81, 0x28, 0x00, 0, 0, 0 };
	static const uint8_t ac_4000[7]  = { 0xfe, 0x0d, 0x02, 0x02, 0x30, 0, 0 };
	static const uint8_t ac_aaaa[7]  = { 0xfe, 0x0d, 0x06, 0x06, 0x80, 0, 0 };
	static const uint8_t ac_ffff[7]  = { 0xfe, 0x0d, 0x07, 0x07, 0xff, 0, 0 };
	static const uint8_t nat1[7]     = { 0xf8, 0x0a, 0, 0, 0, 0, 0 };
	static const uint8_t nat2[7]     = { 0xf8, 0x09, 0x03, 0x01, 0, 0, 0 };
	static const uint8_t p_dfex[7]   = { 0xf8, 0x09, 0x00, 0x01, 0, 0, 0 };
	static const uint8_t p_dfp[7]    = { 0xf8, 0x09, 0x01, 0x01, 0, 0, 0 };
	static const uint8_t p_g25[7]    = { 0xf8, 0x09, 0x02, 0x01, 0, 0, 0 };
	static const uint8_t p_g27[7]    = { 0xf8, 0x09, 0x04, 0x01, 0, 0, 0 };
	static const uint8_t p_g29[7]    = { 0xf8, 0x09, 0x05, 0x01, 0x01, 0, 0 };

	cmd_force_off(c);             expect_bytes("force_off", c, off);
	cmd_constant(0, c);           expect_bytes("constant(0)", c, off);
	cmd_constant(24, c);          expect_bytes("constant(24)", c, const24);
	cmd_constant(-24, c);         expect_bytes("constant(-24)", c, const_m24);
	cmd_constant(9999, c);        expect_bytes("constant(9999)", c, const_cl);
	cmd_range(900, c);            expect_bytes("range(900)", c, r900);
	cmd_range(200, c);            expect_bytes("range(200)", c, r200);
	cmd_range(540, c);            expect_bytes("range(540)", c, r540);
	cmd_range(10, c);             expect_bytes("range(10)->clamp", c, r_min);
	cmd_range(5000, c);           expect_bytes("range(5000)->clamp", c, r900);
	cmd_autocenter(0x4000, c);    expect_bytes("autocenter(0x4000)", c, ac_4000);
	cmd_autocenter(0xaaaa, c);    expect_bytes("autocenter(0xaaaa)", c, ac_aaaa);
	cmd_autocenter(0xffff, c);    expect_bytes("autocenter(0xffff)", c, ac_ffff);
	cmd_autocenter(0, c);         expect_bytes("autocenter(0)", c, ac_off);
	cmd_autocenter_off(c);        expect_bytes("autocenter_off", c, ac_off);
	cmd_autocenter_activate(c);   expect_bytes("autocenter_activate", c, ac_on);
	cmd_mode_revert_marker(c);    expect_bytes("mode_revert_marker", c, nat1);
	cmd_mode_switch(0x03, c);     expect_bytes("mode_switch(dfgt)", c, nat2);
	cmd_mode_switch(0x00, c);     expect_bytes("mode_switch(dfex)", c, p_dfex);
	cmd_mode_switch(0x01, c);     expect_bytes("mode_switch(dfp)", c, p_dfp);
	cmd_mode_switch(0x02, c);     expect_bytes("mode_switch(g25)", c, p_g25);
	cmd_mode_switch(0x04, c);     expect_bytes("mode_switch(g27)", c, p_g27);
	cmd_mode_switch(0x05, c);     expect_bytes("mode_switch(g29)", c, p_g29);
}

static int selftest_fixture(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[256];
	int n = 0, bad = 0;

	if (!f) { say("FAIL cannot open fixture %s", path); return 1; }
	while (fgets(line, sizeof line, f)) {
		unsigned v[8];
		uint8_t r[8];
		struct dfgt_state s;
		if (line[0] == '#' || line[0] == '\n') continue;
		if (sscanf(line, "%x %x %x %x %x %x %x %x",
		           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) != 8) {
			say("FAIL %s:%d unparsable: %s", path, n + 1, line);
			bad++;
			continue;
		}
		for (int k = 0; k < 8; k++) r[k] = (uint8_t)v[k];
		memset(&s, 0, sizeof s);
		dfgt_decode(r, &s);
		if (s.steer > DFGT_AXIS_MAX || s.hat > 15 || s.vendor7 > 0x7f || s.vendor2 > 3) {
			say("FAIL %s:%d out-of-range decode", path, n + 1);
			bad++;
		}
		n++;
	}
	fclose(f);
	printf("%s: %d reports, %d bad\n", path, n, bad);
	return bad ? 1 : 0;
}

static int cmd_selftest(int argc, char **argv)
{
	selftest_decoder();
	selftest_names();
	selftest_commands();
	for (int i = 0; i < argc; i++)
		if (!strcmp(argv[i], "--fixture") && i + 1 < argc)
			failures += selftest_fixture(argv[++i]) ? 1 : 0;
	if (failures) { say("dfgt selftest: %d failure(s)", failures); return 1; }
	printf("dfgt selftest: ok (decoder + command builders)\n");
	return 0;
}

static void usage(void)
{
	printf(
	"dfgt — Logitech Driving Force GT (046d:c29a) user-space driver\n"
	"\n"
	"  dfgt probe [--cmd HEX] [--values]          elements / current values, identity, self-test\n"
	"  dfgt watch [N] [--hex]                    live decoded input (control identification)\n"
	"  dfgt range <40..900>                      steering lock-to-lock range\n"
	"  dfgt ffb constant <-128..127>|off         constant force (0 = off)\n"
	"  dfgt ffb autocenter <0..65535>|off        self-centring spring\n"
	"  dfgt modes                                list firmware personas and their USB pids\n"
	"  dfgt mode <name> [--force]                re-enumerate as dfex|dfp|g25|dfgt|g27|g29\n"
	"  dfgt native                               alias for: dfgt mode dfgt\n"
	"  dfgt daemon [--range D] [--autocenter M]   keep the wheel alive + serve the socket\n"
	"  dfgt status                               print current wheel state\n"
	"  dfgt selftest [--fixture FILE]            regression checks\n"
	"\n"
	"commands go to the daemon when one is running, otherwise straight to the wheel.\n"
	"socket: $DFGT_SOCKET (default /tmp/dfgt-daemon.sock)\n"
	"global: --vid 0x.. --pid 0x..             address a wheel that re-enumerated as another persona\n");
}

int main(int argc, char **argv)
{
	/* a client that goes away mid-broadcast must not kill the daemon (default SIGPIPE) */
	signal(SIGPIPE, SIG_IGN);
	/* fd 0 is valid: never let a zero-initialised struct look like a connection */
	C.listen_fd = -1;
	for (int i = 0; i < DFGT_MAX_CONNS; i++) C.conns[i].fd = -1;
	C.vid = DFGT_VID;
	C.pid = DFGT_PID;
	{   /* global --vid/--pid, so a wheel that switched persona stays reachable */
		static char *filtered[32];
		int n = 0;
		filtered[n++] = argv[0];
		for (int i = 1; i < argc && n < 31; i++) {
			if (!strcmp(argv[i], "--vid") && i + 1 < argc) {
				C.vid = (unsigned)strtoul(argv[++i], NULL, 0);
				C.addr_override = 1;
				continue;
			}
			if (!strcmp(argv[i], "--pid") && i + 1 < argc) {
				C.pid = (unsigned)strtoul(argv[++i], NULL, 0);
				C.addr_override = 1;
				continue;
			}
			filtered[n++] = argv[i];
		}
		argc = n;
		argv = filtered;
	}
	if (argc < 2) { usage(); return 2; }
	if (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help")) { usage(); return 0; }
	if (!strcmp(argv[1], "probe"))    return cmd_probe(argc - 2, argv + 2);
	if (!strcmp(argv[1], "watch"))    return cmd_watch(argc - 2, argv + 2);
	if (!strcmp(argv[1], "native"))   return cmd_native();
	if (!strcmp(argv[1], "modes"))    return cmd_modes();
	if (!strcmp(argv[1], "mode"))     return cmd_mode(argc > 2 ? argv[2] : "dfgt",
	                                                   argc > 3 && !strcmp(argv[3], "--force"));
	if (!strcmp(argv[1], "daemon"))   return cmd_daemon(argc - 2, argv + 2);
	if (!strcmp(argv[1], "status"))   return cmd_status(argc - 2, argv + 2);
	if (!strcmp(argv[1], "selftest")) return cmd_selftest(argc - 2, argv + 2);
	if (!strcmp(argv[1], "range"))    return dispatch_control_command(argc - 1, argv + 1);
	if (!strcmp(argv[1], "ffb"))      return dispatch_control_command(argc - 1, argv + 1);
	usage();
	return 2;
}
