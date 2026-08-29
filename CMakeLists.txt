cmake_minimum_required(VERSION 3.25)
project(faultline VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FAULTLINE_BUILD_TESTS "Build Faultline tests" ON)
option(FAULTLINE_ENABLE_SANITIZERS "Enable ASan and UBSan" OFF)

add_library(faultline_core
    src/config.cpp
    src/control_server.cpp
    src/log.cpp
    src/proxy.cpp
)
add_library(Faultline::core ALIAS faultline_core)

target_include_directories(faultline_core PUBLIC include)
target_compile_options(faultline_core PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

if(FAULTLINE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
    target_compile_options(faultline_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(faultline_core PUBLIC -fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)
target_link_libraries(faultline_core PUBLIC Threads::Threads)

add_executable(faultline src/main.cpp)
target_link_libraries(faultline PRIVATE Faultline::core)
target_compile_options(faultline PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

install(TARGETS faultline RUNTIME DESTINATION bin)

if(FAULTLINE_BUILD_TESTS)
    enable_testing()
    add_executable(faultline_tests tests/tests.cpp)
    target_link_libraries(faultline_tests PRIVATE Faultline::core)
    target_compile_options(faultline_tests PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
    foreach(test_name IN ITEMS
        config
        invalid_config
        staged_config
        round_trip_and_latency
        staged_runtime
        forced_reset
        bandwidth_limit
        large_half_close
        fault_wait
        connection_limit
        connect_shutdown
        control_api
        live_policy
    )
        add_test(NAME ${test_name} COMMAND faultline_tests ${test_name})
    endforeach()
endif()
