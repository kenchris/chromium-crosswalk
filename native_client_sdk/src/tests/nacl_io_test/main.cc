// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "gtest/gtest.h"

#if defined(SEL_LDR)

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else

#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/var.h"
#include "ppapi_simple/ps_main.h"

#if defined(WIN32)
#include <Windows.h>
#undef PostMessage
#endif

class GTestEventListener : public ::testing::EmptyTestEventListener {
 public:
  // TestEventListener overrides.
  virtual void OnTestStart(const ::testing::TestInfo& test_info) {
    std::stringstream msg;
    msg << "start:" << test_info.test_case_name() << "." << test_info.name();
    pp::Instance(PSGetInstanceId()).PostMessage(msg.str());
  }

  virtual void OnTestPartResult(
      const ::testing::TestPartResult& test_part_result) {
    if (test_part_result.failed()) {
      std::stringstream msg;
      msg << "fail:" << test_part_result.file_name() << ","
          << test_part_result.line_number() << ","
          << test_part_result.summary();
      pp::Instance(PSGetInstanceId()).PostMessage(msg.str());
    }
  }

  virtual void OnTestEnd(const ::testing::TestInfo& test_info) {
    std::stringstream msg;
    msg << "end:" << test_info.test_case_name() << "." << test_info.name()
        << "," << (test_info.result()->Failed() ? "failed" : "ok");
    pp::Instance(PSGetInstanceId()).PostMessage(msg.str());
  }

  virtual void OnTestProgramEnd(const ::testing::UnitTest&) {
    pp::Instance(PSGetInstanceId()).PostMessage("testend");
  }
};

int example_main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::UnitTest::GetInstance()->listeners()
      .Append(new GTestEventListener());
  int result = RUN_ALL_TESTS();

  // When running as an automated test, we don't want the final message
  // ("testend") to be dropped, so don't exit. The web page will kill the
  // plugin if it needs to.
  while(1);

  // Silence the warning.
  return result;
}

// Register the function to call once the Instance Object is initialized.
// see: pappi_simple/ps_main.h
PPAPI_SIMPLE_REGISTER_MAIN(example_main);

#endif
