cmake_minimum_required(VERSION 3.10)
project(MiniDB CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# Engine (no networking): hash table + LRU + TTL, WAL, snapshots, command layer
add_library(minidb_core STATIC
  src/event_bus.cpp src/hash_table.cpp src/write_ahead_log.cpp
  src/snapshot_manager.cpp src/storage_engine.cpp src/database.cpp)
target_include_directories(minidb_core PUBLIC include)
target_link_libraries(minidb_core PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(minidb_core PUBLIC /W3)
else()
  target_compile_options(minidb_core PUBLIC -Wall -Wextra)
endif()

# Server: raw TCP protocol + HTTP/SSE for the web UI
add_executable(MiniDB src/main.cpp src/tcp_server.cpp src/http_server.cpp)
target_link_libraries(MiniDB PRIVATE minidb_core)
if(WIN32)
  target_link_libraries(MiniDB PRIVATE ws2_32)
elseif(NOT APPLE)
  target_link_options(MiniDB PRIVATE -static-libstdc++ -static-libgcc)  # slim Docker runtime image
endif()

# Tests
enable_testing()
add_executable(FunctionalTests tests/functional_test.cpp)
target_link_libraries(FunctionalTests PRIVATE minidb_core)
add_executable(StressTest tests/stress_test.cpp)
target_link_libraries(StressTest PRIVATE minidb_core)
add_test(NAME functional COMMAND FunctionalTests)
add_test(NAME stress COMMAND StressTest 8 5000)
