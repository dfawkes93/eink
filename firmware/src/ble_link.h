#pragma once
#include <Arduino.h>

// Nordic UART Service over BLE: the host writes newline-delimited JSON to RX,
// we notify on TX. Links are encrypted (Just Works pairing + bonding).
void bleBegin(const char *name);
bool bleConnected();
// Pops one complete line received over BLE, without the newline.
bool bleReadLine(String &out);
void bleSend(const String &line);
// Drop connections cleanly before deep sleep, so the host notices at once.
void bleDisconnectAll();
