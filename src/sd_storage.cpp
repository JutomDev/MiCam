#include "sd_storage.h"
#include <algorithm>
#include "esp_log.h"

static const char* TAG = "SD_STORAGE";

bool SdStorage::_mounted = false;
uint16_t SdStorage::_nextPhotoIndex = 1;
std::vector<String> SdStorage::_photoFiles;

bool SdStorage::init() {
    _mounted = false;
    _photoFiles.clear();

    ESP_LOGI(TAG, "Initializing SPI for SD on SCK:%d MISO:%d MOSI:%d CS:%d", SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN,
             SD_CS_PIN);

    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

    if (!SD.begin(SD_CS_PIN, SPI, SD_SPI_FREQ)) {
        ESP_LOGE(TAG, "Card Mount Failed! Check if microSD is inserted.");
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        ESP_LOGW(TAG, "No SD card attached");
        return false;
    }

    ESP_LOGI(TAG, "SD Card mounted successfully. Type: %d, Size: %llu MB", cardType, SD.cardSize() / (1024 * 1024));

    if (!SD.exists(PHOTO_DIR)) {
        if (!SD.mkdir(PHOTO_DIR)) {
            ESP_LOGE(TAG, "Failed to create directory %s", PHOTO_DIR);
            return false;
        }
        ESP_LOGI(TAG, "Created directory %s", PHOTO_DIR);
    }

    _mounted = true;
    scanPhotos();
    return true;
}

bool SdStorage::isAvailable() {
    return _mounted && (SD.cardType() != CARD_NONE);
}

void SdStorage::scanPhotos() {
    _photoFiles.clear();
    uint16_t maxIndex = 0;

    if (!_mounted) return;

    File root = SD.open(PHOTO_DIR);
    if (!root || !root.isDirectory()) {
        ESP_LOGE(TAG, "Cannot open %s directory", PHOTO_DIR);
        return;
    }

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String name = String(file.name());
            // Ensure full path prefix
            if (!name.startsWith("/")) {
                name = String(PHOTO_DIR) + "/" + name;
            }
            if (name.endsWith(".JPG") || name.endsWith(".jpg")) {
                _photoFiles.push_back(name);

                // Parse index e.g. IMG_0042.JPG
                int idxPos = name.indexOf("IMG_");
                if (idxPos != -1) {
                    String numStr = name.substring(idxPos + 4, idxPos + 8);
                    uint16_t num = numStr.toInt();
                    if (num > maxIndex) {
                        maxIndex = num;
                    }
                }
            }
        }
        file = root.openNextFile();
    }
    root.close();

    _nextPhotoIndex = maxIndex + 1;
    std::sort(_photoFiles.begin(), _photoFiles.end());
    ESP_LOGI(TAG, "Scan complete: %d photos found. Next index: %d", (int)_photoFiles.size(), _nextPhotoIndex);
}

void SdStorage::refreshList() {
    scanPhotos();
}

uint16_t SdStorage::getPhotoCount() {
    return (uint16_t)_photoFiles.size();
}

const std::vector<String>& SdStorage::getPhotoList() {
    return _photoFiles;
}

bool SdStorage::savePhoto(const uint8_t* buffer, size_t len, char* outFilename, size_t maxLen) {
    if (!_mounted || !buffer || len == 0) {
        ESP_LOGE(TAG, "Cannot save: SD not mounted or invalid buffer");
        return false;
    }

    char filename[32];
    snprintf(filename, sizeof(filename), "%s%04d%s", PHOTO_PREFIX, _nextPhotoIndex, PHOTO_EXT);

    ESP_LOGI(TAG, "Writing %d bytes to %s...", len, filename);
    uint32_t tStart = millis();

    File file = SD.open(filename, FILE_WRITE);
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file %s for writing", filename);
        return false;
    }

    // Inject standard EXIF header: Make = MIphoto, Model = camera
    static const uint8_t exifHeader[] = {0xFF, 0xE1,                      // APP1 marker
                                         0x00, 0x56,                      // Block length (86 bytes)
                                         'E', 'x', 'i', 'f', 0x00, 0x00,  // Exif identifier
                                         'I', 'I',                        // Little Endian (Intel)
                                         0x2A, 0x00,                      // TIFF Magic (42)
                                         0x08, 0x00, 0x00, 0x00,          // Offset to IFD0 (8 bytes)
                                         0x03, 0x00,                      // Number of tags (3 tags)

                                         // Tag 1: Make (0x010F) - "MIphoto" (8 bytes with null terminator)
                                         0x0F, 0x01, 0x02, 0x00, 0x08, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00,
                                         // Tag 2: Model (0x0110) - "camera" (7 bytes with null terminator)
                                         0x10, 0x01, 0x02, 0x00, 0x07, 0x00, 0x00, 0x00, 0x3A, 0x00, 0x00, 0x00,
                                         // Tag 3: Software (0x0131) - "MIphoto OS v1.2" (16 bytes with null terminator)
                                         0x31, 0x01, 0x02, 0x00, 0x10, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00,

                                         0x00, 0x00, 0x00, 0x00,  // Next IFD (terminal)

                                         // String values in TIFF structure:
                                         // Offset 0x32: Make
                                         'M', 'I', 'p', 'h', 'o', 't', 'o', 0x00,
                                         // Offset 0x3A: Model
                                         'c', 'a', 'm', 'e', 'r', 'a', 0x00,
                                         // Offset 0x41: Software
                                         'M', 'I', 'p', 'h', 'o', 't', 'o', ' ', 'O', 'S', ' ', 'v', '1', '.', '2',
                                         0x00};

    size_t written = 0;
    // If buffer begins with JPEG SOI (0xFF, 0xD8), inject EXIF block between SOI and image stream
    if (len > 4 && buffer[0] == 0xFF && buffer[1] == 0xD8) {
        written += file.write(buffer, 2);                       // Write SOI (0xFF 0xD8)
        written += file.write(exifHeader, sizeof(exifHeader));  // Inject EXIF APP1
        written += file.write(buffer + 2, len - 2);             // Write JPEG payload
    } else {
        written = file.write(buffer, len);
    }

    file.flush();
    file.close();

    uint32_t tElapsed = millis() - tStart;

    if (written < len) {
        ESP_LOGE(TAG, "Write error: wrote %d bytes", written);
        return false;
    }

    ESP_LOGI(TAG, "Saved %s in %u ms (%.2f KB/s)", filename, tElapsed, (len / 1024.0f) / (tElapsed / 1000.0f));

    _photoFiles.push_back(String(filename));
    _nextPhotoIndex++;

    if (outFilename && maxLen > 0) {
        strncpy(outFilename, filename, maxLen - 1);
        outFilename[maxLen - 1] = '\0';
    }

    return true;
}

bool SdStorage::deletePhoto(const char* filename) {
    if (!_mounted || !filename) return false;

    if (SD.remove(filename)) {
        ESP_LOGI(TAG, "Deleted file: %s", filename);
        scanPhotos();
        return true;
    }

    ESP_LOGE(TAG, "Failed to delete file: %s", filename);
    return false;
}

size_t SdStorage::getPhotoSize(const char* filename) {
    if (!_mounted || !filename) return 0;
    File file = SD.open(filename, FILE_READ);
    if (!file) return 0;
    size_t sz = file.size();
    file.close();
    return sz;
}

uint64_t SdStorage::getTotalBytes() {
    if (!_mounted) return 0;
    return SD.totalBytes();
}

uint64_t SdStorage::getUsedBytes() {
    if (!_mounted) return 0;
    return SD.usedBytes();
}
