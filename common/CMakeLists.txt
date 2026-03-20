file(GLOB_RECURSE COMMON_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

add_library(common_core STATIC ${COMMON_SRC})
target_include_directories(common_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(common_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(common_core PRIVATE /W4)
    if(POP_ENABLE_WERROR)
        target_compile_options(common_core PRIVATE /WX)
    endif()
else()
    target_compile_options(common_core PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${POP_ENABLE_WERROR}>:-Werror>
        $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>>:-Og>
    )
endif()

target_link_libraries(common_core PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    ZLIB::ZLIB
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

if(TARGET Boost::system)
    target_link_libraries(common_core PUBLIC Boost::system)
elseif(TARGET Boost::boost)
    target_link_libraries(common_core PUBLIC Boost::boost)
endif()
