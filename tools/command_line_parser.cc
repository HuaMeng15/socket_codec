#include "command_line_parser.h"

void CmdLineParser::Parse(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help") {
        PrintHelp();
        exit(0);
    }
    if (arg.substr(0, 2) != "--") {
        throw std::invalid_argument("Invalid argument format: " + arg + " (use --name=value or --bool-flag)");
    }

    std::string flag_part = arg.substr(2);
    auto flag_it = flags_.find(flag_part);

    size_t eq_pos = flag_part.find('=');
    if (eq_pos == std::string::npos) {
        throw std::invalid_argument("Flag " + arg + " requires a value (use --name=value)");
    }
    std::string name = flag_part.substr(0, eq_pos);
    std::string value = flag_part.substr(eq_pos + 1);

    flag_it = flags_.find(name);
    if (flag_it == flags_.end()) {
        throw std::invalid_argument("Unknown flag: --" + name);
    }
    flag_it->second->ParseValue(value);
  }
}

void CmdLineParser::PrintHelp() const {
  std::cout << "Usage: program [options]\nOptions:\n";
  for (const auto& [name, flag] : flags_) {
      std::string type_str = GetTypeStr(flag.get());
      std::cout << "  --" << name << "=<" << type_str << ">  " << flag->GetHelp() << "\n";
  }
  std::cout << "  --help  Show this help message\n";
}

std::string CmdLineParser::GetTypeStr(FlagBase* flag) const {
  if (dynamic_cast<Flag<std::string>*>(flag)) return "string";
  if (dynamic_cast<Flag<int>*>(flag)) return "int";
  if (dynamic_cast<Flag<bool>*>(flag)) return "bool";
  return "unknown";
}