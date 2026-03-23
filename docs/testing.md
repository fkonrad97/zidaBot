# Testing Guide

## Overview

The test suite uses **GoogleTest v1.14.0** (fetched automatically by CMake). There are 55 unit
tests across four executables, covering the pure-logic layers of the stack:

| Executable | File | Tests | What it covers |
|---|---|---|---|
| `test_orderbook` | `tests/common/test_orderbook.cpp` | 14 | `OrderBook` — insert, sort, update, remove, depth cap, clear, validate |
| `test_orderbook_controller` | `tests/common/test_orderbook_controller.cpp` | 12 | `OrderBookController` — state machine, bridging, seq gap, crossing, B7 |
| `test_arb_detector` | `tests/brain/test_arb_detector.cpp` | 16 | `ArbDetector::scan()` — cross detection, min/max spread, staleness, rate limit, B1/B2/B5, F4 |
| `test_unified_book` | `tests/brain/test_unified_book.cpp` | 13 | `UnifiedBook`, `JsonParsers` — routing, status events, schema errors |

Tests deliberately avoid network, threads, and disk I/O. `WsClient`, `WsServer`, and
`FilePersistSink` are not tested here (integration / sanitizer scope).

---

## Quick start

```bash
# 1. Configure (tests are ON by default)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# 2. Build only the test targets (fast — skips pop/brain executables)
cmake --build build --target test_orderbook test_orderbook_controller \
                            test_arb_detector test_unified_book -j$(nproc)

# 3. Run all tests
cd build && ctest --output-on-failure
```

Expected output:
```
100% tests passed, 0 tests failed out of 55
Total Test time (real) =   0.43 sec
```

---

## Common commands

### Run a specific test binary directly
```bash
./build/tests/test_arb_detector
```

### Filter to a single test case
```bash
./build/tests/test_arb_detector --gtest_filter="ArbDetector.DetectsCrossWhenSellBidExceedsBuyAsk"
```

### Filter by prefix (all ArbDetector tests)
```bash
./build/tests/test_arb_detector --gtest_filter="ArbDetector.*"
```

### Run with verbose output
```bash
./build/tests/test_orderbook --gtest_filter="*" --gtest_print_time=1
```

### Run with ctest and filter by regex
```bash
cd build && ctest -R "ArbDetector" --output-on-failure
```

### Run only failed tests again (after a partial failure)
```bash
cd build && ctest --rerun-failed --output-on-failure
```

---

## Sanitizer builds

### AddressSanitizer + UBSan (catch memory errors and undefined behaviour)
```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build-asan --target test_orderbook test_orderbook_controller \
                                  test_arb_detector test_unified_book -j$(nproc)
cd build-asan && ctest --output-on-failure
```

### ThreadSanitizer (catch data races)
```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build build-tsan --target test_orderbook test_orderbook_controller \
                                  test_arb_detector test_unified_book -j$(nproc)
cd build-tsan && ctest --output-on-failure
```

> **Note:** ASAN and TSAN cannot be enabled simultaneously (CMake enforces this).

---

## Disabling tests

Tests are enabled by default. To disable:
```bash
cmake -B build -DBUILD_TESTS=OFF
```

---

## Adding a new test

1. Create `tests/<area>/test_<component>.cpp` using GoogleTest:
   ```cpp
   #include <gtest/gtest.h>
   #include "your/header.hpp"

   TEST(SuiteName, TestName) {
       // arrange
       // act
       // assert
       EXPECT_EQ(actual, expected);
   }
   ```
2. Add the executable and link targets in `tests/CMakeLists.txt`:
   ```cmake
   add_executable(test_mycomponent <area>/test_mycomponent.cpp)
   target_link_libraries(test_mycomponent PRIVATE common_core GTest::gtest_main)
   gtest_discover_tests(test_mycomponent)
   ```
3. Rebuild and run — `gtest_discover_tests` auto-registers each `TEST()` case with CTest.

---

## Test design rules

- **No network, no threads, no file I/O** in unit tests. Mock time-dependent behaviour with
  controllable `ts_ns` parameters rather than sleeping.
- **Build minimal fixtures**: prefer `make_snap()` / `make_inc()` helpers over duplicating JSON.
- **One assertion per concept** where possible — a failing test should pinpoint the broken invariant.
- **Name after the invariant, not the mechanism**: `CrossedBookAfterIncrementalTriggersResync` is
  clearer than `TestOnIncrementCase3`.
