cmake_minimum_required(VERSION 3.25)
project(faultline VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FAULTLINE_BUILD_TESTS "Build Faultline tests" ON)
option(FAULTLINE_BUILD_BENCHMARKS "Build Faultline benchmarks" OFF)
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

if(FAULTLINE_BUILD_BENCHMARKS)
    add_executable(faultline_benchmarks benchmarks/benchmarks.cpp)
    target_link_libraries(faultline_benchmarks PRIVATE Faultline::core)
    target_compile_definitions(faultline_benchmarks PRIVATE
        FAULTLINE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
        FAULTLINE_SYSTEM_PROCESSOR="${CMAKE_SYSTEM_PROCESSOR}"
        FAULTLINE_VERSION="${PROJECT_VERSION}"
    )
    target_compile_options(faultline_benchmarks PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
endif()

find_package(Threads REQUIRED)
target_link_libraries(faultline_core PUBLIC Threads::Threads)

add_executable(faultline src/main.cpp)
target_link_libraries(faultline PRIVATE Faultline::core)
target_compile_definitions(faultline PRIVATE FAULTLINE_VERSION="${PROJECT_VERSION}")
target_compile_options(faultline PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

install(TARGETS faultline RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY examples ui DESTINATION ${CMAKE_INSTALL_DATADIR}/faultline)
install(DIRECTORY assets DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

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
        latency_accuracy
        staged_runtime
        forced_reset
        initial_stage_reset
        bandwidth_limit
        large_half_close
        fault_wait
        connection_limit
        connect_shutdown
        control_api
        event_history
        live_policy
    )
        add_test(NAME ${test_name} COMMAND faultline_tests ${test_name})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 15)
    endforeach()
    add_test(NAME cli_help COMMAND faultline --help)
    add_test(NAME cli_version COMMAND faultline --version)
    set_tests_properties(cli_help PROPERTIES PASS_REGULAR_EXPRESSION "Usage:")
    set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "faultline ${PROJECT_VERSION}")
endif()

set(CPACK_PACKAGE_NAME "faultline")
set(CPACK_PACKAGE_VENDOR "criizx")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Deterministic TCP network chaos proxy")
set(CPACK_PACKAGE_FILE_NAME "faultline-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ")
set(CPACK_PACKAGE_CHECKSUM "SHA256")
include(CPack)
