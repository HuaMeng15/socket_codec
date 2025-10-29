#include "log_system/log_system.h"
#include "tools/config.h"

int main(int argc, char* argv[]) {
  // Parse command-line arguments
  auto& parser = CmdLineParser::GetInstance();
  parser.Parse(argc, argv);

  std::string filename = parser.GetFlag<std::string>("file");
  if (filename == "NONE") {
      LOG(INFO) << "Running in sender mode";
  } else {
      LOG(INFO) << "Running in receiver mode, saving to file: " << filename;
  }

  LOG(INFO) << "Receiver IP: " << parser.GetFlag<std::string>("ip");
  LOG(INFO) << "Receiver Port: " << parser.GetFlag<int>("port");
  LOG(INFO) << "Video Width: " << parser.GetFlag<int>("width");
  LOG(INFO) << "Video Height: " << parser.GetFlag<int>("height");
  LOG(INFO) << "Video FPS: " << parser.GetFlag<int>("fps");

  return 0;
}