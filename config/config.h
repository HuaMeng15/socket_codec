#ifndef CONFIG_H
#define CONFIG_H

#include "command_line_parser.h"

const uint16_t kDefaultHeight = 1080;
const uint16_t kDefaultWidth = 1920;
const uint16_t kDefaultFps = 30;
const int kDefaultFramesToEncode = 10;
const size_t kDefaultMaxPacketSize = 1460;
// Single source of truth for the startup bitrate. The GCC initial target, the
// encoder's initial rate, and the pacer's initial rate are all aligned to this
// so the first frames aren't oversized for the startup pacing rate. The
// cc_initial_bitrate_kbps flag defaults to this value.
const int kDefaultInitialBitrateKbps = 500;

void InitializeFlags();

#endif  // CONFIG_H