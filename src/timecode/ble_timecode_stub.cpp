#include "ble_timecode.h"
#include "config.h"
#include <Preferences.h>

static Preferences blePrefs;
static char bleName[33] = "TC-WL-HDMI";

int bleGetMode() { return TCWL_HDMI; }
void bleSetMode(int) {}

void bleTimecodeInit() {
    blePrefs.begin("ble", false);
    String saved = blePrefs.getString("name", "TC-WL-HDMI");
    blePrefs.end();
    strncpy(bleName, saved.c_str(), sizeof(bleName) - 1);
    bleName[sizeof(bleName) - 1] = '\0';
}
void bleTimecodeUpdate(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) {}
void bleTimecodeSetName(const char *name) {
    strncpy(bleName, name, sizeof(bleName) - 1);
    bleName[sizeof(bleName) - 1] = '\0';
    blePrefs.begin("ble", false);
    blePrefs.putString("name", bleName);
    blePrefs.end();
}
const char *bleTimecodeGetName() { return bleName; }
void bleTimecodeDisconnectAll() {}
void bleTimecodeDisconnectPeer(const char *) {}
uint8_t bleTimecodeConnectedCount() { return 0; }
uint8_t bleTimecodeGetPeers(BlePeerInfo *, uint8_t) { return 0; }

void bleTimecodeSetCallback(BleTimecodeCb) {}
void bleTimecodeSetConfigCallback(BleConfigCb) {}
void bleTimecodePoll() {}
bool bleTimecodeConnected() { return false; }
uint8_t bleTimecodeScan(BleScanResult *, uint8_t) { return 0; }
void bleTimecodeStartScan() {}
bool bleTimecodeScanning() { return false; }
bool bleTimecodeScanDone() { return false; }
uint8_t bleTimecodeScanResults(BleScanResult *, uint8_t) { return 0; }
void bleTimecodeSelect(const char *, const char *) {}
void bleTimecodeDisconnect() {}
void bleTimecodeSetMenuActive(bool) {}
bool bleTimecodeIsMenuActive() { return false; }
const char *bleTimecodeSelectedAddress() { return ""; }
const char *bleTimecodeConnectedAddress() { return ""; }
const char *bleTimecodeConnectedName() { return ""; }
