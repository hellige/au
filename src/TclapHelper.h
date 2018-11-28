#pragma once

#include <tclap/CmdLine.h>

#include <functional>

namespace au {

class TclapHelper {
  struct UsageVisitor : public TCLAP::Visitor {
    std::function<void()> usage;
    UsageVisitor(const std::function<void()> &usage) : usage(usage) {}
    void visit() override {
      usage();
      exit(0);
    };
  };

  struct UsageOutput : public TCLAP::StdOutput {
    std::function<void()> usage_;
    UsageOutput(const std::function<void()> &usage) : usage_(usage) {}
    void failure(TCLAP::CmdLineInterface &, TCLAP::ArgException &e) override {
      std::cerr << e.error() << std::endl;
      usage_();
      exit(1);
    }
    void usage(TCLAP::CmdLineInterface &) override {
      usage_();
    }
  };

  UsageVisitor usageVisitor_;
  TCLAP::CmdLine cmd_;
  TCLAP::SwitchArg help_;

public:
  explicit TclapHelper(std::function<void()> usage)
      : usageVisitor_(usage),
        cmd_("", ' ', "", false),
        help_("h", "help", "help", cmd_, false, &usageVisitor_) {
  }

  TCLAP::CmdLine &cmd() { return cmd_; }

  bool parse(int argc, const char * const *argv) {
    try {
      UsageOutput output(usageVisitor_.usage);
      cmd_.setOutput(&output);
      cmd_.parse(argc-1, argv+1);
      return true;
    } catch (TCLAP::ArgException &e) {
      std::cerr << "error: " << e.error() << " for arg " << e.argId()
                << std::endl;
      return false;
    }
  }
};

}