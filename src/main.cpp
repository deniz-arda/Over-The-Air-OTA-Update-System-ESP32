#include <Arduino.h>
#include <SPIFFS.h>
#include <esp_flash.h>
#include <esp_partition.h>

#define DEBUG_ENABLED false

#if DEBUG_ENABLED
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(format, ...) Serial.printf(format, __VA_ARGS__)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(format, ...)
#endif

// Command definitions (must match host)
enum Command {
    INIT_UPDATE = 0x01,
    SEND_PACKET = 0x02,
    FINISH_UPDATE = 0x03,
    GET_VERSION = 0x04,
    ABORT_UPDATE = 0x05
};

// Response definitions (must match host)
enum Response {
    ACK = 0x10,
    NACK = 0x11,
    READY = 0x12,
    CHECKSUM_ERROR = 0x13,
    VERSION_INFO = 0x14,
    ERROR = 0x15
};

// Version information structure
struct VersionInfo {
    uint32_t current_version;
    uint32_t backup_version;
    uint32_t current_address;
    uint32_t backup_address;
};

// Update state structure
struct UpdateState {
    bool in_progress;
    uint32_t version;
    uint32_t target_address;
    uint32_t file_size;
    uint32_t bytes_received;
    uint16_t expected_packet;
    unsigned long last_activity;
};

// Configuration
const size_t PACKET_SIZE = 16;
const unsigned long TIMEOUT_MS = 30000;
const size_t MAX_FILE_SIZE = 1024 * 1024;

// Memory addresses for version storage
const uint32_t VERSION_1_ADDRESS = 0x200000;
const uint32_t VERSION_2_ADDRESS = 0x300000;

// Global state
VersionInfo g_version_info = {0, 0, 0, 0};
UpdateState g_update_state = {false, 0, 0, 0, 0, 0, 0};

// Function prototypes
void setup();
void loop();
bool readCommand(Command& cmd, uint8_t* data, size_t& length);
void sendResponse(Response resp, const uint8_t* data = nullptr, size_t length = 0);
uint8_t calculateXORChecksum(const uint8_t* data, size_t length);
bool validateChecksum(const uint8_t* packet, size_t length);
void handleInitUpdate(const uint8_t* data, size_t length);
void handleSendPacket(const uint8_t* data, size_t length);
void handleFinishUpdate();
void handleGetVersion();
void handleAbortUpdate();
bool writeToFlash(uint32_t address, const uint8_t* data, size_t length);
bool eraseFlashSector(uint32_t address);
void loadVersionInfo();
void saveVersionInfo();
void updateVersionInfo(uint32_t new_version, uint32_t new_address);

void setup() {
    Serial.begin(115200);
    
    // Only show essential startup messages
    DEBUG_PRINTLN("ESP32 OTA Update Client Starting...");
    
    // Initialize SPIFFS for version info storage
    if (!SPIFFS.begin(true)) {
        DEBUG_PRINTLN("SPIFFS initialization failed!");
        return;
    }
    
    // Load existing version information
    loadVersionInfo();
    
    DEBUG_PRINTLN("OTA Client Ready");
    DEBUG_PRINTF("Current Version: %u at 0x%08X\n", 
                  g_version_info.current_version, g_version_info.current_address);
    DEBUG_PRINTF("Backup Version: %u at 0x%08X\n", 
                  g_version_info.backup_version, g_version_info.backup_address);
}

void loop() {
    // Check for timeout during update
    if (g_update_state.in_progress) {
        if (millis() - g_update_state.last_activity > TIMEOUT_MS) {
            DEBUG_PRINTLN("Update timeout - aborting");
            handleAbortUpdate();
        }
    }
    
    // Process incoming commands
    Command cmd;
    uint8_t data[256];
    size_t length;
    
    if (readCommand(cmd, data, length)) {
        g_update_state.last_activity = millis();
        
        switch (cmd) {
            case INIT_UPDATE:
                handleInitUpdate(data, length);
                break;
                
            case SEND_PACKET:
                handleSendPacket(data, length);
                break;
                
            case FINISH_UPDATE:
                handleFinishUpdate();
                break;
                
            case GET_VERSION:
                handleGetVersion();
                break;
                
            case ABORT_UPDATE:
                handleAbortUpdate();
                break;
                
            default:
                DEBUG_PRINTF("Unknown command: 0x%02X\n", cmd);
                sendResponse(ERROR);
                break;
        }
    }
    
    delay(10);
}

bool readCommand(Command& cmd, uint8_t* data, size_t& length) {
    // Wait for header (CMD + LEN) with timeout
    const unsigned long HEADER_TIMEOUT_MS = 2000;
    unsigned long start = millis();
    while (Serial.available() < 2) {
        if (millis() - start > HEADER_TIMEOUT_MS) {
            return false;
        }
        delay(1);
    }

    // Read command header bytes
    int cmd_int = Serial.read();
    int len_int = Serial.read();
    if (cmd_int < 0 || len_int < 0) {
        return false;
    }
    uint8_t cmd_byte = (uint8_t)cmd_int;
    uint8_t length_byte = (uint8_t)len_int;

    // Guard against ridiculously large length values
    if (length_byte > 250) {
        DEBUG_PRINTF("Suspicious length byte: %u\n", length_byte);
        return false;
    }

    // Wait for the rest: data (length_byte) + checksum (1 byte)
    const unsigned long PAYLOAD_TIMEOUT_MS = 5000;
    start = millis();
    while (Serial.available() < (int)(length_byte + 1)) {
        if (millis() - start > PAYLOAD_TIMEOUT_MS) {
            DEBUG_PRINTLN("Timeout waiting for packet payload");
            return false;
        }
        delay(1);
    }

    // Read data bytes
    for (size_t i = 0; i < length_byte; i++) {
        int b = Serial.read();
        if (b < 0) {
            return false;
        }
        data[i] = (uint8_t)b;
    }

    // Read checksum
    int rc = Serial.read();
    if (rc < 0) return false;
    uint8_t received_checksum = (uint8_t)rc;

    // Verify checksum: XOR of cmd, length, and data bytes
    uint8_t expected_checksum = cmd_byte ^ length_byte;
    for (size_t i = 0; i < length_byte; i++) {
        expected_checksum ^= data[i];
    }

    if (expected_checksum != received_checksum) {
        DEBUG_PRINTF("Command checksum error (got 0x%02X, expected 0x%02X)\n", received_checksum, expected_checksum);
        return false;
    }

    cmd = (Command)cmd_byte;
    length = length_byte;
    return true;
}

void sendResponse(Response resp, const uint8_t* data, size_t length) {
    // Calculate checksum
    uint8_t checksum = (uint8_t)resp ^ (uint8_t)length;
    if (data && length > 0) {
        for (size_t i = 0; i < length; i++) {
            checksum ^= data[i];
        }
    }
    
    // Send response
    Serial.write((uint8_t)resp);
    Serial.write((uint8_t)length);
    if (data && length > 0) {
        Serial.write(data, length);
    }
    Serial.write(checksum);
    Serial.flush();
}

uint8_t calculateXORChecksum(const uint8_t* data, size_t length) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < length; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

void handleInitUpdate(const uint8_t* data, size_t length) {
    if (length != 12) {
        uint8_t err = 1; // bad length
        sendResponse(ERROR, &err, 1);
        return;
    }
    
    if (g_update_state.in_progress) {
        uint8_t err = 2; // update already in progress
        sendResponse(ERROR, &err, 1);
        return;
    }
    
    // Parse init data
    uint32_t version, address, size;
    memcpy(&version, data, 4);
    memcpy(&address, data + 4, 4);
    memcpy(&size, data + 8, 4);
    
    DEBUG_PRINTF("Init update: version=%u, address=0x%08X, size=%u\n", 
                  version, address, size);
    
    // Validate parameters
    if (size == 0 || size > MAX_FILE_SIZE) {
        uint8_t err = 3; // bad size
        sendResponse(ERROR, &err, 1);
        return;
    }
    
    if (address != VERSION_1_ADDRESS && address != VERSION_2_ADDRESS) {
        uint8_t err = 4; // bad address
        sendResponse(ERROR, &err, 1);
        return;
    }
    
    // Erase target flash sectors
    DEBUG_PRINTF("Erasing flash at 0x%08X...\n", address);
    size_t sectors_to_erase = (size + 4095) / 4096;
    
    for (size_t i = 0; i < sectors_to_erase; i++) {
        if (!eraseFlashSector(address + (i * 4096))) {
            uint8_t err = 5; // flash erase failed
            sendResponse(ERROR, &err, 1);
            return;
        }
    }
    
    // Initialize update state
    g_update_state.in_progress = true;
    g_update_state.version = version;
    g_update_state.target_address = address;
    g_update_state.file_size = size;
    g_update_state.bytes_received = 0;
    g_update_state.expected_packet = 0;
    g_update_state.last_activity = millis();
    
    DEBUG_PRINTLN("Ready for data packets");
    sendResponse(READY);
}

void handleSendPacket(const uint8_t* data, size_t length) {
    if (!g_update_state.in_progress) {
        DEBUG_PRINTLN("No update in progress");
        sendResponse(NACK);
        return;
    }
    
    if (length != (2 + 1 + PACKET_SIZE)) {
        DEBUG_PRINTF("Invalid packet length: %zu\n", length);
        sendResponse(NACK);
        return;
    }
    
    // Parse packet header
    uint16_t packet_num;
    memcpy(&packet_num, data, sizeof(packet_num));
    uint8_t expected_checksum = data[2];
    const uint8_t* packet_data = data + 3;
    
    // Verify packet sequence
    if (packet_num != g_update_state.expected_packet) {
        DEBUG_PRINTF("Packet sequence error: expected %u, got %u\n", 
                      g_update_state.expected_packet, packet_num);
        sendResponse(NACK);
        return;
    }
    
    // Verify checksum
    uint8_t actual_checksum = calculateXORChecksum(packet_data, PACKET_SIZE);
    if (actual_checksum != expected_checksum) {
        DEBUG_PRINTF("Packet checksum error: expected 0x%02X, got 0x%02X\n", 
                      expected_checksum, actual_checksum);
        sendResponse(CHECKSUM_ERROR);
        return;
    }
    
    // Calculate write parameters
    uint32_t write_address = g_update_state.target_address + g_update_state.bytes_received;
    size_t bytes_to_write = PACKET_SIZE;
    
    // Don't write beyond file size
    if (g_update_state.bytes_received + PACKET_SIZE > g_update_state.file_size) {
        bytes_to_write = g_update_state.file_size - g_update_state.bytes_received;
    }
    
    // Write to flash
    if (!writeToFlash(write_address, packet_data, bytes_to_write)) {
        DEBUG_PRINTF("Failed to write packet %u to flash\n", packet_num);
        sendResponse(ERROR);
        return;
    }
    
    // Update state
    g_update_state.bytes_received += bytes_to_write;
    g_update_state.expected_packet++;
    
    // Send acknowledgment
    sendResponse(ACK);
    
    // Progress indicator (only occasionally)
    if (packet_num % 64 == 0 || g_update_state.bytes_received >= g_update_state.file_size) {
        float progress = (float)g_update_state.bytes_received / g_update_state.file_size * 100.0f;
        DEBUG_PRINTF("Progress: %.1f%% (%u/%u bytes)\n", 
                      progress, g_update_state.bytes_received, g_update_state.file_size);
    }
}

void handleFinishUpdate() {
    if (!g_update_state.in_progress) {
        DEBUG_PRINTLN("No update in progress");
        sendResponse(NACK);
        return;
    }
    
    if (g_update_state.bytes_received != g_update_state.file_size) {
        DEBUG_PRINTF("Incomplete update: received %u, expected %u bytes\n", 
                      g_update_state.bytes_received, g_update_state.file_size);
        sendResponse(NACK);
        return;
    }
    
    DEBUG_PRINTLN("Update completed successfully");
    
    // Update version information
    updateVersionInfo(g_update_state.version, g_update_state.target_address);
    
    // Save version info to persistent storage
    saveVersionInfo();
    
    // Clear update state
    g_update_state.in_progress = false;
    
    sendResponse(ACK);
    
    DEBUG_PRINTF("New current version: %u at 0x%08X\n", 
                  g_version_info.current_version, g_version_info.current_address);
    DEBUG_PRINTF("Backup version: %u at 0x%08X\n", 
                  g_version_info.backup_version, g_version_info.backup_address);
}

void handleGetVersion() {
    uint8_t response_data[16];
    
    // Pack version info: current_ver, backup_ver, current_addr, backup_addr
    memcpy(response_data, &g_version_info.current_version, 4);
    memcpy(response_data + 4, &g_version_info.backup_version, 4);
    memcpy(response_data + 8, &g_version_info.current_address, 4);
    memcpy(response_data + 12, &g_version_info.backup_address, 4);
    
    sendResponse(VERSION_INFO, response_data, 16);
}

void handleAbortUpdate() {
    if (g_update_state.in_progress) {
        DEBUG_PRINTLN("Aborting update");
        g_update_state.in_progress = false;
        sendResponse(ACK);
    } else {
        DEBUG_PRINTLN("No update to abort");
        sendResponse(NACK);
    }
}

bool writeToFlash(uint32_t address, const uint8_t* data, size_t length) {
    if (length == 0) return true;
    
    esp_err_t result = esp_flash_write(esp_flash_default_chip, data, address, length);
    if (result != ESP_OK) {
        DEBUG_PRINTF("Flash write failed: %s\n", esp_err_to_name(result));
        return false;
    }
    
    return true;
}

bool eraseFlashSector(uint32_t address) {
    esp_err_t result = esp_flash_erase_region(esp_flash_default_chip, address, 4096);
    if (result != ESP_OK) {
        DEBUG_PRINTF("Flash erase failed: %s\n", esp_err_to_name(result));
        return false;
    }
    
    return true;
}

void loadVersionInfo() {
    File file = SPIFFS.open("/version_info.bin", "r");
    if (file && file.size() == sizeof(VersionInfo)) {
        file.read((uint8_t*)&g_version_info, sizeof(VersionInfo));
        file.close();
        DEBUG_PRINTLN("Version info loaded from SPIFFS");
    } else {
        // Initialize default version info
        g_version_info = {0, 0, 0, 0};
        DEBUG_PRINTLN("Using default version info");
        if (file) file.close();
    }
}

void saveVersionInfo() {
    File file = SPIFFS.open("/version_info.bin", "w");
    if (file) {
        file.write((uint8_t*)&g_version_info, sizeof(VersionInfo));
        file.close();
        DEBUG_PRINTLN("Version info saved to SPIFFS");
    } else {
        DEBUG_PRINTLN("Failed to save version info");
    }
}

void updateVersionInfo(uint32_t new_version, uint32_t new_address) {
    // Current version becomes backup
    g_version_info.backup_version = g_version_info.current_version;
    g_version_info.backup_address = g_version_info.current_address;
    
    // New version becomes current
    g_version_info.current_version = new_version;
    g_version_info.current_address = new_address;
}