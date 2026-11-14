
cmake_minimum_required(VERSION 3.16)

project(ExchangeHub LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(exchangehub_server src/server.cpp)
add_executable(exchangehub_client src/client.cpp)

target_include_directories(exchangehub_server PRIVATE include)
target_include_directories(exchangehub_client PRIVATE include)

target_link_libraries(exchangehub_server PRIVATE OpenSSL::Crypto Threads::Threads)
target_link_libraries(exchangehub_client PRIVATE OpenSSL::Crypto Threads::Threads)
