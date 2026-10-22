#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <vector>
#include "config.h"

class SdStorage {
public:
    static bool init();
    static bool isAvailable();
    static uint16_t getPhotoCount();
    static bool savePhoto(const uint8_t* buffer, size_t len, char* outFilename = nullptr, size_t maxLen = 0);
    static const std::vector<String>& getPhotoList();
    static bool deletePhoto(const char* filename);
    static size_t getPhotoSize(const char* filename);
    static uint64_t getTotalBytes();
    static uint64_t getUsedBytes();
    static void refreshList();

private:
    static bool _mounted;
    static uint16_t _nextPhotoIndex;
    static std::vector<String> _photoFiles;
    static void scanPhotos();
};
