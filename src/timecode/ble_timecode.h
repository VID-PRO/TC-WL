#pragma once
#include <Arduino.h>

#if defined(SOC_BLE_SUPPORTED) || defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE)
#include <BLEUUID.h>
extern const BLEUUID bleTimecodeServiceUUID;
extern const BLEUUID bleTimecodeCharUUID;
extern const BLEUUID bleTimecodeNameCharUUID;
extern const BLEUUID bleTimecodeConfigCharUUID;
#endif

// BLE config command callback
// Returns true if command was handled.
typedef bool (*BleConfigCb)(const char *cmd, const char *val);
void bleTimecodeSetConfigCallback(BleConfigCb cb);

// BLE state read callback — returns a string describing current device state
// (wifi status, SSID, IP, RSSI, etc.) for the app to read.
typedef const char* (*BleStateCb)();
void bleTimecodeSetStateCallback(BleStateCb cb);

// Runtime mode management (always available)
int bleGetMode();
void bleSetMode(int mode);
#define TCWL_MODE_HDMI 1
#define TCWL_MODE_LTC  2
#define TCWL_MODE_LTC_MASTER 3

void bleTimecodeInit();

// HDMI-mode functions (no-ops in LTC/CLAP mode)
void bleTimecodeUpdate(uint8_t dd, uint8_t hh, uint8_t mm, uint8_t ss, uint8_t ff, uint8_t lockState = 0, uint8_t fps = 0, uint8_t flags = 0, uint8_t batteryPct = 0);
void bleTimecodeSetName(const char *name);
const char *bleTimecodeGetName();
void bleTimecodeDisconnectAll();
void bleTimecodeDisconnectPeer(const char *address);
uint8_t bleTimecodeConnectedCount();

typedef struct {
    char address[18];
    char name[33];
    uint16_t connId;
} BlePeerInfo;

uint8_t bleTimecodeGetPeers(BlePeerInfo *peers, uint8_t maxPeers);

// LTC/CLAP-mode functions (no-ops in HDMI mode)
typedef void (*BleTimecodeCb)(uint8_t dd, uint8_t hh, uint8_t mm, uint8_t ss, uint8_t ff, uint8_t fps);
void bleTimecodeSetCallback(BleTimecodeCb cb);
void bleTimecodePoll();
bool bleTimecodeConnected();

typedef struct {
    char name[33];
    char address[18];
} BleScanResult;

uint8_t bleTimecodeScan(BleScanResult *results, uint8_t maxResults);
void bleTimecodeStartScan();
bool bleTimecodeScanning();
bool bleTimecodeScanDone();
uint8_t bleTimecodeScanResults(BleScanResult *results, uint8_t maxResults);
void bleTimecodeSelect(const char *address, const char *name = nullptr);
void bleTimecodeDisconnect();
void bleTimecodeSetMenuActive(bool active);
bool bleTimecodeIsMenuActive();
const char *bleTimecodeSelectedAddress();
const char *bleTimecodeConnectedAddress();
const char *bleTimecodeConnectedName();
