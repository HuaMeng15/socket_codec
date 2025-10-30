#include "log_system/log_system.h"
#include "tools/config.h"
#include "sender.h"
#include "receiver.h"

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

  return 0;
}