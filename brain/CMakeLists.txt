file(GLOB_RECURSE BRAIN_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

add_library(brain_core STATIC ${BRAIN_SRC})
target_include_directories(brain_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(brain_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(brain_core PRIVATE /W4)
    if(POP_ENABLE_WERROR)
        target_compile_options(brain_core PRIVATE /WX)
    endif()
else()
    target_compile_options(brain_core PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${POP_ENABLE_WERROR}>:-Werror>
        $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>>:-Og>
    )
endif()

target_link_libraries(brain_core PUBLIC common_core)

if(TARGET Boost::program_options)
    target_link_libraries(brain_core PUBLIC Boost::program_options)
elseif(DEFINED Boost_LIBRARIES)
    target_link_libraries(brain_core PUBLIC ${Boost_LIBRARIES})
endif()

add_executable(brain app/brain.cpp)
target_link_libraries(brain PRIVATE brain_core)
set_target_properties(brain PROPERTIES OUTPUT_NAME "brain")

add_executable(zidabot_replay app/zidabot_replay.cpp)
target_link_libraries(zidabot_replay PRIVATE brain_core)
set_target_properties(zidabot_replay PROPERTIES OUTPUT_NAME "zidabot_replay")
