cmake_minimum_required(VERSION 3.20)
project(DeltaUno VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /O2)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -O3)
endif()

find_package(Threads REQUIRED)

# Header-only low-latency C++20 engine target
add_library(deltauno_engine INTERFACE)
target_include_directories(deltauno_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(deltauno_engine INTERFACE Threads::Threads)

# Main DeltaUno Executable
add_executable(deltauno src/main.cpp)
target_link_libraries(deltauno PRIVATE deltauno_engine)

# Automated Unit Test Suite
enable_testing()

function(add_deltauno_test target_name source_file test_name)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE deltauno_engine)
    add_test(NAME ${test_name} COMMAND ${target_name})
endfunction()

add_deltauno_test(test_order_book         tests/test_order_book.cpp         OrderBookTest)
add_deltauno_test(test_signal_ingestor    tests/test_signal_ingestor.cpp    SignalIngestorTest)
add_deltauno_test(test_risk_manager       tests/test_risk_manager.cpp       RiskManagerTest)
add_deltauno_test(test_state_journal      tests/test_state_journal.cpp      StateJournalTest)
add_deltauno_test(test_paper_broker       tests/test_paper_broker.cpp       PaperBrokerTest)
add_deltauno_test(test_trade_auditor      tests/test_trade_auditor.cpp      TradeAuditorTest)
add_deltauno_test(test_momentum_strategy  tests/test_momentum_strategy.cpp  MomentumStrategyTest)
add_deltauno_test(test_adversarial_m3     tests/test_adversarial_m3_drawdown_progression.cpp AdversarialM3DrawdownProgressionTest)

# Milestone 4: Dual Track E2E Testing Suite (Tiers 1-4)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(
    NAME DeltaUno_E2E_Master_Suite
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/run_e2e_tests.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

