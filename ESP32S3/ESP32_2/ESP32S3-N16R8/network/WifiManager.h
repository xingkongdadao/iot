#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include "../config/Config.h"

extern WebServer configServer;
extern bool configPortalActive;
extern unsigned long configPortalStartTime;
extern unsigned long configPortalLastActivity;

void loadStoredWiFiCredentials();
bool persistWiFiCredentials(const String& newSsid, const String& newPassword);
bool connectWiFi();
void startConfigPortal();
void sendConfigPortalPage(const String& message = "");
void handleConfigPortalRoot();
void handleConfigPortalSave();
void handleConfigPortalNotFound();
void beginConfigServer();
void handleConfigServer();
void announceConfigServerAddress();
void ensureConfigAP();

const String& getActiveSsid();
const String& getActivePassword();
bool hasStoredCredentials();

