#include "sd_storage.h"
#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char* TAG = "SD_STORAGE";

bool SdStorage::_mounted = false;
uint16_t SdStorage::_nextPhotoIndex = 1;
uint64_t SdStorage::_totalBytes = 0;
uint64_t SdStorage::_usedBytes = 0;
SdStorage::PhotoEntry* SdStorage::_entries = nullptr;
uint16_t SdStorage::_photoCount = 0;

bool SdStorage::ensureCatalogAllocated() {
    if (_entries) return true;
    _entries = static_cast<PhotoEntry*>(
        heap_caps_malloc(MAX_PHOTOS * sizeof(PhotoEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_entries) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM photo catalog!");
        return false;
    }
    memset(_entries, 0, MAX_PHOTOS * sizeof(PhotoEntry));
    return true;
}

bool SdStorage::init() {
    _mounted = false;
    _totalBytes = 0;
    _usedBytes = 0;
    _photoCount = 0;
    ensureCatalogAllocated();

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
    _photoCount = 0;
    _usedBytes = 0;
    uint16_t maxIndex = 0;

    if (!_mounted || !ensureCatalogAllocated()) return;

    File root = SD.open(PHOTO_DIR);
    if (!root || !root.isDirectory()) {
        ESP_LOGE(TAG, "Cannot open %s directory", PHOTO_DIR);
        return;
    }

    uint16_t scanned = 0;
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            const char* rawName = file.name();
            if (rawName && rawName[0] != '\0') {
                char fullPath[32];
                int n = 0;
                if (strncmp(rawName, "/DCIM/", 6) == 0) {
                    n = snprintf(fullPath, sizeof(fullPath), "%s", rawName);
                } else if (rawName[0] == '/') {
                    n = snprintf(fullPath, sizeof(fullPath), "%s%s", PHOTO_DIR, rawName);
                } else {
                    n = snprintf(fullPath, sizeof(fullPath), "%s/%s", PHOTO_DIR, rawName);
                }

                if (n > 5 && n < static_cast<int>(sizeof(fullPath))) {
                    const char* ext = fullPath + (n - 4);
                    if (strcmp(ext, ".JPG") == 0 || strcmp(ext, ".jpg") == 0) {
                        size_t fsize = file.size();
                        if (fsize > 100) {
                            _usedBytes += fsize;
                            const char* idxPtr = strstr(fullPath, "IMG_");
                            if (idxPtr) {
                                uint16_t num = static_cast<uint16_t>(strtoul(idxPtr + 4, nullptr, 10));
                                if (num > maxIndex) {
                                    maxIndex = num;
                                }
                            }
                            if (_photoCount < MAX_PHOTOS) {
                                memcpy(_entries[_photoCount].path, fullPath, sizeof(_entries[_photoCount].path));
                                _entries[_photoCount].size = static_cast<uint32_t>(fsize);
                                _photoCount++;
                            }
                        }
                    }
                }
            }
        }
        file = root.openNextFile();
        if ((++scanned & 0x07) == 0) {
            vTaskDelay(1);
        }
    }
    root.close();

    if (_photoCount > 1) {
        std::sort(_entries, _entries + _photoCount,
                  [](const PhotoEntry& a, const PhotoEntry& b) { return strcmp(a.path, b.path) < 0; });
    }

    _nextPhotoIndex = maxIndex + 1;
    ESP_LOGI(TAG, "Scan complete: %u photos found (%llu KB). Next index: %u", (unsigned)_photoCount, _usedBytes / 1024,
             (unsigned)_nextPhotoIndex);
}

void SdStorage::refreshList() {
    scanPhotos();
}

uint16_t SdStorage::getPhotoCount() {
    return _photoCount;
}

const char* SdStorage::getPhotoPath(size_t index) {
    if (_entries && index < _photoCount) {
        return _entries[index].path;
    }
    return "";
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

    if (ensureCatalogAllocated() && _photoCount < MAX_PHOTOS) {
        snprintf(_entries[_photoCount].path, sizeof(_entries[_photoCount].path), "%s", filename);
        _entries[_photoCount].size = static_cast<uint32_t>(len);
        _photoCount++;
    }
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
        if (_entries) {
            for (uint16_t i = 0; i < _photoCount; ++i) {
                if (strcmp(_entries[i].path, filename) == 0) {
                    uint64_t sz = _entries[i].size;
                    _usedBytes = (_usedBytes >= sz) ? (_usedBytes - sz) : 0;
                    if (i + 1U < _photoCount) {
                        memmove(&_entries[i], &_entries[i + 1U],
                                static_cast<size_t>(_photoCount - 1U - i) * sizeof(PhotoEntry));
                    }
                    _photoCount--;
                    break;
                }
            }
        }
        return true;
    }

    ESP_LOGE(TAG, "Failed to delete file: %s", filename);
    return false;
}

size_t SdStorage::getPhotoSize(const char* filename) {
    if (!_mounted || !filename) return 0;
    if (_entries) {
        for (uint16_t i = 0; i < _photoCount; ++i) {
            if (strcmp(_entries[i].path, filename) == 0) {
                return _entries[i].size;
            }
        }
    }
    File file = SD.open(filename, FILE_READ);
    if (!file) return 0;
    size_t sz = file.size();
    file.close();
    return sz;
}

size_t SdStorage::getPhotoSizeByIndex(size_t index) {
    if (_entries && index < _photoCount) {
        return _entries[index].size;
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
