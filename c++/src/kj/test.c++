// Copyright (c) 2013-2014 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <atomic>

#include "test.h"
#include "main.h"
#include <kj/glob-filter.h>
#include "io.h"
#include "miniposix.h"
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include "time.h"
#ifndef _WIN32
#include <sys/mman.h>
#endif

namespace kj {

namespace {

TestCase* testCasesHead = nullptr;
TestCase** testCasesTail = &testCasesHead;

size_t benchmarkIterCount = 1;

}  // namespace

TestCase::TestCase(const char* file, uint line, const char* description)
    : file(file), line(line), description(description), next(nullptr), prev(testCasesTail),
      matchedFilter(false) {
  *prev = this;
  testCasesTail = &next;
}

TestCase::~TestCase() {
  *prev = next;
  if (next == nullptr) {
    testCasesTail = prev;
  } else {
    next->prev = prev;
  }
}

size_t TestCase::iterCount() {
  return benchmarkIterCount;
}

// =======================================================================================

namespace {

class TestExceptionCallback: public ExceptionCallback {
public:
  TestExceptionCallback(const ProcessContext& context)
      : context(context), mainThreadCallback(kj::none) {}

  bool failed() { return sawError; }

  void fail() const {
    sawError.store(true);
    KJ_IF_SOME(callback, mainThreadCallback) {
      callback.fail();
    }
  }

  void logMessage(LogSeverity severity, const char* file, int line, int contextDepth,
                  String&& text) override {
    void* traceSpace[32]{};
    auto trace = getStackTrace(traceSpace, 2);

    if (text.size() == 0) {
      text = kj::heapString("expectation failed");
    }

    text = kj::str(kj::repeat('_', contextDepth), file, ':', line, ": ", kj::mv(text));

    if (severity == LogSeverity::ERROR || severity == LogSeverity::FATAL) {
      fail();
      context.error(kj::str(text, "\nstack: ", stringifyStackTraceAddresses(trace), stringifyStackTrace(trace)));
    } else {
      context.warning(text);
    }
  }

  Function<void(Function<void()>)> getThreadInitializer() override {
    return [&](Function<void()> func) {
      KJ_IF_SOME(callback, mainThreadCallback) {
        TestExceptionCallback exceptionCallback(context, callback);
        func();
      } else {
        TestExceptionCallback exceptionCallback(context, *this);
        func();
      }
    };
  }

private:
  const ProcessContext& context;
  // In the case where a thread spawns a thread, we report failure to the main thread's
  // TestExceptionCallback, which I assume stays alive as long as all child threads.  If we reported
  // failures to a thread's parent, we'd have to worry about a child thread spawning a grandchild
  // thread and then immediately dying, leaving the grandchild thread with a dangling pointer to the
  // now-dead child's TestExceptionCallback.
  kj::Maybe<const TestExceptionCallback&> mainThreadCallback;
  // sawError is mutable because we need to modify it in const-for-multithreadedness methods.
  mutable std::atomic_bool sawError = false;

  TestExceptionCallback(const ProcessContext& context,
      const TestExceptionCallback& topLevelCallback)
      : context(context), mainThreadCallback(topLevelCallback) {}

};

TimePoint readClock() {
  return systemPreciseMonotonicClock().now();
}

}  // namespace

class TestRunner {
public:
  explicit TestRunner(ProcessContext& context)
      : context(context), useColor(isatty(STDOUT_FILENO)) {}

  MainFunc getMain() {
    return MainBuilder(context, "KJ Test Runner (version not applicable)",
        "Run all tests that have been linked into the binary with this test runner.")
        .addOptionWithArg({'f', "filter"}, KJ_BIND_METHOD(*this, setFilter), "<file>[:<line>]",
            "Run only the specified test case(s). You may use a '*' wildcard in <file>. You may "
            "also omit any prefix of <file>'s path; test from all matching files will run. "
            "You may specify multiple filters; any test matching at least one filter will run. "
            "<line> may be a range, e.g. \"100-500\".")
        .addOption({'l', "list"}, KJ_BIND_METHOD(*this, setList),
            "List all test cases that would run, but don't run them. If --filter is specified "
            "then only the match tests will be listed.")
        .addOptionWithArg({'b', "benchmark"}, KJ_BIND_METHOD(*this, setBenchmarkIters), "<iters>",
            "Specifies that any benchmarks in the tests should run for <iters> iterations. "
            "If not specified, then count is 1, which simply tests that the benchmarks function.")
        .callAfterParsing(KJ_BIND_METHOD(*this, run))
        .build();
  }

  MainBuilder::Validity setFilter(StringPtr pattern) {
    hasFilter = true;
    ArrayPtr<const char> filePattern = pattern;
    uint minLine = kj::minValue;
    uint maxLine = kj::maxValue;

    KJ_IF_SOME(colonPos, pattern.findLast(':')) {
      char* end;
      StringPtr lineStr = pattern.slice(colonPos + 1);

      bool parsedRange = false;
      minLine = strtoul(lineStr.cStr(), &end, 0);
      if (end != lineStr.begin()) {
        if (*end == '-') {
          // A range.
          const char* part2 = end + 1;
          maxLine = strtoul(part2, &end, 0);
          if (end > part2 && *end == '\0') {
            parsedRange = true;
          }
        } else if (*end == '\0') {
          parsedRange = true;
          maxLine = minLine;
        }
      }

      if (parsedRange) {
        // We have an exact line number.
        filePattern = pattern.first(colonPos);
      } else {
        // Can't parse as a number. Maybe the colon is part of a Windows path name or something.
        // Let's just keep it as part of the file pattern.
        minLine = kj::minValue;
        maxLine = kj::maxValue;
      }
    }

    kj::GlobFilter filter(filePattern);

    for (TestCase* testCase = testCasesHead; testCase != nullptr; testCase = testCase->next) {
      if (!testCase->matchedFilter && filter.matches(testCase->file) &&
          testCase->line >= minLine && testCase->line <= maxLine) {
        testCase->matchedFilter = true;
      }
    }

    return true;
  }

  MainBuilder::Validity setList() {
    listOnly = true;
    return true;
  }

  MainBuilder::Validity setBenchmarkIters(StringPtr param) {
    KJ_IF_SOME(i, param.tryParseAs<size_t>()) {
      benchmarkIterCount = i;
      return true;
    } else {
      return "expected an integer";
    }
  }

  MainBuilder::Validity run() {
    if (testCasesHead == nullptr) {
      return "no tests were declared";
    }

    // Find the common path prefix of all filenames, so we can strip it off.
    ArrayPtr<const char> commonPrefix = StringPtr(testCasesHead->file);
    for (TestCase* testCase = testCasesHead; testCase != nullptr; testCase = testCase->next) {
      for (size_t i: kj::indices(commonPrefix)) {
        if (testCase->file[i] != commonPrefix[i]) {
          commonPrefix = commonPrefix.first(i);
          break;
        }
      }
    }

    // Back off the prefix to the last '/'.
    while (commonPrefix.size() > 0 && commonPrefix.back() != '/' && commonPrefix.back() != '\\') {
      commonPrefix = commonPrefix.first(commonPrefix.size() - 1);
    }

    // Run the testts.
    uint passCount = 0;
    uint failCount = 0;
    for (TestCase* testCase = testCasesHead; testCase != nullptr; testCase = testCase->next) {
      if (!hasFilter || testCase->matchedFilter) {
        auto name = kj::str(testCase->file + commonPrefix.size(), ':', testCase->line,
                            ": ", testCase->description);

        write(BLUE, "[ TEST ]", name);

        if (!listOnly) {
          bool currentFailed = true;
          auto start = readClock();
          KJ_IF_SOME(exception, runCatchingExceptions([&]() {
            TestExceptionCallback exceptionCallback(context);
            testCase->run();
            currentFailed = exceptionCallback.failed();
          })) {
            context.error(kj::str(exception));
          }
          auto end = readClock();

          auto message = kj::str(name, " (", end - start, ")");

          if (currentFailed) {
            write(RED, "[ FAIL ]", message);
            ++failCount;
          } else {
            write(GREEN, "[ PASS ]", message);
            ++passCount;
          }
        }
      }
    }

    if (passCount > 0) write(GREEN, kj::str(passCount, " test(s) passed"), "");
    if (failCount > 0) write(RED, kj::str(failCount, " test(s) failed"), "");
    context.exit();

    KJ_UNREACHABLE;
  }

private:
  ProcessContext& context;
  bool useColor;
  bool hasFilter = false;
  bool listOnly = false;

  enum Color {
    RED,
    GREEN,
    BLUE
  };

  void write(StringPtr text) {
    FdOutputStream(STDOUT_FILENO).write(text.asBytes());
  }

  void write(Color color, StringPtr prefix, StringPtr message) {
    StringPtr startColor, endColor;
    if (useColor) {
      switch (color) {
        case RED:   startColor = "\033[0;1;31m"; break;
        case GREEN: startColor = "\033[0;1;32m"; break;
        case BLUE:  startColor = "\033[0;1;34m"; break;
      }
      endColor = "\033[0m";
    }

    String text = kj::str(startColor, prefix, endColor, ' ', message, '\n');
    write(text);
  }
};

}  // namespace kj

KJ_MAIN(kj::TestRunner);
