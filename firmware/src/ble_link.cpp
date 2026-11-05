#include "ble_link.h"

#include <NimBLEDevice.h>

#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// A state message is a couple of KB at most; anything bigger is junk.
constexpr size_t MAX_PENDING = 8192;

static NimBLECharacteristic *tx;
static volatile bool connected;
// Written from the NimBLE host task, read from loop().
static String pending;
static SemaphoreHandle_t lock;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server) override { connected = true; }
  void onDisconnect(NimBLEServer *server) override {
    connected = false;
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    std::string v = c->getValue();
    xSemaphoreTake(lock, portMAX_DELAY);
    if (pending.length() + v.size() > MAX_PENDING) pending = "";
    pending.concat(v.data(), v.size());
    xSemaphoreGive(lock);
  }
};

void bleBegin(const char *name) {
  lock = xSemaphoreCreateMutex();
  NimBLEDevice::init(name);
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  NimBLEDevice::setSecurityAuth(/*bonding*/ true, /*mitm*/ false, /*sc*/ true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService *svc = server->createService(NUS_SERVICE);
  NimBLECharacteristic *rx = svc->createCharacteristic(
      NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
  rx->setCallbacks(new RxCallbacks());
  tx = svc->createCharacteristic(
      NUS_TX, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setScanResponse(true);
  adv->start();
}

bool bleConnected() { return connected; }

bool bleReadLine(String &out) {
  bool got = false;
  xSemaphoreTake(lock, portMAX_DELAY);
  int nl = pending.indexOf('\n');
  if (nl >= 0) {
    out = pending.substring(0, nl);
    pending.remove(0, nl + 1);
    got = true;
  }
  xSemaphoreGive(lock);
  return got;
}

void bleSend(const String &line) {
  if (!connected) return;
  tx->setValue((const uint8_t *)line.c_str(), line.length());
  tx->notify();
}

void bleDisconnectAll() {
  NimBLEServer *server = NimBLEDevice::getServer();
  for (uint16_t handle : server->getPeerDevices()) server->disconnect(handle);
}
