/*
 * g29-gadget - the ESP32-S3 pretends to be a Logitech G29 (046d:c24f).
 *
 * Why: GeForce NOW only supports force feedback for a whitelist of wheels
 * (G29/G920/G923/PRO) and the Driving Force GT is not on it. Nothing on the macOS side
 * can change the identity macOS reports to clients - hidutil property overrides were
 * tried and do not affect what IOHIDDeviceGetProperty returns - so this board supplies
 * the identity instead.
 *
 * The S3 has a single USB PHY, so it cannot also host the wheel: the real wheel stays on
 * the Mac and will be relayed by the dfgt daemon over this board's UART bridge
 * (milestone 2). This firmware is milestone 1 - the decisive question: does a GFN
 * session start sending wheel force feedback to a device that merely claims to be a G29?
 *
 * Everything the host sends is hex-dumped over UART0 (the CH343 bridge port, NOT the
 * native USB port), together with a decode of the Logitech command it looks like.
 * Logging goes through the ESP-IDF console (printf -> UART0), not the Arduino Serial
 * object: the console is already up from boot, and it keeps this file parseable by
 * tooling that has no Xtensa toolchain.
 *
 * Report descriptor: the Driving Force GT's own, captured from the real wheel
 * (src/wheel_descriptor.h). Only the USB identity is a G29's.
 */

#include <Arduino.h>
#include "USB.h"
#include "USBHID.h"
#include "g29_descriptor.h"
#include "soc/rtc_cntl_reg.h"

USBHID HID;

/* Magic output report from the host: reboots into the ROM download mode. Needed because
   while this firmware runs, the S3's single USB PHY belongs to the HID device, so the
   USB-Serial-JTAG port does not exist and esptool has nothing to talk to. Send it with:
     ./build/dfgt --pid 0xc24f probe --cmd 5245424f4f5421   ("REBOOT!")                      */
static const uint8_t kRebootMagic[7] = { 'R', 'E', 'B', 'O', 'O', 'T', '!' };

/* How many force-feedback reports the host has sent us. Reported back to the host in the
   input report's 7 unused vendor bits, so "is the cloud actually sending FFB?" can be
   answered by reading the device from the Mac - no serial cable needed. Declared before
   the class that uses it. */
static volatile uint8_t g_ffb_count = 0;

/* Relay protocol over UART0 (the CH343 bridge port) - see docs/geforce-now.md.
   Frames: 0xA5 <type> <len> <payload...> <xor of type, len and payload>.
   Log lines are plain ASCII, so the sync byte can never appear in them: the host scans
   for 0xA5 and treats everything else as log noise.
     'S' state: 8 bytes, the input report the board should emit   (Mac -> board)
     'F' ffb:   7 bytes, an output report the host just sent us   (board -> Mac)   */
#define RELAY_SYNC 0xA5
#define RELAY_STATE 'S'
#define RELAY_FFB 'F'
#define RELAY_MAX_PAYLOAD 20

/* The G29-shaped state report the client reads: 12 bytes, no report ID, with the three
   trailing vendor bytes at the reference profile's defaults (0x81, 0x80, 0x9c). */
static uint8_t  relay_state[12] = { 0x08, 0x00, 0x00, 0x00, 0x00, 0x80,
                                    0xff, 0xff, 0xff, 0x81, 0x80, 0x9c };
static uint32_t relay_last_ms = 0;

/* The Mac is only considered in charge while it keeps sending state frames. */
static bool relay_active(void)
{
    return relay_last_ms != 0 && (millis() - relay_last_ms) < 1000;
}

static void relay_send(uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t sum = (uint8_t)(type ^ len);

    Serial.write(RELAY_SYNC);
    Serial.write(type);
    Serial.write(len);
    for (uint8_t i = 0; i < len; i++) {
        Serial.write(payload[i]);
        sum ^= payload[i];
    }
    Serial.write(sum);
}

/* Non-blocking frame parser; anything that is not a valid frame is dropped. */
static void relay_poll(void)
{
    static uint8_t frame[RELAY_MAX_PAYLOAD + 4];
    static uint8_t got = 0;
    static uint8_t want = 0;

    while (Serial.available() > 0) {
        uint8_t b = (uint8_t)Serial.read();

        if (got == 0) {
            if (b == RELAY_SYNC) {
                frame[got++] = b;
                want = 0;
            }
            continue;
        }
        frame[got++] = b;
        if (got == 3) {
            want = b;
            if (want > RELAY_MAX_PAYLOAD) got = 0;
        } else if (got == (uint8_t)(want + 4)) {
            uint8_t sum = 0;
            for (uint8_t i = 1; i < got - 1; i++) sum ^= frame[i];
            if (sum == frame[got - 1] && frame[1] == RELAY_STATE && want == 12) {
                memcpy(relay_state, frame + 3, 12);
                relay_last_ms = millis();
            }
            got = 0;
        }
    }
}

class WheelHID : public USBHIDDevice {
public:
    WheelHID() {
        static bool initialized = false;
        if (!initialized) {
            initialized = true;
            HID.addDevice(this, sizeof(g29_report_descriptor));
        }
    }

    uint16_t _onGetDescriptor(uint8_t *buffer) {
        memcpy(buffer, g29_report_descriptor, sizeof(g29_report_descriptor));
        return sizeof(g29_report_descriptor);
    }

    /* The wheel's 131-byte feature report is unknown territory; answer with zeros
       rather than stalling the host. */
    uint16_t _onGetFeature(uint8_t report_id, uint8_t *buffer, uint16_t len) {
        printf("[hid] GET_FEATURE id=%u len=%u -> zeros\n", report_id, len);
        memset(buffer, 0, len);
        return len;
    }

    /* Note: the Arduino wrapper routes a control-pipe SET_REPORT with type Output here,
       and descriptor-less OUT-endpoint data to _onOutput. Log both, so whichever way
       the host sends force feedback, it shows up. */
    void _onSetFeature(uint8_t report_id, const uint8_t *buffer, uint16_t len) {
        dump("SET_REPORT", report_id, buffer, len);
    }

    void _onOutput(uint8_t report_id, const uint8_t *buffer, uint16_t len) {
        dump("OUT", report_id, buffer, len);
    }

private:
    static void dump(const char *what, uint8_t report_id, const uint8_t *buf, uint16_t len) {
        g_ffb_count++;

        /* Host asked for bootloader mode? Has to work even in the middle of a relay. */
        if (len == sizeof kRebootMagic && memcmp(buf, kRebootMagic, sizeof kRebootMagic) == 0) {
            printf("      -> host asked for bootloader mode; rebooting\n");
            fflush(stdout);
            delay(50);
#ifdef RTC_CNTL_FORCE_DOWNLOAD_BOOT
            REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
#endif
            esp_restart();
        }

        /* While relaying, the wire carries frames only - the daemon logs them on the Mac. */
        if (relay_active()) {
            relay_send(RELAY_FFB, buf, (uint8_t)(len > RELAY_MAX_PAYLOAD ? RELAY_MAX_PAYLOAD : len));
            return;
        }

        printf("[ffb] %s id=%u len=%u (total %u):", what, report_id, len, g_ffb_count);
        for (uint16_t i = 0; i < len; i++) {
            printf(" %02x", buf[i]);
        }
        printf("\n");
        if (len >= 4 && buf[0] == 0xf8 && buf[1] == 0x81) {
            printf("      -> lg4ff set range %u deg\n", buf[2] | (buf[3] << 8));
        } else if (len >= 3 && buf[0] == 0x11 && buf[1] == 0x08) {
            printf("      -> lg4ff constant force level %d\n", (int)buf[2] - 0x80);
        } else if (len >= 2 && buf[0] == 0xfe && buf[1] == 0x0d) {
            printf("      -> lg4ff autocenter profile\n");
        } else if (len >= 1 && (buf[0] == 0x14 || buf[0] == 0xf5 || buf[0] == 0x13)) {
            printf("      -> lg4ff force/autocenter enable or disable\n");
        }
    }
};

static WheelHID wheel;

/* The input report the host reads. While the Mac is relaying it owns these bytes
   verbatim (so the real wheel's state, or whatever it sends, goes straight through);
   otherwise the board streams a neutral DFGT state with the FFB counter in the unused
   vendor bits - which doubles as the cable-free "is the cloud sending FFB?" readout. */
static void send_input_state()
{
    uint8_t state[12];

    if (relay_active()) {
        memcpy(state, relay_state, sizeof state);
    } else {
        /* Neutral G29-shaped state: hat centred, no buttons, steering centred at 32768,
           pedals and clutch released, vendor bytes at the reference profile's defaults. */
        const uint8_t neutral[12] = { 0x08, 0x00, 0x00, 0x00, 0x00, 0x80,
                                     0xff, 0xff, 0xff, 0x81, 0x80, 0x9c };
        memcpy(state, neutral, sizeof state);
    }
    if (!HID.SendReport(0, state, sizeof state)) {
        printf("[hid] SendReport failed (host not reading?)\n");
        fflush(stdout);
    }
}

void setup()
{
    Serial.begin(115200);  /* UART0 RX for the relay protocol; logs go out via printf */
    delay(300);
    printf("\ng29-gadget: presenting 046d:c24f \"G29 Driving Force Racing Wheel\"\n");
    printf("report descriptor: %u bytes (GFN-accepted G29 layout: 12-byte input, 7-byte FFB output)\n",
           (unsigned)sizeof(g29_report_descriptor));

    USB.manufacturerName("Logitech");
    USB.productName("G29 Driving Force Racing Wheel");
    USB.serialNumber("0000000000");
    USB.firmwareVersion(0x1350);	/* bcdDevice: the value a real G29 reports, and the one the
					   macOS GeForce NOW client accepted (its log shows
					   "Plugging 046D:C24F:1350" then a HID reading loop). The
					   Windows virtual-G29 project keys on 0x8900 instead, but that
					   value stops macOS attaching a HID driver to this interface. */
    /* Note: these are uppercase setters, and they only take effect before USB.begin(). */
    if (!USB.VID(0x046d) || !USB.PID(0xc24f)) {
        printf("warning: VID/PID refused - USB already started?\n");
    }

    HID.begin();
    USB.begin();
    delay(300);
    printf("USB up. Plug the *native* USB port into the Mac and watch here for ff.\n"
           "Any [ffb] line means the host is talking wheel commands to us.\n");
    fflush(stdout);
}

void loop()
{
    static uint32_t last_report = 0;
    static uint32_t last_beat = 0;

    if (millis() - last_report >= 50) {  /* 20 Hz: enough for a wheel state */
        last_report = millis();
        send_input_state();
    }
    relay_poll();
    if (millis() - last_beat >= 2000) {  /* keep the log readable */
        last_beat = millis();
        printf("[hid] %s (%lus up, ffb reports received: %u)\n",
               relay_active() ? "relaying the Mac's state" : "neutral state streaming",
               (unsigned long)(millis() / 1000), g_ffb_count);
        fflush(stdout);
    }
    delay(5);
}
