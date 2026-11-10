cmake_minimum_required(VERSION 3.20)
project(cppquant_backtest VERSION 0.1.0 LANGUAGES CXX)

option(CPPQUANT_BUILD_TESTS "Build the unit tests" ON)
option(CPPQUANT_BUILD_BENCHMARKS "Build the benchmark" ON)
option(CPPQUANT_BUILD_EXAMPLES "Build the examples" ON)
option(CPPQUANT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(CPPQUANT_TSAN "Build with ThreadSanitizer" OFF)
option(CPPQUANT_WERROR "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(cppquant_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(CPPQUANT_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        if(CPPQUANT_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

if(CPPQUANT_SANITIZE AND CPPQUANT_TSAN)
    message(FATAL_ERROR "CPPQUANT_SANITIZE and CPPQUANT_TSAN are mutually exclusive")
endif()

if(CPPQUANT_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "CPPQUANT_SANITIZE requires Clang or GCC")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(CPPQUANT_TSAN)
    if(MSVC)
        message(FATAL_ERROR "CPPQUANT_TSAN requires Clang or GCC")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

add_library(cppquant_backtest
    src/backtest.cpp
    src/csv.cpp
    src/panel.cpp
    src/sweep.cpp
    src/walk_forward.cpp
)
add_library(cppquant::backtest ALIAS cppquant_backtest)
target_include_directories(cppquant_backtest PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(cppquant_backtest PUBLIC cxx_std_20)
target_link_libraries(cppquant_backtest PUBLIC Threads::Threads)
set_target_properties(cppquant_backtest PROPERTIES EXPORT_NAME backtest)
cppquant_set_warnings(cppquant_backtest)

if(CPPQUANT_BUILD_EXAMPLES)
    add_executable(backtest_cli
        examples/backtest_cli.cpp
    )
    target_link_libraries(backtest_cli PRIVATE cppquant::backtest)
    cppquant_set_warnings(backtest_cli)

    add_executable(panel_backtest
        examples/panel_backtest.cpp
    )
    target_link_libraries(panel_backtest PRIVATE cppquant::backtest)
    cppquant_set_warnings(panel_backtest)

    add_executable(walk_forward_cli
        examples/walk_forward_cli.cpp
    )
    target_link_libraries(walk_forward_cli PRIVATE cppquant::backtest)
    cppquant_set_warnings(walk_forward_cli)
endif()

if(CPPQUANT_BUILD_BENCHMARKS)
    add_executable(backtest_bench
        bench/bench_backtest.cpp
    )
    target_link_libraries(backtest_bench PRIVATE cppquant::backtest)
    cppquant_set_warnings(backtest_bench)
endif()

if(CPPQUANT_BUILD_TESTS)
    enable_testing()

    add_executable(backtest_tests
        tests/main.cpp
        tests/test_backtest.cpp
        tests/test_panel.cpp
        tests/test_corporate_actions.cpp
        tests/test_sweep.cpp
        tests/test_walk_forward.cpp
    )
    target_link_libraries(backtest_tests PRIVATE cppquant::backtest)
    target_include_directories(backtest_tests PRIVATE tests)
    cppquant_set_warnings(backtest_tests)

    add_test(NAME backtest_tests COMMAND backtest_tests)
    set_tests_properties(backtest_tests PROPERTIES TIMEOUT 120)

    if(TARGET backtest_cli)
        add_test(NAME backtest_cli_sample
            COMMAND backtest_cli --csv ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_bars.csv
                    --fast 5 --slow 20 --quantity 100
        )
        set_tests_properties(backtest_cli_sample PROPERTIES TIMEOUT 60)

        add_test(NAME backtest_cli_actions
            COMMAND backtest_cli --csv ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_bars.csv
                    --strategy buyhold --quantity 100
                    --actions ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_corporate_actions.csv
                    --margin-rate 0.02 --borrow-rate 0.01
        )
        set_tests_properties(backtest_cli_actions PROPERTIES TIMEOUT 60)

        add_test(NAME panel_backtest_sample
            COMMAND panel_backtest ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_multi.csv 10
        )
        set_tests_properties(panel_backtest_sample PROPERTIES TIMEOUT 60)

        add_test(NAME walk_forward_cli_sample
            COMMAND walk_forward_cli --csv ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_bars.csv
                    --train 100 --test 50 --fast 5,10 --slow 20,30
                    --export ${CMAKE_CURRENT_BINARY_DIR}/walk_forward_sample.csv
        )
        set_tests_properties(walk_forward_cli_sample PROPERTIES TIMEOUT 120)
    endif()

    if(TARGET backtest_bench)
        add_test(NAME backtest_bench_smoke COMMAND backtest_bench --quick)
        set_tests_properties(backtest_bench_smoke PROPERTIES TIMEOUT 120)
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS cppquant_backtest
    EXPORT cppquantBacktestTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT cppquantBacktestTargets
    FILE cppquantBacktestTargets.cmake
    NAMESPACE cppquant::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppquant_backtest
)

configure_package_config_file(
    cmake/cppquant_backtestConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cppquant_backtestConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppquant_backtest
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cppquant_backtestConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cppquant_backtestConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cppquant_backtestConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppquant_backtest
)
