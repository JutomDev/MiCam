#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include "config.h"

class WifiShare {
public:
    static bool start();
    static void stop();
    static void loop();
    static bool isRunning();
    static String getIP();

private:
    static WebServer _server;
    static DNSServer _dnsServer;
    static bool _running;

    static void handleRoot();
    static void handleSnapshot();
    static void handleCapture();
    static void handlePreset();
    static void handleAudioLevel();
    static void handleToggleClap();
    static void handleAudioThreshold();
    static void handleAudioGain();
    static void handleDownload();
    static void handleView();
    static void handleDelete();
    static void handleNotFound();
};
