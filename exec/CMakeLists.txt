file(GLOB_RECURSE EXEC_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

add_library(exec_core STATIC ${EXEC_SRC})
target_include_directories(exec_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(exec_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(exec_core PRIVATE /W4)
    if(POP_ENABLE_WERROR)
        target_compile_options(exec_core PRIVATE /WX)
    endif()
else()
    target_compile_options(exec_core PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${POP_ENABLE_WERROR}>:-Werror>
        $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>>:-Og>
    )
endif()

target_link_libraries(exec_core PUBLIC common_core brain_core)

if(TARGET Boost::program_options)
    target_link_libraries(exec_core PUBLIC Boost::program_options)
elseif(DEFINED Boost_LIBRARIES)
    target_link_libraries(exec_core PUBLIC ${Boost_LIBRARIES})
endif()

add_executable(exec app/exec.cpp)
target_link_libraries(exec PRIVATE exec_core)
set_target_properties(exec PROPERTIES OUTPUT_NAME "exec")
