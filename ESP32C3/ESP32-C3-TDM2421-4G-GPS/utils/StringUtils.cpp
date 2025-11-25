#include "StringUtils.h"

namespace StringUtils {

bool splitCsvFields(const String& data, String* outFields, size_t expectedCount) {
    size_t idx = 0;
    int start = 0;
    while (idx < expectedCount && start <= data.length()) {
        int next = data.indexOf(',', start);
        if (next == -1) {
            next = data.length();
        }
        outFields[idx++] = data.substring(start, next);
        start = next + 1;
    }
    return idx == expectedCount;
}

}  // namespace StringUtils

