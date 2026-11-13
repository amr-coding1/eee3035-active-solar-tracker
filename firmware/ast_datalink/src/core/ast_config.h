// AST data link - compile-time constants shared by every module.
// Implements docs/CONTRACT.md v1.5 (sections quoted next to each constant).
// Portable C++17: no Arduino headers in firmware/core/.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ast {

constexpr const char* kContractVersion = "1.5";  // section 2

// --- 7.2 local buffer -------------------------------------------------------
// The ring is split into small "segment" files so that rewriting one record
// only rewrites one flash block (LittleFS is copy-on-write: a single big file
// would be re-copied from the changed block to its end on every write).
constexpr uint32_t kRecordsPerSegment = 96;   // 96 * 40 B = 3840 B < one 4096 B block
constexpr uint32_t kSegments = 120;
constexpr uint32_t kRingCapacity = kRecordsPerSegment * kSegments;  // 11,520 = 8 days at 1/min

// Storage "file ids" (see ast_storage.h). 0..kSegments-1 are data segments.
constexpr uint16_t kFileMetaA = 1000;
constexpr uint16_t kFileMetaB = 1001;

// --- 7.3 upload ---------------------------------------------------------------
constexpr size_t kMaxBatch = 960;            // ThingSpeak bulk-update limit
constexpr int kUploadMinDefault = 15;
constexpr int kUploadMinLow = 5;
constexpr int kUploadMinHigh = 60;
constexpr int kUploadSecond = 20;            // uploads start at second 20 of the minute
constexpr int kBatchSpacingS = 16;           // >= 16 s between writes (ThingSpeak allows 1 per 15 s)
constexpr int kBackoffS[] = {60, 120, 240, 480, 900};  // 1, 2, 4, 8, 15 min (cap)
constexpr int kFault9AfterS = 3600;          // fault 9 after 60 min without success
constexpr int kReadbackDelayS = 30;          // read-back no earlier than 30 s after a doubtful request
constexpr int kHttpTimeoutMs = 30000;        // 7.4

// --- 7.1 time -----------------------------------------------------------------
constexpr int kRtcSetThresholdS = 2;         // set RTC when |RTC - NTP| > 2 s
// Any clock earlier than this is treated as "lost time" (2026-01-01 00:00:00 UTC).
constexpr int64_t kMinPlausibleUnix = 1767225600;

// --- 7.4 ------------------------------------------------------------------------
constexpr int kWatchdogS = 180;

// --- fault codes, section 4 -----------------------------------------------------
constexpr uint8_t kFaultNone = 0;
constexpr uint8_t kFaultSensorTrk = 1;
constexpr uint8_t kFaultSensorFix = 2;
constexpr uint8_t kFaultSensorUse = 3;
constexpr uint8_t kFaultClock = 4;
constexpr uint8_t kFaultSunSensor = 5;
constexpr uint8_t kFaultOverflow = 6;
constexpr uint8_t kFaultLimit = 7;
constexpr uint8_t kFaultMotor = 8;
constexpr uint8_t kFaultUpload = 9;

constexpr uint16_t kRunMax = 999;            // section 5

}  // namespace ast
