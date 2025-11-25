#pragma once

#include "GpsTypes.h"

namespace GpsService {

void warmup();
bool fetchFix(GpsFix& fix);

}  // namespace GpsService

