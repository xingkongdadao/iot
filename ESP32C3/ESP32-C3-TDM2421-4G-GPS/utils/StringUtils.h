#pragma once

#include <Arduino.h>

namespace StringUtils {

bool splitCsvFields(const String& data, String* outFields, size_t expectedCount);

}  // namespace StringUtils

