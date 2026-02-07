#ifndef CONFIG_H
#define CONFIG_H

#include "command_line_parser.h"

const uint16_t kDefaultHeight = 1080;
const uint16_t kDefaultWidth = 1920;
const uint16_t kDefaultFps = 30;
const int kDefaultFramesToEncode = 10;
const size_t kDefaultMaxPacketSize = 1460;

void InitializeFlags();

#endif  // CONFIG_H