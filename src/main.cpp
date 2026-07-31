#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "esp_system.h"
#include <Preferences.h>
#include <esp_efuse.h>
#include <esp_mac.h>
#include "config.h"
#include "hdmi/tc358743.h"
#include "hdmi/tc358743_regs.h"
#include "ltc/ltc_encoder.h"
#include "ltc/ltc_decoder.h"
#include "hdmi/panasonic_tc.h"
#if RTC_ENABLE
#if defined(TCWL_USE_INTERNAL_RTC)
#include "rtc/internal_rtc.h"
#else
#include "rtc/ds3231.h"
#endif
#endif
#if OLED_ENABLE
#include "oled/oled_display.h"
#endif
#if MAX7219_ENABLE
#include "matrix/max7219_display.h"
#endif
#if WEBUI_ENABLE
#include "webui/webui.h"
#endif
#include "timecode/ble_timecode.h"

#if OLED_ENABLE
#include "btn/button.h"
#include "oled/oled_menu.h"
#endif

// ---------------------------------------------------------------------------
// Master-specific globals
// ---------------------------------------------------------------------------
#if TCWL_HDMI
TC358743 tc(Wire, TC_I2C_ADDR);
#endif
LtcEncoder ltc(LTC_OUT_PIN, LTC_FPS, LTC_DROP_FRAME);
#if TCWL_LTC
static LtcDecoder ltcDecoder(LTC_IN_PIN);
#endif
static char tcStr[13] = "00:00:00:00";

// Master role sub-modes — stored in NVS ("ltc" namespace, key "m_role")
#define MASTER_ROLE_IN   0   // decode LTC input only, no output
#define MASTER_ROLE_OUT  1   // generate LTC output only, free-running
#define MASTER_ROLE_BOTH 2   // decode input AND generate output

static int getMasterRole() {
    Preferences p;
    p.begin("ltc", false);
    int role = p.getUChar("m_role", MASTER_ROLE_IN);
    p.end();
    return role;
}

static void setMasterRole(int role) {
    Preferences p;
    p.begin("ltc", false);
    p.putUChar("m_role", (uint8_t)role);
    p.end();
}

static const char* masterRoleStr(int role) {
    switch (role) {
        case MASTER_ROLE_IN:   return "IN";
        case MASTER_ROLE_OUT:  return "OUT";
        case MASTER_ROLE_BOTH: return "BOTH";
        default:               return "?";
    }
}

static void fmtTcStr(uint8_t hh, uint8_t mm, uint8_t ss, uint8_t ff) {
    snprintf(tcStr, sizeof(tcStr), "%02u:%02u:%02u:%02u", hh, mm, ss, ff);
}

#if OLED_ENABLE
static OledDisplay oled;
static char gDeviceName[33] = "";

static bool _pendingReboot = false;
static unsigned long _rebootTimer = 0;

static bool _delayedRecycle = false;
static unsigned long _lastRecycle = 0;

enum TcEditField { TC_DD, TC_HH, TC_MM, TC_SS, TC_FF };
static bool _editingTc = false;
static TcEditField _editField = TC_DD;
static uint8_t _editVals[5] = {0, 0, 0, 0, 0};
static uint8_t _menuOpenBtn = 0;

// Master selection state
static bool _selectingMaster = false;
static BleScanResult _menuScanResults[16];
static uint8_t _menuScanCount = 0;
static uint8_t _menuScanCursor = 0;
static int8_t _menuScanScroll = 0;

static bool handleRebootDisplay() {
    if (!_pendingReboot) return false;
    unsigned long elapsed = millis() - _rebootTimer;
    if (elapsed >= 2000) {
        auto &d = oled.display();
        d.clearDisplay();
        d.setTextSize(2);
        d.setTextColor(WHITE);
        d.setCursor(28, 24);
        d.print("REBOOT");
        d.display();
        if (elapsed >= 2500) {
            ESP.restart();
        }
        return true;
    }
    return false;
}

static bool drawTcEditor() {
    if (!_editingTc) return false;
    auto &d = oled.display();
    d.clearDisplay();
    d.setTextSize(1);
    d.setTextColor(SSD1306_WHITE);
    const char* labels[] = {"DD", "HH", "MM", "SS", "FF"};
    d.setCursor(28, 0);
    d.print("SET TIMECODE");
    d.drawFastHLine(0, 10, 128, SSD1306_WHITE);
    int y = 17;
    for (int i = 0; i < 5; i++) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%s%s: %02u", _editField == i ? ">" : " ", labels[i], _editVals[i]);
        d.setCursor(24, y);
        d.print(buf);
        y += 8;
    }
    d.drawFastHLine(0, 56, 128, SSD1306_WHITE);
    d.setCursor(14, 57);
    d.print("OK=next  CAN=exit");
    d.display();
    return true;
}

#if OLED_ENABLE && BTN_UP_PIN >= 0
static Button btnUp(BTN_UP_PIN);
static Button btnDown(BTN_DOWN_PIN);
static Button btnOk(BTN_OK_PIN);
static Button btnCancel(BTN_CANCEL_PIN);
static OledMenu menu(oled);
#endif
#endif

#if RTC_ENABLE
#if defined(TCWL_USE_INTERNAL_RTC)
static InternalRtc rtc;
#else
static DS3231 rtc(Wire, RTC_I2C_ADDR);
#endif
static bool rtcPresent = false;
static unsigned long lastRtcReadMs = 0;
static unsigned long lastRtcSyncMs = 0;
static uint8_t rtcHH = 0, rtcMM = 0, rtcSS = 0;
#endif

#if WEBUI_ENABLE
static WebUI webui;
#endif
#if MAX7219_ENABLE
static Max7219Display mx7219(MAX7219_CS_PIN, MAX7219_NUM_MODULES);
#endif

static bool tcPresent = false;
static char tcSource[8] = "FREE";

// Master fast-poll constants
static const unsigned long FAST_POLL_MS = 8;
unsigned long lastFastPoll = 0;
unsigned long lastFramePoll = 0;
unsigned long framePollMs = 1000 / LTC_FPS;

// ---------------------------------------------------------------------------
// Frame rate auto-detection (HDMI only)
// ---------------------------------------------------------------------------
#if FPS_AUTO_DETECT
static bool fpsDetected = false;
static unsigned long lastFrameUs = 0;
static unsigned long intervals[30];
static uint8_t intervalCount = 0;
static const uint8_t MIN_SAMPLES = 10;

static void resetFpsDetect() {
    fpsDetected = false;
    intervalCount = 0;
    lastFrameUs = 0;
}

static bool tryDetectFps() {
    if (intervalCount < MIN_SAMPLES) return false;

    uint32_t sum = 0;
    for (uint8_t i = 0; i < intervalCount; i++) sum += intervals[i];
    uint32_t avg = sum / intervalCount;

    uint8_t fps;
    if      (avg >= 40833) fps = 24;
    else if (avg >= 36667) fps = 25;
    else if (avg >= 26667) fps = 30;
    else if (avg >= 18333) fps = 50;
    else                   fps = 60;

    bool df = LTC_DROP_FRAME;

    if (fps != LTC_FPS || df != LTC_DROP_FRAME) {
        ltc.setFps(fps, df);
        framePollMs = 1000 / fps;
    }

    Serial.print(F("Auto-detected "));
    Serial.print(fps);
    Serial.println(F(" fps"));
    fpsDetected = true;
    return true;
}
#endif

// ---------------------------------------------------------------------------
// Reverse-engineer mode helpers (HDMI only)
// ---------------------------------------------------------------------------
#if defined(TCWL_HDMI) && REVERSE_ENGINEER_MODE
void dumpBuffer(const char *label, uint16_t reg, size_t len) {
    uint8_t buf[32];
    if (len > sizeof(buf)) len = sizeof(buf);
    tc.readBlock(reg, buf, len);
    Serial.print(label);
    Serial.print(": ");
    for (size_t i = 0; i < len; i++) {
        if (buf[i] < 0x10) Serial.print('0');
        Serial.print(buf[i], HEX);
        Serial.print(' ');
    }
    Serial.println();
}

void testEdidWrite() {
    const uint16_t TEST_REG = 0x8C00;
    Serial.println(F("--- EDID RAM test (read-only) ---"));
    Serial.print(F("EDID header: "));
    for (int i = 0; i < 8; i++) {
        Serial.print(tc.readReg8(TEST_REG + i), HEX); Serial.print(' ');
    }
    Serial.println();
}

void dumpEdid() {
    static const size_t EDID_SIZE = 256;
    uint8_t buf[EDID_SIZE];
    Serial.println(F("--- EDID readback (byte-by-byte) ---"));
    for (size_t i = 0; i < EDID_SIZE; i++) {
        buf[i] = tc.readReg8(EDID_RAM + i);
    }
    for (size_t i = 0; i < EDID_SIZE; i += 16) {
        char line[80];
        int pos = snprintf(line, sizeof(line), "%04X: ", (unsigned)i);
        for (size_t j = 0; j < 16; j++) {
            pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", buf[i + j]);
        }
        Serial.println(line);
    }
    uint8_t sum0 = 0;
    for (int i = 0; i < 128; i++) sum0 += buf[i];
    Serial.print(F("Block 0 checksum: 0x")); Serial.print(sum0, HEX);
    Serial.println(sum0 == 0 ? F(" (OK)") : F(" (BAD!)"));
    uint8_t sum1 = 0;
    for (int i = 128; i < 256; i++) sum1 += buf[i];
    Serial.print(F("Block 1 checksum: 0x")); Serial.print(sum1, HEX);
    Serial.println(sum1 == 0 ? F(" (OK)") : F(" (BAD!)"));
    Serial.println();
}

void runReverseEngineerDump() {
    uint8_t st = tc.readReg8(SYS_STATUS);
    uint8_t hpd = tc.readReg8(HPD_CTL);
    uint8_t ddc = tc.readReg8(DDC_CTL);

    static bool first = true;

    // One-line status every 500ms
    Serial.print(F("SYS_STATUS: 0x")); Serial.print(st, HEX);
    Serial.print(F(" DDC5V=")); Serial.print((st >> 0) & 1);
    Serial.print(F(" TMDS="));  Serial.print((st >> 1) & 1);
    Serial.print(F(" PLL="));   Serial.print((st >> 2) & 1);
    Serial.print(F(" HDMI="));  Serial.print((st >> 3) & 1);
    Serial.print(F(" MUTE="));  Serial.print((st >> 6) & 1);
    Serial.print(F(" SYNC="));  Serial.print((st >> 7) & 1);
    Serial.print(F(" VI1=0x")); Serial.print(tc.readReg8(VI_STATUS1), HEX);
    Serial.print(F(" VI3=0x")); Serial.print(tc.readReg8(VI_STATUS3), HEX);
    Serial.println();

    // Read VSIF via ACP (type 0x81) — PK_VS registers are zero on this clone
    static uint8_t lastVsifAcp[PK_ACP_LEN] = {0};
    uint8_t vsifAcp[PK_ACP_LEN];
    tc.selectPacketType(0x81);
    delay(2);
    tc.readBlock(PK_ACP_0HEAD, vsifAcp, PK_ACP_LEN);
    bool vsifChanged = memcmp(vsifAcp, lastVsifAcp, PK_ACP_LEN) != 0;
    bool vsifNonZero = false;
    for (size_t i = 0; i < PK_ACP_LEN; i++) { if (vsifAcp[i]) { vsifNonZero = true; break; } }
    if (vsifChanged || vsifNonZero) {
        memcpy(lastVsifAcp, vsifAcp, PK_ACP_LEN);
        Serial.print(F("VSIF(ACP): "));
        for (size_t i = 0; i < PK_ACP_LEN; i++) {
            if (vsifAcp[i] < 0x10) Serial.print('0');
            Serial.print(vsifAcp[i], HEX); Serial.print(' ');
        }
        if (vsifAcp[0] == 0x81) Serial.print(F(" (VSIF)"));
        Serial.println();
    }

    if (!first) return;
    first = false;

    // Full dump once on first call
    Serial.println(F("--- full diagnostics ---"));
    // 0x85xx register write-read-back test
    uint8_t testVal = tc.readReg8(SYS_CLK);
    tc.writeReg8(SYS_CLK, 0xA5);
    uint8_t testRead = tc.readReg8(SYS_CLK);
    tc.writeReg8(SYS_CLK, testVal);
    if (testRead != 0xA5) {
        Serial.print(F("WARN: 0x85xx write/read mismatch! SYS_CLK: wrote 0xA5 got 0x"));
        Serial.println(testRead, HEX);
    }
    uint8_t vi1 = tc.readReg8(VI_STATUS1);
    uint8_t vi3 = tc.readReg8(VI_STATUS3);
    uint8_t phyCsq = tc.readReg8(PHY_CSQ);
    uint16_t sysInt = tc.readReg16(SYS_INT);
    uint8_t edidSeg = tc.readReg8(EDID_SEG);
    uint8_t edidLen1 = tc.readReg8(EDID_LEN1);
    uint8_t edidLen2 = tc.readReg8(EDID_LEN2);
    uint8_t sf0 = tc.readReg8(SYS_FREQ0);
    uint8_t sf1 = tc.readReg8(SYS_FREQ1);
    uint8_t nco = tc.readReg8(NCO_F0_MOD);
    uint8_t fhMin0 = tc.readReg8(FH_MIN0);
    uint8_t fhMin1 = tc.readReg8(FH_MIN1);
    uint8_t fhMax0 = tc.readReg8(FH_MAX0);
    uint8_t fhMax1 = tc.readReg8(FH_MAX1);
    uint8_t ldet0 = tc.readReg8(LOCKDET_REF0);
    uint8_t ldet1 = tc.readReg8(LOCKDET_REF1);
    uint8_t ldet2 = tc.readReg8(LOCKDET_REF2);
    uint8_t phyCtl0 = tc.readReg8(PHY_CTL0);
    uint16_t sysctl = tc.readReg16(SYSCTL);
    uint8_t hpdNew = tc.readReg8(0x85C8);
    uint8_t ddcNew = tc.readReg8(0x85C7);
    Serial.print(F("HPD_CTL(0x8544): 0x")); Serial.print(hpd, HEX);
    Serial.print(F(" (0x85C8): 0x")); Serial.print(hpdNew, HEX);
    Serial.print(F(" DDC_CTL(0x8543): 0x")); Serial.print(ddc, HEX);
    Serial.print(F(" (0x85C7): 0x")); Serial.print(ddcNew, HEX);
    Serial.print(F(" PHY_CSQ: 0x")); Serial.print(phyCsq, HEX);
    Serial.print(F(" VI_STAT1: 0x")); Serial.print(vi1, HEX);
    Serial.print(F(" VI_STAT3: 0x")); Serial.print(vi3, HEX);
    Serial.print(F(" SYS_INT: 0x")); Serial.print(sysInt, HEX);
    Serial.print(F(" SF:0x")); Serial.print(sf1, HEX); Serial.print(sf0, HEX);
    Serial.print(F(" FH:")); Serial.print(fhMin1, HEX); Serial.print(fhMin0, HEX);
    Serial.print(F("-")); Serial.print(fhMax1, HEX); Serial.print(fhMax0, HEX);
    Serial.print(F(" LD:")); Serial.print(ldet2, HEX); Serial.print(ldet1, HEX); Serial.print(ldet0, HEX);
    Serial.print(F(" NCO:0x")); Serial.print(nco, HEX);
    Serial.print(F(" PHYCTL0:0x")); Serial.print(phyCtl0, HEX);
    Serial.print(F(" SYSCTL:0x")); Serial.print(sysctl, HEX);
    uint8_t edidSegNum = tc.readReg8(EDID_SEG_NUM);
    Serial.print(F(" EDID_SEG: 0x")); Serial.print(edidSeg, HEX);
    Serial.print(F(" EDID_SEG_NUM: 0x")); Serial.print(edidSegNum, HEX);
    Serial.print(F(" EDID_LEN: 0x")); Serial.print(edidLen2, HEX); Serial.print(edidLen1, HEX);
    Serial.println();
    uint8_t edidHdr[16];
    for (int i = 0; i < 16; i++) edidHdr[i] = tc.readReg8(EDID_RAM + i);
    Serial.print(F("EDID_RAM[0..15]: "));
    for (int i = 0; i < 16; i++) { if (edidHdr[i] < 0x10) Serial.print('0'); Serial.print(edidHdr[i], HEX); Serial.print(' '); }
    Serial.println();
    Serial.print(F("HDMI mode: "));
    Serial.println(tc.isHdmiMode() ? F("yes (good, packets active)") : F("no (DVI - no packets!)"));

    dumpEdid();
    Serial.print(F("CEA_BLK1[0..19]: "));
    for (int i = 0; i < 20; i++) {
        uint8_t b = tc.readReg8(EDID_RAM + 128 + i);
        if (b < 0x10) Serial.print('0');
        Serial.print(b, HEX); Serial.print(' ');
    }
    Serial.println();

    dumpBuffer("AVI InfoFrame       ", PK_AVI_0HEAD, PK_AVI_LEN);
    dumpBuffer("Vendor-Specific IF  ", PK_VS_0HEAD,  PK_VS_LEN);
    dumpBuffer("Audio InfoFrame     ", PK_AUD_0HEAD, PK_AUD_LEN);
    dumpBuffer("Source Product IF   ", PK_SPD_0HEAD, PK_SPD_LEN);
    dumpBuffer("MPEG Source IF      ", PK_MS_0HEAD,  PK_MS_LEN);
    dumpBuffer("ISRC1               ", PK_ISRC1_0HEAD, PK_ISRC1_LEN);
    dumpBuffer("ISRC2               ", PK_ISRC2_0HEAD, PK_ISRC2_LEN);

    // Scan ALL CEA-861 packet types to find non-zero data (GH5 timecode location)
    Serial.println(F("--- Scanning all ACP packet types (non-zero only) ---"));
    for (uint16_t t = 0; t <= 0xFF; t++) {
        tc.selectPacketType((uint8_t)t);
        delay(2);
        uint8_t buf[PK_ACP_LEN];
        tc.readBlock(PK_ACP_0HEAD, buf, PK_ACP_LEN);
        bool allZero = true;
        for (size_t i = 0; i < PK_ACP_LEN; i++) {
            if (buf[i]) { allZero = false; break; }
        }
        if (!allZero) {
            Serial.printf("Type 0x%02X: ", t);
            for (size_t i = 0; i < PK_ACP_LEN; i++) {
                if (buf[i] < 0x10) Serial.print('0');
                Serial.print(buf[i], HEX); Serial.print(' ');
            }
            Serial.println();
        }
    }
    Serial.println(F("--- End scan ---"));

    Serial.println();
}
#endif

// ---------------------------------------------------------------------------
// Slave-specific: BLE timecode callback
// ---------------------------------------------------------------------------
static void onBleTimecode(uint8_t dd, uint8_t hh, uint8_t mm, uint8_t ss, uint8_t ff, uint8_t fps) {
    // Sync FPS from master unless the menu is active (user is changing it).
    if (!bleTimecodeIsMenuActive() && fps != ltc.fps()) {
        ltc.setFps(fps, ltc.dropFrame());
        framePollMs = 1000 / fps;
    }
    ltc.setTime(hh, mm, ss, ff);
    ltc.setDd(dd);
}

#if TCWL_LTC
static void onLtcDecoded(uint8_t dd, uint8_t hh, uint8_t mm, uint8_t ss, uint8_t ff) {
    (void)dd;
    ltc.setTime(hh, mm, ss, ff);
}

static unsigned long lastDecodedFrameMs = 0;
#endif

#if OLED_ENABLE && BTN_UP_PIN >= 0
// ── Shared OLED menu helpers ──────────────────────────────────
static void menuSaveFps(uint8_t fps, bool df) {
    ltc.setFps(fps, df);
    framePollMs = 1000 / fps;
    Preferences p;
    p.begin("ltc", false);
    p.putUChar("fps", fps);
    p.putBool("df", df);
    p.end();
}
static const char* menuGetFps() {
    static char buf[8];
    snprintf(buf, sizeof(buf), "%dfps", ltc.fps());
    return buf;
}
static void menuCycleFps() {
    static const uint8_t vals[] = {24, 25, 30, 50, 60};
    uint8_t cur = ltc.fps();
    uint8_t next = vals[0];
    for (uint8_t i = 0; i < 5; i++) {
        if (vals[i] == cur) { next = vals[(i + 1) % 5]; break; }
    }
    menuSaveFps(next, ltc.dropFrame());
}
static const char* menuGetDf() {
    return ltc.dropFrame() ? "On" : "Off";
}
static void menuToggleDf() {
    menuSaveFps(ltc.fps(), !ltc.dropFrame());
}
static const char* menuGetAutoFps() { return webui.autoFps() ? "Auto" : "Man"; }
static void menuToggleAutoFps() { webui.setAutoFps(!webui.autoFps()); }
static const char* menuGetLtcOut() { return webui.ltcEnabled() ? "On" : "Off"; }
static const char* menuGetOled()   { return webui.oledEnabled() ? "On" : "Off"; }
static const char* menuGetWifi()   { return webui.wifiEnabled() ? "On" : "Off"; }
static void menuToggleLtcOut() { webui.setLtcEnabled(!webui.ltcEnabled()); }
static void menuToggleOled()   { webui.setOledEnabled(!webui.oledEnabled()); oled.forceRedraw(); }
static void menuToggleWifi()   { webui.setWifiEnabled(!webui.wifiEnabled()); }
static void menuExit()         { menu.hide(); oled.forceRedraw(); }
static void menuRestart()      { menu.hide(); _pendingReboot = true; _rebootTimer = millis(); }

#if TCWL_LTC
// ── LTC-specific menu helpers ─────────────────────────────────
static const char* menuGetMode() {
    return bleGetMode() == TCWL_MODE_LTC_MASTER ? "Master" : "Slave";
}
static void menuToggleMode() {
    bleSetMode(bleGetMode() == TCWL_MODE_LTC_MASTER ? TCWL_MODE_LTC : TCWL_MODE_LTC_MASTER);
    menu.hide();
    _pendingReboot = true;
    _rebootTimer = millis();
}
static const char* menuGetMatrix() { return webui.matrixEnabled() ? "On" : "Off"; }
static const char* menuGetBright() {
    static char buf[4];
    snprintf(buf, sizeof(buf), "%d", webui.brightness());
    return buf;
}
static void menuToggleMatrix() { webui.setMatrixEnabled(!webui.matrixEnabled()); }
static void menuCycleBright()  { webui.setBrightness((webui.brightness() + 1) % 16); }

static const char* menuGetRole() { return masterRoleStr(getMasterRole()); }
static void menuCycleRole() {
    int r = getMasterRole();
    r = (r + 1) % 3;
    setMasterRole(r);
    _delayedRecycle = true;
    _lastRecycle = millis();
}
#endif

// ── Shared TC editor (both LTC and HDMI) ─────────────────────
static void menuSetTc() {
    menu.hide();
    _editingTc = true;
    _editField = TC_DD;
    _editVals[0] = ltc.dd();
    _editVals[1] = ltc.hh();
    _editVals[2] = ltc.mm();
    _editVals[3] = ltc.ss();
    _editVals[4] = ltc.ff();
}

static const char* menuGetMaster() {
    return bleTimecodeConnected() ? "CON" : "---";
}

static void menuSelectMaster() {
    menu.hide();
    _selectingMaster = true;
    _menuScanCount = 0;
    _menuScanCursor = 0;
    _menuScanScroll = 0;
    if (bleTimecodeConnected()) {
        // Show connected master with Disconnect option
    } else {
        bleTimecodeStartScan();
    }
    // Consume the btnOk release that triggered this callback so the
    // _selectingMaster overlay doesn't see a stale release event.
    btnOk.read();
}

static void handleTcEditorInput() {
    if (!_editingTc) return;
    if (btnUp.pressed()) {
        static const uint8_t maxVals[] = {99, 23, 59, 59, 59};
        if (_editVals[_editField] < maxVals[_editField]) _editVals[_editField]++;
    }
    if (btnDown.pressed()) {
        if (_editVals[_editField] > 0) _editVals[_editField]--;
    }
    if (btnOk.released()) {
        if (_editField >= TC_FF) {
            ltc.setTime(_editVals[1], _editVals[2], _editVals[3], _editVals[4]);
            ltc.setDd(_editVals[0]);
#if RTC_ENABLE
            if (rtcPresent) {
                rtc.setTime(_editVals[1], _editVals[2], _editVals[3]);
                rtcHH = _editVals[1]; rtcMM = _editVals[2]; rtcSS = _editVals[3];
                lastRtcSyncMs = millis();
            }
#endif
            _editingTc = false;
        } else {
            _editField = static_cast<TcEditField>(_editField + 1);
        }
    }
    if (btnCancel.released()) {
        _editingTc = false;
    }
}

#if TCWL_LTC
static void menuBuildItems() {
    menu.clear();
    menu.addItem("Master",    menuGetMaster, menuSelectMaster);
    menu.addItem("FPS",       menuGetFps,    menuCycleFps);
    menu.addItem("DropFr",    menuGetDf,     menuToggleDf);
    menu.addItem("FPS Mode",  menuGetAutoFps,menuToggleAutoFps);
    menu.addItem("Mode",      menuGetMode,   menuToggleMode);
    menu.addItem("LTC Role",  menuGetRole,   menuCycleRole);
    menu.addItem("Set TC",    nullptr,       menuSetTc);
    menu.addItem("LTC Out",   menuGetLtcOut, menuToggleLtcOut);
    menu.addItem("WiFi",      menuGetWifi,   menuToggleWifi);
    menu.addItem("OLED",      menuGetOled,   menuToggleOled);
    menu.addItem("Matrix",    menuGetMatrix, menuToggleMatrix);
    menu.addItem("Bright",    menuGetBright, menuCycleBright);
    menu.addItem("Restart",   nullptr,       menuRestart);
    menu.addItem("Exit",      nullptr,       menuExit);
    menu.setTimeout(15000);
}
#endif

#if TCWL_HDMI
static void menuBuildItems() {
    menu.clear();
    menu.addItem("FPS",       menuGetFps,    menuCycleFps);
    menu.addItem("DropFr",    menuGetDf,     menuToggleDf);
    menu.addItem("FPS Mode",  menuGetAutoFps,menuToggleAutoFps);
    menu.addItem("Set TC",    nullptr,       menuSetTc);
    menu.addItem("LTC Out",   menuGetLtcOut, menuToggleLtcOut);
    menu.addItem("WiFi",      menuGetWifi,   menuToggleWifi);
    menu.addItem("OLED",      menuGetOled,   menuToggleOled);
    menu.addItem("Restart",   nullptr,       menuRestart);
    menu.addItem("Exit",      nullptr,       menuExit);
    menu.setTimeout(15000);
}
#endif
#endif

// ---------------------------------------------------------------------------
// Battery voltage
// ---------------------------------------------------------------------------
#if BAT_ADC_PIN >= 0
static void initBatteryAdc() {
    analogReadResolution(12);
    // Force ADC pin init (analogSetPinAttenuation requires the pin to be
    // registered with periman first; analogRead auto-registers it).
    analogRead(BAT_ADC_PIN);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
}
static uint8_t readBatteryPct() {
    static unsigned long lastRead = 0;
    static uint8_t cached = 255;
    unsigned long now = millis();
    if (now - lastRead > 10000) {
        lastRead = now;
        analogRead(BAT_ADC_PIN);
        analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
        delay(1);
        int raw = analogRead(BAT_ADC_PIN);
        uint32_t mv = analogReadMilliVolts(BAT_ADC_PIN);
        float vPin = mv > 0 ? mv / 1000.0f : raw * 3.3f / 4095.0f;
        float vBat = vPin * BAT_DIVIDER;
        float pct = (vBat - 3.3f) / (4.2f - 3.3f) * 100.0f;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;
        cached = (uint8_t)pct;
    }
    return cached;
}
#else
static void initBatteryAdc() {}
static uint8_t readBatteryPct() { return 255; }
#endif

// ---------------------------------------------------------------------------
// Common: print reset reason
// ---------------------------------------------------------------------------
static void printResetReason() {
    const char *reason;
    switch (esp_reset_reason()) {
        case ESP_RST_UNKNOWN:    reason = "unknown";          break;
        case ESP_RST_POWERON:    reason = "power-on";         break;
        case ESP_RST_SW:         reason = "software reset";   break;
        case ESP_RST_PANIC:      reason = "panic (crash)";    break;
        case ESP_RST_INT_WDT:    reason = "interrupt WDT";    break;
        case ESP_RST_TASK_WDT:   reason = "task WDT";         break;
        case ESP_RST_WDT:        reason = "other watchdog";   break;
        case ESP_RST_BROWNOUT:   reason = "brownout";         break;
        case ESP_RST_SDIO:       reason = "SDIO";             break;
        default:                 reason = "?";                break;
    }
    Serial.print(F("Last reset: "));
    Serial.println(reason);
}

// ---------------------------------------------------------------------------
// Common: config dump
// ---------------------------------------------------------------------------
static void printConfig() {
    Serial.println(F("── CONFIG ──────────────────────────────"));
    Serial.print(F("  MODE              "));
#if TCWL_CLAP
    Serial.println(F("CLAP"));
#elif TCWL_HDMI
    Serial.println(F("HDMI"));
#else
    if (bleGetMode() == TCWL_MODE_LTC_MASTER) {
        Serial.print(F("LTC (master — "));
        Serial.print(masterRoleStr(getMasterRole()));
        Serial.println(F(")"));
    } else {
        Serial.println(F("LTC (slave — BLE client)"));
    }
#endif
#ifndef TCWL_CLAP
    Serial.print(F("  LTC_ENABLED       ")); Serial.println(webui.ltcEnabled() ? "1" : "0");
    Serial.print(F("  LTC_OUT_PIN       ")); Serial.println(LTC_OUT_PIN);
    Serial.print(F("  LTC_FPS           ")); Serial.println(LTC_FPS);
    Serial.print(F("  LTC_DROP_FRAME    ")); Serial.println(LTC_DROP_FRAME);
#endif
    Serial.print(F("  FPS_AUTO_DETECT   ")); Serial.println(FPS_AUTO_DETECT);
#ifdef REVERSE_ENGINEER_MODE
    Serial.print(F("  REVERSE_ENGINEER  ")); Serial.println(REVERSE_ENGINEER_MODE);
#endif
#if TCWL_HDMI
    Serial.print(F("  TC358743 I2C      SDA=")); Serial.print(TC_I2C_SDA_PIN);
    Serial.print(F(" SCL=")); Serial.print(TC_I2C_SCL_PIN);
    Serial.print(F(" ADDR=0x")); Serial.println(TC_I2C_ADDR, HEX);
    Serial.print(F("  TC_RESET_PIN      ")); Serial.println(TC_RESET_PIN);
#endif
#if OLED_ENABLE
    Serial.print(F("  OLED_ENABLED      ")); Serial.println(webui.oledEnabled() ? "1" : "0");
    Serial.print(F("  OLED_I2C          SDA=")); Serial.print(OLED_I2C_SDA_PIN);
    Serial.print(F(" SCL=")); Serial.print(OLED_I2C_SCL_PIN);
    Serial.print(F(" ADDR=0x")); Serial.println(OLED_I2C_ADDR, HEX);
#endif
    Serial.print(F("  RTC_ENABLE        ")); Serial.println(RTC_ENABLE);
    if (RTC_ENABLE) {
        Serial.print(F("  RTC I2C           SDA=")); Serial.print(RTC_I2C_SDA_PIN);
        Serial.print(F(" SCL=")); Serial.print(RTC_I2C_SCL_PIN);
        Serial.print(F(" ADDR=0x")); Serial.println(RTC_I2C_ADDR, HEX);
    }
#if WEBUI_ENABLE
    Serial.print(F("  WEBUI_ENABLE      1    AP=")); Serial.print(WEBUI_AP_SSID);
    if (WEBUI_AP_PASSWORD) { Serial.print(F(" (pass)")); }
    Serial.println();
    if (WEBUI_STA_SSID[0]) {
        Serial.print(F("  STA               ")); Serial.println(WEBUI_STA_SSID);
    } else {
        Serial.println(F("  STA               (none)"));
    }
#endif
    Serial.println(F("────────────────────────────────────────"));
    Serial.println();
}

// ===========================================================================
// Master-specific: I2C/HDMI setup
// ===========================================================================
#if TCWL_HDMI
static void hdmiSetup() {
    // Load persisted FPS/DF from NVS
    {
        Preferences prefs;
        prefs.begin("ltc", false);
        uint8_t savedFps = prefs.getUChar("fps", LTC_FPS);
        bool savedDf = prefs.getBool("df", LTC_DROP_FRAME);
        prefs.end();
        if (savedFps != LTC_FPS || savedDf != LTC_DROP_FRAME) {
            ltc.setFps(savedFps, savedDf);
            framePollMs = 1000 / savedFps;
        }
    }

    // Start LTC encoder
#ifndef SKIP_LTC_TIMER
    ltc.begin();
    ltc.setTime(1, 0, 0, 0);
    Serial.print(F("LTC encoder "));
    Serial.println(webui.ltcEnabled() ? F("started") : F("disabled"));
#else
    Serial.println(F("LTC timer SKIPPED (debug)"));
#endif

    // TC358743 probe — initialises Wire for all I2C devices on the shared bus
    Serial.printf("I2C: SDA=%d SCL=%d RST=%d\n",
                  TC_I2C_SDA_PIN, TC_I2C_SCL_PIN, TC_RESET_PIN);
    tcPresent = tc.begin(TC_I2C_SDA_PIN, TC_I2C_SCL_PIN, TC_RESET_PIN, TC_REFCLK_HZ);
    if (!tcPresent) {
        Wire.begin(TC_I2C_SDA_PIN, TC_I2C_SCL_PIN, 50000);
        static const uint8_t altAddrs[] = { 0x0F, 0x1F, 0x3D };
        for (int i = 0; i < 3; i++) {
            if (altAddrs[i] == TC_I2C_ADDR) continue;
            Wire.beginTransmission(altAddrs[i]);
            if (Wire.endTransmission() == 0) {
                Serial.printf("TC358743 found at alt address 0x%02X\n", altAddrs[i]);
                tcPresent = true;
                break;
            }
        }
    }

    // Full I2C bus scan
    Serial.print(F("I2C bus: "));
    {
        int nFound = 0;
        for (uint8_t addr = 1; addr < 127; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                if (nFound) Serial.print(F(", "));
                Serial.printf("0x%02X", addr);
                nFound++;
                if (addr == 0x0F) Serial.print(F("(TC358743)"));
                else if (addr == 0x1F) Serial.print(F("(TC358743?)"));
                else if (addr == 0x3C) Serial.print(F("(OLED)"));
                else if (addr == 0x3D) Serial.print(F("(TC358743?)"));
                else if (addr == 0x68) Serial.print(F("(DS3231)"));
            }
        }
        if (nFound == 0) Serial.print(F("(none) — I2C bus not working or no devices"));
        Serial.println();
    }
    if (!tcPresent) {
        Serial.println(F("ERROR: TC358743 not responding — HDMI disabled."));
    } else {
        Serial.println(F("TC358743 detected OK."));
    }

    if (!tcPresent) {
        Serial.println(F("Skipping TC358743 — running in standalone mode."));
    }

    // RTC & OLED are on their own I2C bus — always probe regardless of TC
#if RTC_ENABLE
    rtcPresent = rtc.begin(RTC_I2C_SDA_PIN, RTC_I2C_SCL_PIN);
    if (rtcPresent) {
        Serial.println(F("DS3231 RTC detected."));
        if (rtc.readTime(rtcHH, rtcMM, rtcSS)) {
            ltc.setTime(rtcHH, rtcMM, rtcSS, 0);
            lastRtcReadMs = lastRtcSyncMs = millis();
            Serial.print(F("RTC initial time: "));
            if (rtcHH < 10) Serial.print('0');
            Serial.print(rtcHH); Serial.print(':');
            if (rtcMM < 10) Serial.print('0');
            Serial.print(rtcMM); Serial.print(':');
            if (rtcSS < 10) Serial.print('0');
            Serial.println(rtcSS);
        }
    } else {
        Serial.println(F("No RTC detected."));
        ltc.setTime(1, 0, 0, 0);
    }
#else
    ltc.setTime(1, 0, 0, 0);
#endif

#if OLED_ENABLE
    if (webui.oledEnabled()) {
        if (oled.begin()) {
            Serial.println(F("OLED display initialized."));
        } else {
            Serial.println(F("OLED not detected — skipping."));
        }
    }
#endif

#if OLED_ENABLE && BTN_UP_PIN >= 0
    btnUp.begin();
    btnDown.begin();
    btnOk.begin();
    btnCancel.begin();
    menuBuildItems();
#endif

#if FPS_AUTO_DETECT
    Serial.println(F("FPS auto-detect enabled."));
#endif

#if MAX7219_ENABLE
    Serial.print(F("Starting MAX7219 display... "));
    mx7219.begin();
    mx7219.setIntensity(webui.brightness());
    if (webui.matrixEnabled()) {
        mx7219.showText("TC-WL-HDMI");
        delay(3000);
    }
    Serial.print(MAX7219_NUM_MODULES);
    Serial.println(F(" modules initialized."));
#endif
}
#endif // TCWL_HDMI

// ===========================================================================
// Master-specific: loop
// ===========================================================================
#if TCWL_HDMI
static void hdmiLoop() {
    unsigned long now = millis();

#if OLED_ENABLE && BTN_UP_PIN >= 0
    btnUp.read();
    btnDown.read();
    btnOk.read();
    btnCancel.read();

    if (_editingTc) {
        handleTcEditorInput();
    } else if (!menu.active()) {
        if (btnUp.pressed() || btnDown.pressed() || btnOk.pressed() || btnCancel.pressed()) {
            menu.show();
        }
    }

    if (_editingTc) {
        // editor handles its own buttons
    } else if (menu.active()) {
        if (btnUp.pressed())      { menu.up(); _delayedRecycle = false; }
        if (btnDown.pressed())    { menu.down(); _delayedRecycle = false; }
        if (btnOk.released())     menu.ok(btnOk.heldFor(2000));
        if (btnCancel.released()) { menu.cancel(); oled.forceRedraw(); _delayedRecycle = false; }
        if (!menu.tick())         { oled.forceRedraw(); _delayedRecycle = false; }
    }
    if (_delayedRecycle && !menu.active()) {
        _delayedRecycle = false;
    }
    if (_delayedRecycle && millis() - _lastRecycle >= 2000) {
        _delayedRecycle = false;
        menu.hide();
        _pendingReboot = true;
        _rebootTimer = millis();
    }
#endif

    // AP health check
    static unsigned long lastApDiag = 0;
    if (now - lastApDiag >= 30000) {
        lastApDiag = now;
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
            uint8_t ch;
            wifi_second_chan_t sec;
            esp_wifi_get_channel(&ch, &sec);
            Serial.print(F("AP IP="));
            Serial.print(WiFi.softAPIP());
            Serial.print(F(" ch="));
            Serial.print(ch);
            Serial.print(F(" stations="));
            Serial.println(WiFi.softAPgetStationNum());
        }
    }

#if REVERSE_ENGINEER_MODE
    if (tcPresent) {
        runReverseEngineerDump();
        delay(500);
        return;
    } else {
        static bool tcAbsentWarned = false;
        if (!tcAbsentWarned) {
            tcAbsentWarned = true;
            Serial.println(F("TC358743 absent — web UI still available."));
        }
    }
#endif

    bool locked = tcPresent ? tc.hasSignal() : false;

    // Track whether we've ever seen a signal — used by the retry block below
    // to decide whether to do a full chip reset (first-time acquisition only).
    static bool everLocked = false;
    if (locked) everLocked = true;

    // When no signal detected, make sure HPD is in manual mode.
    // Interlock mode (0x10) creates a deadlock: HPD=low → no TMDS → PLL unlocked → HPD stays low.
    if (tcPresent && !locked) {
        uint8_t hpdCtl = tc.readReg8(HPD_CTL);
        if ((hpdCtl & 0x10) && !(hpdCtl & 0x01)) {
            tc.writeReg8(HPD_CTL, 0x01);
            delay(200);
        }
    }

    // Periodic HPD retry when TMDS stays 0 — non-blocking state machine
    static uint8_t hpdStep = 0;          // 0=idle, 1=HPDlow, 2=HPDhigh, 3=EDID, 4=check, 5=resetLow, 6=resetHigh, 7=reinit
    static unsigned long hpdTimer = 0;
    static unsigned long lastHpdRetry = 0;

    if (tcPresent && !locked) {
        // Start a new retry cycle every 10s when idle
        if (hpdStep == 0 && now - lastHpdRetry > 10000) {
            lastHpdRetry = now;
            uint8_t s = tc.readReg8(SYS_STATUS);
            if (s != 0x01) Serial.printf("H retry SYS_STATUS=0x%02X\n", s);
            tc.writeReg8(HPD_CTL, 0x00);
            hpdStep = 1;
            hpdTimer = now + 200;
        }

        // Non-blocking state machine spreads delays across loop iterations
        switch (hpdStep) {
        case 1: // HPD low duration
            if (now >= hpdTimer) {
                tc.writeReg8(HPD_CTL, 0x01);
                hpdStep = 2;
                hpdTimer = now + 2000;
            }
            break;
        case 2: // HPD high, waiting for source TMDS
            if (now >= hpdTimer) {
                tc.writeReg8(EDID_MODE, MASK_EDID_MODE_DDC2B | MASK_EDID_MODE_E_DDC);
                hpdStep = 3;
                hpdTimer = now + 100;
            }
            break;
        case 3: // EDID settle
            if (now >= hpdTimer) {
                uint8_t s = tc.readReg8(SYS_STATUS);
                static uint8_t resetAttempts = 0;
                if (!(s & 0x02) && ((s & 0x0C) != 0x0C) && !everLocked && resetAttempts < 3) {
                    resetAttempts++;
                    Serial.printf("H reset chip %d/3\n", resetAttempts);
                    pinMode(TC_RESET_PIN, OUTPUT);
                    digitalWrite(TC_RESET_PIN, LOW);
                    hpdStep = 4;
                    hpdTimer = now + 200;
                } else {
                    if (!(s & 0x02) && ((s & 0x0C) != 0x0C) && everLocked) {
                        // unlocked after lock — silent, free-run continues
                    }
                    hpdStep = 0;
                }
            }
            break;
        case 4: // Reset pin low
            if (now >= hpdTimer) {
                digitalWrite(TC_RESET_PIN, HIGH);
                hpdStep = 5;
                hpdTimer = now + 500;
            }
            break;
        case 5: // Reset pin high
            if (now >= hpdTimer) {
                tcPresent = tc.begin(TC_I2C_SDA_PIN, TC_I2C_SCL_PIN, TC_RESET_PIN, TC_REFCLK_HZ);
                if (!tcPresent) Serial.println(F("H: chip dead after reset"));
                hpdStep = 0;
            }
            break;
        }
    } else {
        hpdStep = 0;  // reset state machine when locked or tc absent
    }

    // Fast poll: detect new frames via SYS_INT
    if (now - lastFastPoll >= FAST_POLL_MS) {
        lastFastPoll = now;

        if (locked) {
            uint16_t sysInt = tc.readReg16(SYS_INT);
            if (sysInt) {
                tc.writeReg16(SYS_INT, 0xFFFF);

                if (sysInt & 0x8000) {
                    uint32_t nowUs = micros();

#if FPS_AUTO_DETECT
                    if (!fpsDetected) {
                        if (lastFrameUs != 0) {
                            uint32_t interval = nowUs - lastFrameUs;
                            if (interval > 1000 && interval < 100000 && intervalCount < 30) {
                                intervals[intervalCount++] = interval;
                            }
                        }
                        lastFrameUs = nowUs;
                    }
#endif

                    Gh5Timecode gh5tc;
                    if (decodeGh5Timecode(tc, gh5tc)) {
                        ltc.setTime(gh5tc.hh, gh5tc.mm, gh5tc.ss, gh5tc.ff);
#if RTC_ENABLE
                        if (rtcPresent && gh5tc.ss != rtcSS) {
                            rtc.setTime(gh5tc.hh, gh5tc.mm, gh5tc.ss);
                            rtcHH = gh5tc.hh; rtcMM = gh5tc.mm; rtcSS = gh5tc.ss;
                            lastRtcSyncMs = now;
                        }
#endif
                    }
                }
            }
        }
    }

    // Frame-synced processing
    if (now - lastFramePoll >= framePollMs) {
        lastFramePoll += framePollMs;
        if (now - lastFramePoll >= framePollMs) lastFramePoll = now;

#if FPS_AUTO_DETECT
        if (webui.autoFps() && !fpsDetected && tryDetectFps()) {
        }
#endif

        bool hdmiOk = locked && tcPresent && tc.isHdmiMode();

        static bool prevHdmiOk = false;
        if (hdmiOk != prevHdmiOk) {
            tc.enableCsiStream(hdmiOk);
            prevHdmiOk = hdmiOk;
        }

        uint16_t frames = ltc.framesCompleted();
        // Always tick LTC when frames are available (free-run or HDMI)
        if (frames > 0) {
            while (frames--) {
                ltc.tick();
#if MAX7219_ENABLE
#if WEBUI_ENABLE
                if (webui.matrixEnabled())
#endif
                    mx7219.showTimecode(ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff());
#endif
            }
        }
        // Poll GH5 timecode from HDMI VSIF (always, not just on SYS_INT)
        if (hdmiOk) {
            Gh5Timecode gh5tc;
            if (decodeGh5Timecode(tc, gh5tc)) {
                ltc.setTime(gh5tc.hh, gh5tc.mm, gh5tc.ss, gh5tc.ff);
            }
        }
        // Set source label based on HDMI / RTC / free-run
        if (hdmiOk) {
            strcpy(tcSource, "HDMI");
        } else
#if RTC_ENABLE
        if (rtcPresent) {
            if (now - lastRtcReadMs >= 1000) {
                lastRtcReadMs = now;
                if (rtc.readTime(rtcHH, rtcMM, rtcSS)) {
                    lastRtcSyncMs = now;
                }
            }
            unsigned long elapsed = now - lastRtcSyncMs;
            uint8_t ff = (elapsed * ltc.fps()) / 1000;
            if (ff >= ltc.fps()) ff = ltc.fps() - 1;
            ltc.setTime(rtcHH, rtcMM, rtcSS, ff);
            strcpy(tcSource, "RTC");
        } else
#endif
        {
            strcpy(tcSource, "FREE");
        }
        static unsigned long lastTcPrint = 0;
        if (now - lastTcPrint >= 30000) {
            lastTcPrint = now;
            Serial.printf("[TC] %s %02u:%02u:%02u:%02u.%02u hdmiOk=%d tcPresent=%d locked=%d\n",
                          tcSource, ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff(),
                          (int)hdmiOk, tcPresent, locked);
        }

#if FPS_AUTO_DETECT
        if (!locked) resetFpsDetect();
#endif

#if OLED_ENABLE
        if (drawTcEditor()) {
            // editor drawn, skip rest
        } else if (!handleRebootDisplay() && webui.oledEnabled()) {
            bool menuDrawn = false;
#if BTN_UP_PIN >= 0
            if (menu.active()) {
                menu.draw();
                menuDrawn = true;
            }
#endif
            if (!menuDrawn) {
                fmtTcStr(ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff());
                oled.update(tcStr, ltc.fps(), hdmiOk ? 1 : (rtcPresent ? 2 : 0), gDeviceName, webui.autoFps(), "OUT", 0, readBatteryPct(), 'M', bleTimecodeConnectedCount() > 0, webui.wifiEnabled());
            }
        }
#endif
#if MAX7219_ENABLE
        if (webui.matrixEnabled()) {
            mx7219.showTimecode(ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff());
        }
        mx7219.setBleConnected(bleTimecodeConnectedCount() > 0);
#endif
        bleTimecodeUpdate(ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff(), hdmiOk ? 1 : (rtcPresent ? 2 : 0), ltc.fps(), (webui.autoFps() ? 1 : 0) | (1 << 1) | (0 << 2), readBatteryPct());

#if WEBUI_ENABLE
        fmtTcStr(ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff());
        webui.update(ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff(),
                     ltc.fps(), ltc.dropFrame(), hdmiOk, tcSource);
#endif
    }
}
#endif // TCWL_HDMI

// ===========================================================================
// Slave-specific: setup
// ===========================================================================
#if TCWL_LTC
static void ltcSetup() {
#if OLED_ENABLE || RTC_ENABLE
    Wire.begin(TC_I2C_SDA_PIN, TC_I2C_SCL_PIN, 100000);
    delay(50);
#endif

    ltc.begin();
    ltc.setTime(0, 0, 0, 0);

#if defined(TCWL_LTC) && OLED_ENABLE && BTN_UP_PIN >= 0
    btnUp.begin();
    btnDown.begin();
    btnOk.begin();
    btnCancel.begin();
    menuBuildItems();
#endif
#ifndef TCWL_CLAP
    Serial.print(F("LTC encoder "));
    Serial.println(webui.ltcEnabled() ? F("started") : F("disabled"));
#endif

#if OLED_ENABLE
    Serial.printf("  OLED_ENABLED=%d\n", webui.oledEnabled());
    if (webui.oledEnabled()) {
        oled.begin();
        delay(2500);
        Serial.println(F("OLED started"));
    } else {
        Serial.println(F("OLED disabled"));
    }
#endif

#if RTC_ENABLE
    rtcPresent = rtc.begin(RTC_I2C_SDA_PIN, RTC_I2C_SCL_PIN);
    if (rtcPresent) {
        Serial.println(F("DS3231 RTC detected."));
        if (rtc.readTime(rtcHH, rtcMM, rtcSS)) {
            ltc.setTime(rtcHH, rtcMM, rtcSS, 0);
            lastRtcReadMs = lastRtcSyncMs = millis();
            Serial.print(F("RTC initial time: "));
            Serial.printf("%02u:%02u:%02u\n", rtcHH, rtcMM, rtcSS);
        }
    } else {
        Serial.println(F("No RTC detected."));
    }
#endif

#if MAX7219_ENABLE
#ifdef TCWL_CLAP
    // Already initialized at the top of setup() — only (re)apply brightness,
    // which now reflects the NVS value loaded by WebUI::begin().
    mx7219.setIntensity(webui.brightness());
    if (!webui.matrixEnabled()) mx7219.clear();
#else
    mx7219.begin();
    mx7219.setIntensity(webui.brightness());
    if (webui.matrixEnabled()) {
        mx7219.showText("TC-WL-LTC");
        delay(2000);
    }
#endif
    Serial.println(F("MAX7219 started"));
#endif

    if (bleGetMode() == TCWL_MODE_LTC_MASTER) {
        int role = getMasterRole();
        Serial.printf("LTC master role: %s\n", masterRoleStr(role));
        if (role == MASTER_ROLE_IN) {
            ltc.setEnabled(false);
            ltcDecoder.begin();
            ltcDecoder.setCallback(onLtcDecoded);
            Serial.println(F("  IN — decode only"));
        } else if (role == MASTER_ROLE_OUT) {
            ltc.setEnabled(true);
            Serial.println(F("  OUT — generate only"));
        } else {
            ltc.setEnabled(true);
            ltcDecoder.begin();
            ltcDecoder.setCallback(onLtcDecoded);
            Serial.println(F("  BOTH — decode + generate"));
        }
    } else {
        ltc.setEnabled(true);
        bleTimecodeSetCallback(onBleTimecode);
        Serial.println(F("BLE client mode — scanning for server..."));
    }
}

// ===========================================================================
// Slave-specific: loop
// ===========================================================================
static void ltcLoop() {
#if defined(TCWL_LTC) && OLED_ENABLE && BTN_UP_PIN >= 0
    btnUp.read();
    btnDown.read();
    btnOk.read();
    btnCancel.read();

    // Track menu active state to pause BLE auto-connect while browsing
    {
        static bool _prevMenuActive = false;
        if (menu.active() != _prevMenuActive) {
            _prevMenuActive = menu.active();
            bleTimecodeSetMenuActive(menu.active());
        }
    }

    if (_editingTc) {
        handleTcEditorInput();
    } else if (!menu.active() && !_selectingMaster) {
        if (btnUp.pressed() || btnDown.pressed() || btnOk.pressed() || btnCancel.pressed()) {
            menu.show();
            _menuOpenBtn = btnUp.pressed() ? 1 : btnDown.pressed() ? 2 : btnOk.pressed() ? 3 : 4;
        }
    }

    if (_editingTc) {
        // editor handles its own buttons
    } else if (menu.active()) {
        if (btnUp.pressed())      { menu.up(); _delayedRecycle = false; }
        if (btnDown.pressed())    { menu.down(); _delayedRecycle = false; }
        if (btnOk.released() && _menuOpenBtn != 3)     menu.ok(btnOk.heldFor(2000));
        if (btnCancel.released() && _menuOpenBtn != 4) { menu.cancel(); oled.forceRedraw(); _delayedRecycle = false; }
        if (!menu.tick())         { oled.forceRedraw(); _delayedRecycle = false; }
        if (!btnUp.held() && !btnDown.held() && !btnOk.held() && !btnCancel.held())
            _menuOpenBtn = 0;
    }
    if (_delayedRecycle && !menu.active()) {
        _delayedRecycle = false;
    }
    if (_delayedRecycle && millis() - _lastRecycle >= 2000) {
        _delayedRecycle = false;
        menu.hide();
        _pendingReboot = true;
        _rebootTimer = millis();
    }

    // ── Master selection / disconnect mode ──
    if (_selectingMaster) {
        auto &d = oled.display();
        // Connected while no fresh scan data yet → show disconnect screen
        if (bleTimecodeConnected() && !_menuScanCount) {
            d.clearDisplay();
            d.setTextSize(1);
            d.setTextColor(WHITE);
            d.setCursor(0, 0);
            d.print("Connected:");
            const char *name = bleTimecodeConnectedName();
            const char *addr = bleTimecodeConnectedAddress();
            if (name && name[0]) {
                d.setCursor(0, 16);
                d.print(name);
            } else if (addr && addr[0]) {
                d.setCursor(0, 16);
                d.print(addr);
            }
            d.setCursor(0, 48);
            d.print(_menuScanCursor == 0 ? "> " : "  ");
            d.print("[Disconnect]");
            d.setCursor(0, 56);
            d.print(_menuScanCursor == 1 ? "> " : "  ");
            d.print("[Back]");
            d.display();
            if (btnUp.pressed() && _menuScanCursor > 0) _menuScanCursor--;
            if (btnDown.pressed() && _menuScanCursor < 1) _menuScanCursor++;
            if (btnOk.released()) {
                if (_menuScanCursor == 0) {
                    bleTimecodeDisconnect();
                }
                _selectingMaster = false;
                oled.forceRedraw();
            }
        } else if (bleTimecodeScanDone()) {
            if (_menuScanCount == 0) {
                _menuScanCount = bleTimecodeScanResults(_menuScanResults, 16);
            }
            if (_menuScanCount > 0) {
                d.clearDisplay();
                d.setTextSize(1);
                d.setTextColor(WHITE);
                uint8_t total = _menuScanCount + 1; // +1 for "Back"
                for (uint8_t i = 0; i < 4 && (i + _menuScanScroll) < total; i++) {
                    uint8_t idx = i + _menuScanScroll;
                    d.setCursor(0, i * 16);
                    if (idx == _menuScanCursor) d.print("> ");
                    else d.print("  ");
                    if (idx < _menuScanCount)
                        d.print(_menuScanResults[idx].name);
                    else
                        d.print("[Back]");
                }
                d.display();

                if (btnUp.pressed() && _menuScanCursor > 0) {
                    _menuScanCursor--;
                    if (_menuScanCursor < _menuScanScroll) _menuScanScroll--;
                }
                if (btnDown.pressed() && _menuScanCursor < total - 1) {
                    _menuScanCursor++;
                    if (_menuScanCursor >= _menuScanScroll + 4) _menuScanScroll++;
                }
                if (btnOk.released()) {
                    if (_menuScanCursor < _menuScanCount) {
                        bleTimecodeSelect(_menuScanResults[_menuScanCursor].address, _menuScanResults[_menuScanCursor].name);
                    }
                    _selectingMaster = false;
                    oled.forceRedraw();
                }
            } else {
                d.clearDisplay();
                d.setTextSize(1);
                d.setTextColor(WHITE);
                d.setCursor(0, 0);
                d.print("> [Retry]");
                d.setCursor(0, 16);
                d.print("  [Back]");
                d.display();
                if (btnOk.released()) {
                    if (_menuScanCursor == 0) {
                        // Retry
                        _selectingMaster = false;
                        _menuScanCursor = 0;
                        _menuScanScroll = 0;
                        menuSelectMaster();
                        return;
                    } else {
                        _selectingMaster = false;
                        oled.forceRedraw();
                    }
                }
                if (btnUp.pressed() && _menuScanCursor > 0) _menuScanCursor--;
                if (btnDown.pressed() && _menuScanCursor < 1) _menuScanCursor++;
            }
        } else {
            d.clearDisplay();
            d.setTextSize(2);
            d.setTextColor(WHITE);
            d.setCursor(8, 24);
            d.print("Scan...");
            d.display();
        }
        if (btnCancel.released()) {
            _selectingMaster = false;
            oled.forceRedraw();
        }
        return; // Skip OLED update below — we drew our own
    }
#endif

    if (bleGetMode() == TCWL_MODE_LTC_MASTER) {
        int role = getMasterRole();
        unsigned long now = millis();

        // Poll decoder in IN or BOTH mode
        bool decoderActive = (role != MASTER_ROLE_OUT);
        if (decoderActive) {
            ltcDecoder.poll();
            if (ltcDecoder.locked()) lastDecodedFrameMs = now;
        }

        bool signalOk = decoderActive && ltcDecoder.locked() && (now - lastDecodedFrameMs < 2000);

        uint16_t frames = ltc.framesCompleted();
        if (frames > 0) {
            if ((frames & 0xF) == 0) {
                Serial.printf("[TC] frames=%u tc=%02u:%02u:%02u:%02u:%02u locked=%d\n",
                    frames, ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff(), ltcDecoder.locked());
            }

            if (role == MASTER_ROLE_IN) {
                // IN: free-run when no signal from decoder
                if (!signalOk) {
                    while (frames--) ltc.tick();
                }
                strcpy(tcSource, signalOk ? "LTC-IN" : "FREE");
            } else if (role == MASTER_ROLE_OUT) {
                // OUT: always free-run
                while (frames--) ltc.tick();
                strcpy(tcSource, "GEN");
            } else {
                // BOTH: free-run when no signal
                if (!signalOk) {
                    while (frames--) ltc.tick();
                }
                strcpy(tcSource, signalOk ? "LTC-IN" : "FREE");
            }

            bleTimecodeUpdate(ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff(), (decoderActive && ltcDecoder.locked()) ? 1 : (rtcPresent ? 2 : 0), ltc.fps(), (webui.autoFps() ? 1 : 0) | (1 << 1) | (static_cast<uint8_t>(role == MASTER_ROLE_IN ? 1 : (role == MASTER_ROLE_BOTH ? 2 : 0)) << 2), readBatteryPct());
        } else if (role == MASTER_ROLE_IN) {
            // IN mode, encoder off — broadcast BLE periodically
            static unsigned long lastBleBcast = 0;
            if (now - lastBleBcast >= 1000) {
                lastBleBcast = now;
                bleTimecodeUpdate(ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff(), (decoderActive && ltcDecoder.locked()) ? 1 : (rtcPresent ? 2 : 0), ltc.fps(), (webui.autoFps() ? 1 : 0) | (1 << 1) | (static_cast<uint8_t>(role == MASTER_ROLE_IN ? 1 : (role == MASTER_ROLE_BOTH ? 2 : 0)) << 2), readBatteryPct());
            }
        }

        if (!decoderActive) {
            strcpy(tcSource, "GEN");
        }

#if OLED_ENABLE
        if (drawTcEditor()) {
            // editor drawn, skip rest
        } else if (!handleRebootDisplay() && webui.oledEnabled()) {
            bool menuDrawn = false;
#if defined(TCWL_LTC) && BTN_UP_PIN >= 0
            if (menu.active()) {
                menu.draw();
                menuDrawn = true;
            }
#endif
            if (!menuDrawn) {
                fmtTcStr(ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff());
                oled.update(tcStr, ltc.fps(), (decoderActive && ltcDecoder.locked()) ? 1 : (rtcPresent ? 2 : 0),
                            gDeviceName, webui.autoFps(), masterRoleStr(role), 0,
                            readBatteryPct(), 'M', bleTimecodeConnectedCount() > 0, webui.wifiEnabled());
            }
        }
#endif

#if MAX7219_ENABLE
        if (webui.matrixEnabled()) {
            mx7219.showTimecode(ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff());
        }
        mx7219.setBleConnected(bleTimecodeConnected());
#endif

#if WEBUI_ENABLE
        webui.update(ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff(),
                     ltc.fps(), ltc.dropFrame(), decoderActive ? ltcDecoder.locked() : false, tcSource);
#endif
    } else {
        bleTimecodePoll();

        uint16_t frames = ltc.framesCompleted();
        if (frames > 0) {
#if RTC_ENABLE
            if (!bleTimecodeConnected() && rtcPresent) {
                unsigned long now = millis();
                if (now - lastRtcReadMs >= 1000) {
                    lastRtcReadMs = now;
                    if (rtc.readTime(rtcHH, rtcMM, rtcSS)) {
                        lastRtcSyncMs = now;
                    }
                }
                unsigned long elapsed = now - lastRtcSyncMs;
                uint8_t ff = (elapsed * ltc.fps()) / 1000;
                if (ff >= ltc.fps()) ff = ltc.fps() - 1;
                ltc.setTime(rtcHH, rtcMM, rtcSS, ff);
            } else
#endif
            {
                while (frames--) ltc.tick();
            }

            // Send BLE notification at frame rate only — calling this on every
            // loop iteration floods the NimBLE host and exhausts the mbuf pool
            // when a subscriber (e.g. the Android app) is connected.
            bleTimecodeUpdate(ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff(), bleTimecodeConnected() ? 3 : (rtcPresent ? 2 : 0), ltc.fps(), (webui.autoFps() ? 1 : 0) | (0 << 2), readBatteryPct());
        }

#if OLED_ENABLE
        if (drawTcEditor()) {
            // editor drawn, skip rest
        } else if (!handleRebootDisplay() && webui.oledEnabled()) {
            bool menuDrawn = false;
#if defined(TCWL_LTC) && BTN_UP_PIN >= 0
            if (menu.active()) {
                menu.draw();
                menuDrawn = true;
            }
#endif
            if (!menuDrawn) {
                fmtTcStr(ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff());
                uint8_t lockSt = bleTimecodeConnected() ? 3 : (rtcPresent ? 2 : 0);
                oled.update(tcStr, ltc.fps(), lockSt, gDeviceName, webui.autoFps(), "OUT", 0, readBatteryPct(), 'S', bleTimecodeConnectedCount() > 0, webui.wifiEnabled());
            }
        }
#endif

#if MAX7219_ENABLE
        if (webui.matrixEnabled()) {
            mx7219.showTimecode(ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff());
        }
        mx7219.setBleConnected(bleTimecodeConnected());
#endif

#if WEBUI_ENABLE
        webui.update(ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff(),
                     ltc.fps(), ltc.dropFrame(), bleTimecodeConnected(),
                     bleTimecodeConnected() ? "BLE" : "SCAN");
#endif
    }
}
#endif // TCWL_LTC

// ===========================================================================
// setup()
// ===========================================================================
void setup() {
    // Drive MAX7219 SPI pins safe IMMEDIATELY — before any delays, before
    // Serial.  On ESP32-C3, GPIO 2 (MAX7219 DIN) is a strapping pull-up, and
    // CS/CLK are undefined during boot; if CS floats low the MAX7219 latches
    // garbage and lights all LEDs.  Low-power USB ports exacerbate this because
    // the voltage ramp is slower.
#if MAX7219_ENABLE
    pinMode(MAX7219_CS_PIN,  OUTPUT);
    digitalWrite(MAX7219_CS_PIN, HIGH);
    pinMode(MAX7219_DIN_PIN, OUTPUT);
    digitalWrite(MAX7219_DIN_PIN, LOW);
    pinMode(MAX7219_CLK_PIN, OUTPUT);
    digitalWrite(MAX7219_CLK_PIN, LOW);
#endif

    Serial.begin(115200);
    for (int i = 0; i < 200 && !Serial; i++) delay(10);

    pinMode(LTC_OUT_PIN, OUTPUT);
    delay(2000);
    printResetReason();
    initBatteryAdc();

    // TC-WL-CLAP: initialize the LED matrix, apply saved brightness, clear it,
    // then show the splash while the rest of the system boots.
#if MAX7219_ENABLE && defined(TCWL_CLAP)
    {
        Preferences prefs;
        prefs.begin("webui", false);
        uint8_t brightness = prefs.getUChar("brightness", 4);
        prefs.end();
        mx7219.begin();
        mx7219.setIntensity(brightness);
        mx7219.clear();
        mx7219.showText("TC-WL-CLAP");
    }
#endif

    Wire.setPins(TC_I2C_SDA_PIN, TC_I2C_SCL_PIN);

    Wire.begin(TC_I2C_SDA_PIN, TC_I2C_SCL_PIN, 100000);

#if OLED_ENABLE
    oled.begin();
#endif

    // On cold power-on the C6 ESP-Hosted coprocessor can take 8-12s to boot
    // its firmware before it responds to SDIO enumeration. Warm resets skip
    // this delay since the C6 stays running.
    if (esp_reset_reason() == ESP_RST_POWERON) {
        Serial.print(F("Waiting for C6 coprocessor... "));
        for (int i = 8; i > 0; i--) {
            Serial.printf("%d ", i);
            delay(1000);
        }
        Serial.println(F("done"));
    }

    Serial.print(F("TC-WL starting in "));
#if TCWL_CLAP
    Serial.println(F("CLAP mode..."));
#elif TCWL_HDMI
    Serial.println(F("HDMI mode..."));
#else
    Serial.println(F("LTC mode..."));
#endif
    Serial.println();

    // Start WiFi AP + web server BEFORE LTC timer
#if WEBUI_ENABLE
    // Build AP SSID from BLE name or env name with last 4 MAC digits
    static char apSsid[33];
    {
        Preferences blePrefs;
        blePrefs.begin("ble", false);
        uint8_t mac[6] = {};
        esp_efuse_mac_get_default(mac);
        char macSuffix[8];
        snprintf(macSuffix, sizeof(macSuffix), "%02X%02X", mac[4], mac[5]);

#if TCWL_CLAP
        char defaultClapName[33];
        snprintf(defaultClapName, sizeof(defaultClapName), "TC-WL-CLAP-%s", macSuffix);
        String bleName = blePrefs.isKey("ltc_name") ? blePrefs.getString("ltc_name", "") : defaultClapName;
        strncpy(apSsid, bleName.c_str(), sizeof(apSsid) - 1);
        apSsid[sizeof(apSsid) - 1] = '\0';
#elif TCWL_HDMI
        String bleName = blePrefs.isKey("name") ? blePrefs.getString("name", "") : "TC-WL-HDMI";
        if (bleName == "TC-WL-HDMI") {
            snprintf(apSsid, sizeof(apSsid), "TC-WL-HDMI-%s", macSuffix);
        } else {
            strncpy(apSsid, bleName.c_str(), sizeof(apSsid) - 1);
            apSsid[sizeof(apSsid) - 1] = '\0';
        }
#else
        if (bleGetMode() == TCWL_MODE_LTC_MASTER) {
            char defaultName[33];
            snprintf(defaultName, sizeof(defaultName), "TC-WL-LTC-%s", macSuffix);
            String bleName = blePrefs.isKey("ltc_name") ? blePrefs.getString("ltc_name", "") : defaultName;
            strncpy(apSsid, bleName.c_str(), sizeof(apSsid) - 1);
            apSsid[sizeof(apSsid) - 1] = '\0';
        } else {
            char defaultSlaveName[33];
            snprintf(defaultSlaveName, sizeof(defaultSlaveName), "TC-WL-LTC-%s", macSuffix);
            String bleName = blePrefs.isKey("ltc_name") ? blePrefs.getString("ltc_name", "") : defaultSlaveName;
            strncpy(apSsid, bleName.c_str(), sizeof(apSsid) - 1);
            apSsid[sizeof(apSsid) - 1] = '\0';
        }
#endif
        blePrefs.end();
    }
#if OLED_ENABLE
#ifdef TCWL_CLAP
    // CLAP: use a clean name without MAC suffix.  Read from NVS "name"
    // (user-customizable via config drawer) but override stale defaults
    // left behind by another variant.
    {
        const char *n = bleTimecodeGetName();
        if (n[0] == '\0' || strcmp(n, "TC-WL-HDMI") == 0 ||
            strcmp(n, "TC-WL-LTC") == 0) {
            strcpy(gDeviceName, "TC-WL-CLAP");
        } else {
            strncpy(gDeviceName, n, sizeof(gDeviceName) - 1);
            gDeviceName[sizeof(gDeviceName) - 1] = '\0';
        }
    }
#else
    strncpy(gDeviceName, apSsid, sizeof(gDeviceName) - 1);
    gDeviceName[sizeof(gDeviceName) - 1] = '\0';
#endif
#endif
    // Register persistent callbacks BEFORE begin() so NVS state applies at once
#if OLED_ENABLE
    webui.onSetOledEnabled([](bool en) {
        oled.setEnabled(en);
    });
#endif
    webui.onSetLtcEnabled([](bool en) {
        ltc.setEnabled(en);
    });
    webui.onSetWifiEnabled([](bool en) {
        (void)en;
    });

    Serial.print(F("Starting WiFi AP... "));
    Serial.println(apSsid);
    webui.begin(apSsid, WEBUI_AP_PASSWORD,
                WEBUI_STA_SSID, WEBUI_STA_PASSWORD);

    // CLAP has OLED hardwired with no user toggle (no physical buttons), so
    // force it on regardless of any stale NVS preference from another variant.
#ifdef TCWL_CLAP
    webui.setOledEnabled(true);
#endif

    webui.onSetFps([](uint8_t fps, bool df) {
        ltc.setFps(fps, df);
        framePollMs = 1000 / fps;
        {
            Preferences prefs;
            prefs.begin("ltc", false);
            prefs.putUChar("fps", fps);
            prefs.putBool("df", df);
            prefs.end();
        }
#if FPS_AUTO_DETECT
        if (webui.autoFps()) resetFpsDetect();
#endif
    });
    webui.onJamTime([](uint8_t dd, uint8_t hh, uint8_t mm, uint8_t ss, uint8_t ff) {
        ltc.setTime(hh, mm, ss, ff);
        ltc.setDd(dd);
#if RTC_ENABLE
        if (rtcPresent) {
            rtc.setTime(hh, mm, ss);
            rtcHH = hh; rtcMM = mm; rtcSS = ss;
            lastRtcSyncMs = millis();
        }
#endif
        Serial.printf("jam %02u:%02u:%02u:%02u:%02u\n", dd, hh, mm, ss, ff);
    });
#if MAX7219_ENABLE
    webui.onSetBrightness([](uint8_t val) {
        mx7219.setIntensity(val);
    });
    webui.onSetMatrixEnabled([](bool en) {
        if (en) {
            mx7219.showTimecode(ltc.dd(), ltc.hh(), ltc.mm(), ltc.ss(), ltc.ff());
        } else {
            mx7219.clear();
        }
    });
#endif

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
                Serial.print(F("WiFi: STA connected, MAC="));
                Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X\n",
                    info.wifi_ap_staconnected.mac[0],
                    info.wifi_ap_staconnected.mac[1],
                    info.wifi_ap_staconnected.mac[2],
                    info.wifi_ap_staconnected.mac[3],
                    info.wifi_ap_staconnected.mac[4],
                    info.wifi_ap_staconnected.mac[5]);
                break;
            case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
                Serial.printf("WiFi: STA disconnected, MAC=%02X:%02X:%02X:%02X:%02X:%02X aid=%d\n",
                    info.wifi_ap_stadisconnected.mac[0],
                    info.wifi_ap_stadisconnected.mac[1],
                    info.wifi_ap_stadisconnected.mac[2],
                    info.wifi_ap_stadisconnected.mac[3],
                    info.wifi_ap_stadisconnected.mac[4],
                    info.wifi_ap_stadisconnected.mac[5],
                    info.wifi_ap_stadisconnected.aid);
                break;
            case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
                Serial.println(F("WiFi: STA got IP"));
                break;
        }
    });
#endif

    printConfig();

    Serial.println();

    // Init BLE (starts server for HDMI or LTC-master, scanner for LTC-slave)
    {
        Serial.print(F("BLE init... "));
        bleTimecodeInit();
#if TCWL_LTC
        if (bleGetMode() != TCWL_MODE_LTC_MASTER) {
            bleTimecodeSetCallback(onBleTimecode);
        }
#endif
        Serial.println(F("done"));

        // BLE config command callback — allows Android app to configure
        // device over BLE instead of WiFi HTTP.
        bleTimecodeSetConfigCallback([](const char *cmd, const char *val) -> bool {
            if (strcmp(cmd, "fps") == 0) {
                int fps = atoi(val);
                if (fps >= 24 && fps <= 60) {
                    ltc.setFps(fps, ltc.dropFrame());
                    framePollMs = 1000 / fps;
                    Preferences prefs;
                    prefs.begin("ltc", false);
                    prefs.putUChar("fps", fps);
                    prefs.putBool("df", ltc.dropFrame());
                    prefs.end();
                }
                return true;
            }
            if (strcmp(cmd, "df") == 0) {
                ltc.setFps(ltc.fps(), atoi(val) != 0);
                framePollMs = 1000 / ltc.fps();
                Preferences prefs;
                prefs.begin("ltc", false);
                prefs.putUChar("fps", ltc.fps());
                prefs.putBool("df", atoi(val) != 0);
                prefs.end();
                return true;
            }
            if (strcmp(cmd, "jam") == 0) {
                unsigned dd, hh, mm, ss, ff;
                if (sscanf(val, "%u %u %u %u %u", &dd, &hh, &mm, &ss, &ff) >= 4) {
                    ltc.setTime(hh, mm, ss, ff);
                    ltc.setDd(dd);
#if RTC_ENABLE
                    if (rtcPresent) {
                        rtc.setTime(hh, mm, ss);
                        rtcHH = hh; rtcMM = mm; rtcSS = ss;
                        lastRtcSyncMs = millis();
                    }
#endif
                }
                return true;
            }
            if (strcmp(cmd, "brightness") == 0) {
#if WEBUI_ENABLE
                webui.setBrightness(atoi(val));
#endif
                return true;
            }
            if (strcmp(cmd, "matrix") == 0) {
#if WEBUI_ENABLE
                webui.setMatrixEnabled(atoi(val) != 0);
#endif
                return true;
            }
            if (strcmp(cmd, "oled") == 0) {
#if WEBUI_ENABLE
                webui.setOledEnabled(atoi(val) != 0);
#elif OLED_ENABLE
                oled.setEnabled(atoi(val) != 0);
#endif
                return true;
            }
            if (strcmp(cmd, "ltc") == 0) {
#if WEBUI_ENABLE
                webui.setLtcEnabled(atoi(val) != 0);
#else
                ltc.setEnabled(atoi(val) != 0);
#endif
                return true;
            }
            if (strcmp(cmd, "name") == 0) {
                bleTimecodeSetName(val);
                return true;
            }
            if (strcmp(cmd, "restart") == 0) {
                delay(100);
                ESP.restart();
                return true;
            }
#if TCWL_LTC
            if (strcmp(cmd, "mode") == 0) {
                if (strcmp(val, "master") == 0) {
                    bleSetMode(TCWL_MODE_LTC_MASTER);
                } else if (strcmp(val, "slave") == 0) {
                    bleSetMode(TCWL_MODE_LTC);
                }
                delay(100);
                ESP.restart();
                return true;
            }
#endif
            if (strcmp(cmd, "wifi") == 0) {
                webui.setWifiEnabled(atoi(val) != 0);
                return true;
            }
            if (strcmp(cmd, "wifi_ssid") == 0) {
                const char *delim = strchr(val, ',');
                if (delim) {
                    std::string ssid(val, delim - val);
                    const char *pass = delim + 1;
                    webui.connectWifi(ssid.c_str(), pass);
                }
                return true;
            }
            if (strcmp(cmd, "wifi_forget") == 0) {
                webui.forgetWifi();
                return true;
            }
#if TCWL_LTC
            if (strcmp(cmd, "scan") == 0) {
                bleTimecodeStartScan();
                return true;
            }
            if (strcmp(cmd, "select") == 0) {
                bleTimecodeSelect(val);
                return true;
            }
            if (strcmp(cmd, "disconnect_master") == 0) {
                bleTimecodeDisconnect();
                return true;
            }
#endif
            return false; // unknown command
        });

        // State read callback — return current device state for the Android app
        bleTimecodeSetStateCallback([]() -> const char* {
            static char state[768];
            int n = snprintf(state, sizeof(state),
                "wifi=%d|ssid=%s|ip=%s|rssi=%d|fw=%s",
                webui.wifiEnabled() ? 1 : 0,
                webui.staSsid(),
                webui.staIp().toString().c_str(),
                webui.wifiEnabled() ? static_cast<int>(WiFi.RSSI()) : 0,
                FW_VERSION);
            n += snprintf(state + n, sizeof(state) - n,
                "|conn=%d", bleTimecodeConnected() ? 1 : 0);
            if (bleTimecodeConnected()) {
                const char *name = bleTimecodeConnectedName();
                if (name && name[0]) {
                    n += snprintf(state + n, sizeof(state) - n,
                        "|conn_name=%s", name);
                } else {
                    const char *addr = bleTimecodeConnectedAddress();
                    if (addr) {
                        n += snprintf(state + n, sizeof(state) - n,
                            "|conn_name=%s", addr);
                    }
                }
            } else if (bleTimecodePendingConnect()) {
                const char *name = bleTimecodePendingName();
                if (name && name[0]) {
                    n += snprintf(state + n, sizeof(state) - n,
                        "|conn=1|conn_name=%s", name);
                } else {
                    const char *addr = bleTimecodePendingAddress();
                    if (addr && addr[0]) {
                        n += snprintf(state + n, sizeof(state) - n,
                            "|conn=1|conn_name=%s", addr);
                    }
                }
            }
            bool scanning = bleTimecodeScanning();
            n += snprintf(state + n, sizeof(state) - n,
                "|scanning=%d", scanning ? 1 : 0);
            if (bleTimecodeScanDone()) {
                BleScanResult results[10];
                uint8_t count = bleTimecodeScanResults(results, 10);
                n += snprintf(state + n, sizeof(state) - n,
                    "|scan_count=%d", count);
                for (uint8_t i = 0; i < count && n < (int)sizeof(state) - 64; i++) {
                    n += snprintf(state + n, sizeof(state) - n,
                        "|scan_name_%d=%s|scan_addr_%d=%s",
                        i, results[i].name, i, results[i].address);
                }
            }
            return state;
        });
#ifdef TCWL_CLAP
        // Override stale BLE name left by another variant.
        {
            const char *n = bleTimecodeGetName();
            if (strcmp(n, "TC-WL-HDMI") == 0 ||
                strcmp(n, "TC-WL-LTC") == 0 ||
                n[0] == '\0') {
                bleTimecodeSetName("TC-WL-CLAP");
            }
            // Also refresh gDeviceName now that we have the real name
            // (it was set before BLE init from the old NVS value).
#if OLED_ENABLE
            strncpy(gDeviceName, bleTimecodeGetName(), sizeof(gDeviceName) - 1);
            gDeviceName[sizeof(gDeviceName) - 1] = '\0';
            if (gDeviceName[0] == '\0' ||
                strcmp(gDeviceName, "TC-WL-HDMI") == 0 ||
                strcmp(gDeviceName, "TC-WL-LTC") == 0) {
                strcpy(gDeviceName, "TC-WL-CLAP");
            }
#endif
        }
#endif
    }

#if TCWL_HDMI
    hdmiSetup();
#else
    ltcSetup();
#endif

    Serial.println(F("System ready."));
}

// ===========================================================================
// loop()
// ===========================================================================
void loop() {
#if WEBUI_ENABLE
    webui.handleClient();
#endif

#if TCWL_HDMI
    hdmiLoop();
#else
    ltcLoop();
#endif
}
