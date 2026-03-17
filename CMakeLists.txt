cmake_minimum_required(VERSION 3.20)

# ---- Policies ----
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(trading_stack VERSION 0.1.0 LANGUAGES CXX)

# ---- Build type default ----
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "No build type specified — defaulting to Release")
endif()

# ---- Global defaults ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(POP_ENABLE_WERROR "Treat warnings as errors" OFF)

# ---- Binary hardening (GCC/Clang, Release builds) ----
if(NOT MSVC)
    # Stack protection, buffer overflow detection, position-independent executable.
    # _FORTIFY_SOURCE requires optimisation (-O1 or higher); guard against Debug builds.
    add_compile_options(
        -fstack-protector-strong
        -fPIE
        $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
    )
    add_link_options(-pie -Wl,-z,relro,-z,now)
endif()

# ---- Sanitizer options (off by default; enable for dev/CI) ----
option(ENABLE_ASAN "Enable AddressSanitizer + UBSan" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ASAN and TSAN cannot be enabled simultaneously")
endif()

if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer + UBSan enabled")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# ---- External dependencies (resolved once; propagated via target links) ----
include(FetchContent)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)

find_package(Boost CONFIG QUIET COMPONENTS system program_options)
if(NOT Boost_FOUND)
    find_package(Boost REQUIRED COMPONENTS system program_options)
endif()

# Hash pin ensures supply-chain integrity: the downloaded archive is verified
# against a known-good SHA-256 before extraction.
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---- Subprojects ----
add_subdirectory(common)
add_subdirectory(pop)
add_subdirectory(brain)

message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "ASAN: ${ENABLE_ASAN}  TSAN: ${ENABLE_TSAN}")
