#pragma once

#include <Preferences.h>

#include "../gps/GpsTypes.h"

namespace GeoBuffer {

void init();
bool empty();
size_t count();
void enqueue(const GpsFix& fix);
bool peek(GpsFix& fix);
void dropOldest();

}  // namespace GeoBuffer

