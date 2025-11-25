#pragma once

#include "../gps/GpsTypes.h"

namespace CellularClient {

bool ensureReady();
bool upload(const GpsFix& fix);

}  // namespace CellularClient

