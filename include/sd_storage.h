#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include "config.h"

class SdStorage {
public:
    static bool init();
    static bool isAvailable();
    static uint16_t getPhotoCount();
    static const char* getPhotoPath(size_t index);
    static bool savePhoto(const uint8_t* buffer, size_t len, char* outFilename = nullptr, size_t maxLen = 0);
    static bool deletePhoto(const char* filename);
    static size_t getPhotoSize(const char* filename);
    static size_t getPhotoSizeByIndex(size_t index);
    static uint64_t getTotalBytes();
    static uint64_t getUsedBytes();
    static void refreshList();

private:
    struct PhotoEntry {
        char path[32];
        uint32_t size;
    };

    static constexpr size_t MAX_PHOTOS = 512;
    static bool _mounted;
    static uint16_t _nextPhotoIndex;
    static uint64_t _totalBytes;
    static uint64_t _usedBytes;
    static PhotoEntry* _entries;
    static uint16_t _photoCount;
    static bool ensureCatalogAllocated();
    static void scanPhotos();
};
