#include <Arduino.h>
#include <NimBLEDevice.h>

// --- Configuration ---
// Service and Characteristic UUIDs for Heart Rate (Standard)
static BLEUUID serviceUUID("180D");
static BLEUUID charUUID("2A37");
// Address of the Pixel Watch (from your log)
static BLEAddress targetDeviceAddress("20:F0:94:4C:01:D5");

// --- Global Variables ---
// Client (Connecting to Watch)
static BLEAddress *pServerAddress = nullptr;
static boolean doConnect = false;
static boolean connectedToWatch = false;
static boolean doScan = false;
static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEClient*  pClient = nullptr;

// Server (Rebroadcasting to Phone/Bike Computer)
static BLEServer* pLocalServer = nullptr;
static BLECharacteristic* pLocalCharacteristic = nullptr;
static boolean deviceConnectedToUs = false;

// Flags for hr measurement
const int HR_VALUE_FORMAT_MASK = 1;

// --- Server Callbacks ---
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnectedToUs = true;
      Serial.println("Device connected to our Rebroadcast Server");
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnectedToUs = false;
      Serial.println("Device disconnected from our Rebroadcast Server");
      // Restart advertising so others can connect
      pServer->getAdvertising()->start(); 
    }
};

// --- Client Logic ---

void notifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify) {
    if (length > 1) {
        uint8_t flags = pData[0];
        int heartRate;
        if (flags & HR_VALUE_FORMAT_MASK) {
             // 16 bit
             heartRate = pData[1] + (pData[2] << 8);
        } else {
             // 8 bit
             heartRate = pData[1];
        }
        Serial.printf("Heart Rate: %d bpm\n", heartRate);

        // --- REBROADCAST LOGIC ---
        if (pLocalServer && pLocalCharacteristic) {
            // We can just forward the raw data packet exactly as received
            // because we are implementing the exact same service/characteristic.
            pLocalCharacteristic->setValue(pData, length);
            pLocalCharacteristic->notify();
            // Serial.println("Rebroadcasted value"); 
        }
    }
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
      Serial.println("Connected to Pixel Watch");
      connectedToWatch = true;
  }

  void onDisconnect(BLEClient* pclient) {
    connectedToWatch = false;
    Serial.println("Disconnected from Pixel Watch");
  }
};

bool connectToWatchFunc() {
    Serial.print("Forming a connection to ");
    Serial.println(pServerAddress->toString().c_str());
    
    if(!pClient) {
        pClient = BLEDevice::createClient();
        pClient->setClientCallbacks(new MyClientCallback());
    }

    // Connect
    if(!pClient->connect(*pServerAddress)) {
        Serial.println("Failed to connect");
        return false;
    }
    Serial.println(" - Connected to server");

    // Get Service
    BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
    if (pRemoteService == nullptr) {
      Serial.print("Failed to find our service UUID: ");
      Serial.println(serviceUUID.toString().c_str());
      pClient->disconnect();
      return false;
    }

    // Get Characteristic
    pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
    if (pRemoteCharacteristic == nullptr) {
      Serial.print("Failed to find our characteristic UUID: ");
      Serial.println(charUUID.toString().c_str());
      pClient->disconnect();
      return false;
    }

    // Register Notification
    if(pRemoteCharacteristic->canNotify())
      pRemoteCharacteristic->registerForNotify(notifyCallback);

    return true;
}

// --- Scan Logic ---
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice* advertisedDevice) {
    Serial.print("BLE Advertised Device found: ");
    Serial.println(advertisedDevice->toString().c_str());

    bool found = false;
    
    // Check by Service UUID
    if (advertisedDevice->haveServiceUUID() && advertisedDevice->isAdvertisingService(serviceUUID)) {
       // found = true; // Use address only to be safe? The watch does advertise the UUID though.
    }
    // Check by Address
    if (advertisedDevice->getAddress().equals(targetDeviceAddress)) {
        found = true;
        Serial.println("Found Pixel Watch by Address!");
    }

    if (found) {
      Serial.println("Stop Scan. Connect to Watch."); 
      advertisedDevice->getScan()->stop();
      if(pServerAddress) delete pServerAddress;
      pServerAddress = new BLEAddress(advertisedDevice->getAddress());
      doConnect = true;
      doScan = true;
    }
  }
};

void setup() {
  Serial.begin(115200);
  Serial.println("Starting Pixel Watch HR Rebroadcast...");
  
  // 1. Initialize BLE Device with a name for our Rebroadcast
  BLEDevice::init("Pixel-HR-Repeater");

  // --- SERVER SETUP (Rebroadcast) ---
  pLocalServer = BLEDevice::createServer();
  pLocalServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pLocalServer->createService(serviceUUID);
  pLocalCharacteristic = pService->createCharacteristic(
                      charUUID,
                      NIMBLE_PROPERTY::NOTIFY
                    );

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(serviceUUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setName("Pixel-HR-Repeater"); // Ensure name is in advertisement
  pAdvertising->start();
  Serial.println("Rebroadcast Server Started. Advertising as 'Pixel-HR-Repeater'...");


  // --- CLIENT SETUP (Watch Connection) ---
  // Security for Bonding
  BLESecurity *pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND); 
  pSecurity->setCapability(ESP_IO_CAP_IO); 
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK); 
  
  // Start Scanning
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  // Scan for 5 seconds
  pBLEScan->start(5, false);
}

void loop() {
  // If the flag "doConnect" is true then we have scanned for and found the desired
  // BLE Server with which we wish to connect.
  if (doConnect == true) {
    if (connectToWatchFunc()) {
      Serial.println("We are now connected to the BLE Server.");
    } else {
      Serial.println("We have failed to connect to the server; there is nothin more we will do.");
    }
    doConnect = false;
  }

  // If connected to watch, just keep running.
  // If NOT connected to watch, keep scanning periodically.
  if (!connectedToWatch) {
      if(!BLEDevice::getScan()->isScanning()){
        Serial.println("Scanning for Watch...");
        BLEDevice::getScan()->start(5, false);
      }
  }
  
  // Simple Heartbeat
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 2000) {
      Serial.print("."); 
      if (deviceConnectedToUs) Serial.print(" [Client Connected]");
      Serial.println();
      lastPrint = millis();
  }
  
  delay(10); // Small delay for stability
}
