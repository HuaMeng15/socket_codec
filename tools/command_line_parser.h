#ifndef COMMAND_LINE_PARSER_H
#define COMMAND_LINE_PARSER_H

#include <iostream>
#include <string>
#include <unordered_map>
#include <memory>
#include <sstream>
#include <vector>
#include <type_traits>

class FlagBase {
public:
  FlagBase(const std::string& name, const std::string& help)
      : name_(name), help_(help) {}
  virtual ~FlagBase() = default;

  const std::string& GetName() const { return name_; }
  const std::string& GetHelp() const { return help_; }
  virtual void ParseValue(const std::string& value) = 0;

protected:
  std::string name_;
  std::string help_;
};

template <typename T>
class Flag : public FlagBase {
public:
  Flag(const std::string& name, const T& default_val, const std::string& help)
      : FlagBase(name, help), value_(default_val) {}

  void ParseValue(const std::string& value) override {
      std::istringstream iss(value);
      if (!(iss >> value_)) {
          throw std::invalid_argument("Invalid value for flag --" + name_ + ": " + value);
      }
  }

  const T& GetValue() const { return value_; }

private:
  T value_;
};

class CmdLineParser {
public:
  CmdLineParser(const CmdLineParser&) = delete;
  CmdLineParser& operator=(const CmdLineParser&) = delete;

  static CmdLineParser& GetInstance() {
      static CmdLineParser instance;
      return instance;
  }
  void AddStringFlag(const std::string& name, const std::string& default_val, const std::string& help) {
      AddFlag<std::string>(name, default_val, help);
  }

  void AddIntFlag(const std::string& name, int default_val, const std::string& help) {
      AddFlag<int>(name, default_val, help);
  }

  void Parse(int argc, char* argv[]);

  template <typename T>
  const T& GetFlag(const std::string& name) const {
      auto it = flags_.find(name);
      if (it == flags_.end()) {
          throw std::invalid_argument("Flag --" + name + " not defined");
      }
      Flag<T>* flag = dynamic_cast<Flag<T>*>(it->second.get());
      if (!flag) {
          throw std::invalid_argument("Flag --" + name + " type mismatch");
      }
      return flag->GetValue();
  }

  void PrintHelp() const;

private:
  CmdLineParser() = default;

  std::unordered_map<std::string, std::unique_ptr<FlagBase>> flags_;

  template <typename T>
  void AddFlag(const std::string& name, const T& default_val, const std::string& help) {
      if (flags_.count(name)) {
          throw std::invalid_argument("Flag --" + name + " already defined");
      }
      flags_[name] = std::make_unique<Flag<T>>(name, default_val, help);
  }

  std::string GetTypeStr(FlagBase* flag) const;
};

#endif // COMMAND_LINE_PARSER_H