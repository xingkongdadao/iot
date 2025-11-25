#pragma once

#include <Arduino.h>

struct ParsedUrl {
    String host;
    String path = "/";
    uint16_t port = 80;
    bool https = false;
};

bool parseUrl(const String& url, ParsedUrl& parsed);

