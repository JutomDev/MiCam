#include "sd_storage.h"
#include <algorithm>
#include <utility>
#include "esp_log.h"

static const char* TAG = "SD_STORAGE";

bool SdStorage::_mounted = false;
uint16_t SdStorage::_nextPhotoIndex = 1;
uint64_t SdStorage::_totalBytes = 0;
uint64_t SdStorage::_usedBytes = 0;
std::vector<String> SdStorage::_photoFiles;
std::vector<size_t> SdStorage::_photoSizes;

bool SdStorage::init() {
    _mounted = false;
    _totalBytes = 0;
    _usedBytes = 0;
    _photoFiles.clear();
    _photoSizes.clear();

    ESP_LOGI(TAG, "Initializing SPI for SD on SCK:%d MISO:%d MOSI:%d CS:%d", SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN,
             SD_CS_PIN);

    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

    // Try mounting at configured SPI frequency first, fallback to 10 MHz if card needs slower init
    if (!SD.begin(SD_CS_PIN, SPI, SD_SPI_FREQ)) {
        ESP_LOGW(TAG, "SD mount at %d Hz failed, retrying at 10 MHz...", SD_SPI_FREQ);
        delay(50);
        if (!SD.begin(SD_CS_PIN, SPI, 10000000)) {
            ESP_LOGE(TAG, "Card Mount Failed! Check if microSD is inserted.");
            return false;
        }
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        ESP_LOGW(TAG, "No SD card attached");
        return false;
    }

    // Cache total card capacity once (fast BPB read; avoids f_getfree() FAT traversal later)
    _totalBytes = SD.totalBytes();
    ESP_LOGI(TAG, "SD Card mounted successfully. Type: %d, Size: %llu MB", cardType, _totalBytes / (1024 * 1024));

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
    // Do NOT poll SD.cardType() on every frame — return cached mount status
    // If not mounted at boot (e.g. card inserted later), attempt background mount every 4 seconds
    if (!_mounted) {
        static uint32_t lastRetry = 0;
        uint32_t now = millis();
        if (now - lastRetry > 4000) {
            lastRetry = now;
            init();
        }
    }
    return _mounted;
}

void SdStorage::scanPhotos() {
    _photoFiles.clear();
    _photoSizes.clear();
    _usedBytes = 0;
    uint16_t maxIndex = 0;

    if (!_mounted) return;

    File root = SD.open(PHOTO_DIR);
    if (!root || !root.isDirectory()) {
        ESP_LOGE(TAG, "Cannot open %s directory", PHOTO_DIR);
        return;
    }

    std::vector<std::pair<String, size_t>> entries;
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String name = String(file.name());
            // Ensure full path prefix /DCIM/...
            if (!name.startsWith("/DCIM/")) {
                if (name.startsWith("/")) {
                    name = String(PHOTO_DIR) + name;
                } else {
                    name = String(PHOTO_DIR) + "/" + name;
                }
            }
            size_t fsize = file.size();
            if ((name.endsWith(".JPG") || name.endsWith(".jpg")) && fsize > 100) {
                entries.push_back(std::make_pair(name, fsize));
                _usedBytes += fsize;

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

    std::sort(entries.begin(), entries.end(),
              [](const std::pair<String, size_t>& a, const std::pair<String, size_t>& b) { return a.first < b.first; });

    _photoFiles.reserve(entries.size());
    _photoSizes.reserve(entries.size());
    for (const auto& e : entries) {
        _photoFiles.push_back(e.first);
        _photoSizes.push_back(e.second);
    }

    _nextPhotoIndex = maxIndex + 1;
    ESP_LOGI(TAG, "Scan complete: %d photos found (%llu KB). Next index: %d", (int)_photoFiles.size(),
             _usedBytes / 1024, _nextPhotoIndex);
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

    size_t written = file.write(buffer, len);
    file.flush();
    file.close();

    uint32_t tElapsed = millis() - tStart;

    if (written < len) {
        ESP_LOGE(TAG, "Write error: wrote %d of %d bytes", (int)written, (int)len);
        SD.remove(filename);
        return false;
    }

    float elapsedSec = (tElapsed > 0) ? (tElapsed / 1000.0f) : 0.001f;
    ESP_LOGI(TAG, "Saved %s in %u ms (%.2f KB/s)", filename, tElapsed, (len / 1024.0f) / elapsedSec);

    _photoFiles.push_back(String(filename));
    _photoSizes.push_back(len);
    _usedBytes += len;
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
    auto it = std::find(_photoFiles.begin(), _photoFiles.end(), filename);
    if (it != _photoFiles.end()) {
        size_t idx = std::distance(_photoFiles.begin(), it);
        return _photoSizes[idx];
    }
    File file = SD.open(filename, FILE_READ);
    if (!file) return 0;
    size_t sz = file.size();
    file.close();
    return sz;
}

size_t SdStorage::getPhotoSizeByIndex(size_t index) {
    if (index < _photoSizes.size()) {
        return _photoSizes[index];
    }
    return 0;
}

uint64_t SdStorage::getTotalBytes() {
    if (!_mounted) return 0;
    return _totalBytes;
}

uint64_t SdStorage::getUsedBytes() {
    if (!_mounted) return 0;
    // Return cached DCIM media byte count immediately without calling SD.usedBytes() (FatFs f_getfree),
    // which traverses the entire FAT table over SPI and can trigger the 5s Task Watchdog reset.
    return _usedBytes;
}
