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
 *
 * Report descriptor: the Driving Force GT's own, captured from the real wheel
 * (src/wheel_descriptor.h). Only the USB identity is a G29's.
 */

#include <Arduino.h>
#include "USB.h"
#include "USBHID.h"
#include "wheel_descriptor.h"

USBHID HID;

class WheelHID : public USBHIDDevice {
public:
    WheelHID() {
        static bool initialized = false;
        if (!initialized) {
            initialized = true;
            HID.addDevice(this, sizeof(wheel_report_descriptor));
        }
    }

    uint16_t _onGetDescriptor(uint8_t *buffer) {
        memcpy(buffer, wheel_report_descriptor, sizeof(wheel_report_descriptor));
        return sizeof(wheel_report_descriptor);
    }

    /* The wheel's 131-byte feature report is unknown territory; answer with zeros
       rather than stalling the host. */
    uint16_t _onGetFeature(uint8_t report_id, uint8_t *buffer, uint16_t len) {
        Serial.printf("[hid] GET_FEATURE id=%u len=%u -> zeros\n", report_id, len);
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
        Serial.printf("[ffb] %s id=%u len=%u:", what, report_id, len);
        for (uint16_t i = 0; i < len; i++) {
            Serial.printf(" %02x", buf[i]);
        }
        Serial.println();
        if (len >= 4 && buf[0] == 0xf8 && buf[1] == 0x81) {
            Serial.printf("      -> lg4ff set range %u deg\n", buf[2] | (buf[3] << 8));
        } else if (len >= 3 && buf[0] == 0x11 && buf[1] == 0x08) {
            Serial.printf("      -> lg4ff constant force level %d\n", (int)buf[2] - 0x80);
        } else if (len >= 2 && buf[0] == 0xfe && buf[1] == 0x0d) {
            Serial.println("      -> lg4ff autocenter profile");
        } else if (len >= 1 && (buf[0] == 0x14 || buf[0] == 0xf5 || buf[0] == 0x13)) {
            Serial.println("      -> lg4ff force/autocenter enable or disable");
        }
    }
};

static WheelHID wheel;

/* Neutral 8-byte state, exactly what a real DFGT sends at rest: hat centred (8), no
   buttons, vendor bits 0x3f, steering at 8192, pedals released (0xff). */
static void send_neutral_state()
{
    static const uint8_t state[8] = { 0x08, 0x00, 0x00, 0x7e, 0x00, 0x20, 0xff, 0xff };
    if (!HID.SendReport(0, state, sizeof state)) {
        Serial.println("[hid] SendReport failed (host not reading?)");
    }
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("g29-gadget: presenting 046d:c24f \"G29 Driving Force Racing Wheel\"");
    Serial.printf("report descriptor: %u bytes (DFGT's, captured from the real wheel)\n",
                  (unsigned)sizeof(wheel_report_descriptor));

    USB.manufacturerName("Logitech");
    USB.productName("G29 Driving Force Racing Wheel");
    USB.serialNumber("0000000000");
    /* Note: these are uppercase setters, and they only take effect before USB.begin(). */
    if (!USB.VID(0x046d) || !USB.PID(0xc24f)) {
        Serial.println("warning: VID/PID refused - USB already started?");
    }

    HID.begin();
    USB.begin();
    delay(300);
    Serial.println("USB up. Plug the *native* USB port into the Mac and watch here for ff.\n"
                   "Any [ffb] line means the host is talking wheel commands to us.");
}

void loop()
{
    static uint32_t last_report = 0;
    static uint32_t last_beat = 0;

    if (millis() - last_report >= 50) {  /* 20 Hz: enough for a wheel state */
        last_report = millis();
        send_neutral_state();
    }
    if (millis() - last_beat >= 2000) {  /* keep the log readable */
        last_beat = millis();
        Serial.printf("[hid] neutral state streaming (%lus up)\n",
                      (unsigned long)(millis() / 1000));
    }
    delay(5);
}
