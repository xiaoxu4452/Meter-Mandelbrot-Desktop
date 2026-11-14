// ---------------------------------------------------------------------------
// mb_bridgecheck.cpp — standalone diagnostic for the Boost <-> MPFR bridge.
// Runs bridgeSelfTest(), then optionally validates a JSON orbit file whose
// path is given on the command line.  Prints the accumulated log and returns
// 0 on success / 1 on failure.
// ---------------------------------------------------------------------------
#include "mb_bridge.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
  std::printf("Boost <-> MPFR reference-orbit bridge diagnostic\n\n");

  std::string log;
  bool ok = mbc::bridgeSelfTest(log);
  std::fputs(log.c_str(), stdout);

  if (argc > 1) {
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
      std::printf("[bridgecheck] cannot open JSON file: %s\n", argv[1]);
      return 1;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string checkLog;
    const bool c = mbc::bridgeCheckJson(ss.str(), checkLog);
    std::fputs(checkLog.c_str(), stdout);
    ok = ok && c;
  } else {
    std::printf("[bridgecheck] no JSON file given; self-test only\n");
  }

  std::printf("\nRESULT: %s\n", ok ? "ALL PASS" : "FAIL");
  return ok ? 0 : 1;
}
