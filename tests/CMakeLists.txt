include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# ── common ────────────────────────────────────────────────────────────────────
add_executable(test_orderbook
    common/test_orderbook.cpp
)
target_link_libraries(test_orderbook PRIVATE common_core GTest::gtest_main)

add_executable(test_orderbook_controller
    common/test_orderbook_controller.cpp
)
target_link_libraries(test_orderbook_controller PRIVATE common_core GTest::gtest_main)

# ── brain ─────────────────────────────────────────────────────────────────────
add_executable(test_arb_detector
    brain/test_arb_detector.cpp
)
target_link_libraries(test_arb_detector PRIVATE brain_core GTest::gtest_main)

add_executable(test_unified_book
    brain/test_unified_book.cpp
)
target_link_libraries(test_unified_book PRIVATE brain_core GTest::gtest_main)

# ── register ──────────────────────────────────────────────────────────────────
gtest_discover_tests(test_orderbook)
gtest_discover_tests(test_orderbook_controller)
gtest_discover_tests(test_arb_detector)
gtest_discover_tests(test_unified_book)
