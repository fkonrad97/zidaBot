file(GLOB_RECURSE FEED_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

add_library(feed_core STATIC ${FEED_SRC})
target_include_directories(feed_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(feed_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(feed_core PRIVATE /W4)
    if(POP_ENABLE_WERROR)
        target_compile_options(feed_core PRIVATE /WX)
    endif()
else()
    target_compile_options(feed_core PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${POP_ENABLE_WERROR}>:-Werror>
        $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>>:-Og>
    )
endif()

target_link_libraries(feed_core PUBLIC common_core)

if(TARGET Boost::program_options)
    target_link_libraries(feed_core PUBLIC Boost::program_options)
elseif(DEFINED Boost_LIBRARIES)
    target_link_libraries(feed_core PUBLIC ${Boost_LIBRARIES})
endif()

# Main executable
add_executable(feed app/main.cpp)
target_link_libraries(feed PRIVATE feed_core)
set_target_properties(feed PROPERTIES OUTPUT_NAME "feed")