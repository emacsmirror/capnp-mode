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

#include "memory.h"
#include "debug.h"
#include <stdlib.h>

namespace kj {

const NullDisposer NullDisposer::instance = NullDisposer();

namespace _ {

#if KJ_ASSERT_PTR_COUNTERS

void atomicPtrCounterAssertionFailed(char const* reason) {
  KJ_FAIL_ASSERT("ptr counter contract violated", reason);

  // Really make sure we abort.
  KJ_KNOWN_UNREACHABLE(abort());
}

#endif // KJ_ASSERT_PTR_COUNTERS

void throwWrongDisposerError() {
  KJ_FAIL_REQUIRE("When disowning an object, disposer must be equal to Own's disposer");
}

}

}  // namespace kj
