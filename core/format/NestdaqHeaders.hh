// Single entry point for the nestdaq-user-impl data format headers.
//
// TimeFrameHeader.h sets "#pragma pack(2)" without restoring it, which would also pack every
// declaration that follows (including standard library classes) and break their ABI.
// Always include the nestdaq headers through this file.
#pragma once

#include <cstdio>  // TimeFrameHeader.h uses printf without declaring it.

#pragma pack(push)
#include "AmQStrTdcData.h"
#include "FileSinkHeader.h"
#include "FileSinkTrailer.h"
#include "HeartbeatFrameHeader.h"
#include "SubTimeFrameHeader.h"
#include "TimeFrameHeader.h"
#pragma pack(pop)
