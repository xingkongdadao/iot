#pragma once

#include <Arduino.h>

void sim_at_wait();
bool sim_at_cmd(const String& cmd);
bool sim_at_send(char c);
bool sim_at_cmd_with_response(const String& cmd, String& response, uint32_t timeoutMs = 5000);
bool waitForSubstring(const String& expect, uint32_t timeoutMs, String* response = nullptr);
bool sim_at_cmd_expect(const String& cmd,
                       const String& expect,
                       uint32_t timeoutMs = 5000,
                       String* response = nullptr);

