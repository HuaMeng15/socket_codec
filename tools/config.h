#ifndef CONFIG_H
#define CONFIG_H

#include "command_line_parser.h"

const uint16_t kDefaultHeight = 1080;
const uint16_t kDefaultWidth = 1920;
const uint16_t kDefaultFps = 30;

struct FlagInitializer {
    FlagInitializer() {
        auto& parser = CmdLineParser::GetInstance();
        parser.AddStringFlag("ip", "127.0.0.1", "receiver IP address");
        parser.AddIntFlag("port", 8888, "receiver port");
        parser.AddStringFlag("file", "NONE", "NONE if is sender, otherwise specify the file for receiver to save");
        parser.AddIntFlag("width", kDefaultWidth, "video width");
        parser.AddIntFlag("height", kDefaultHeight, "video height");
        parser.AddIntFlag("fps", kDefaultFps, "video fps");
    }
};

static FlagInitializer flag_initializer;

#endif // CONFIG_H