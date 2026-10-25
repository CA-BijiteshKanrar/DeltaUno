cmake_minimum_required(VERSION 3.20)
project(DeltaUno VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /O2)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -O3)
endif()

include_directories(include)

# Main DeltaUno Executable
add_executable(deltauno
    src/main.cpp
)

# Automated Unit Tests
enable_testing()

add_executable(test_order_book tests/test_order_book.cpp)
add_test(NAME OrderBookTest COMMAND test_order_book)

add_executable(test_risk_manager tests/test_risk_manager.cpp)
add_test(NAME RiskManagerTest COMMAND test_risk_manager)

add_executable(test_state_journal tests/test_state_journal.cpp)
add_test(NAME StateJournalTest COMMAND test_state_journal)

add_executable(test_paper_broker tests/test_paper_broker.cpp)
add_test(NAME PaperBrokerTest COMMAND test_paper_broker)
