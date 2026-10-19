cmake_minimum_required(VERSION 3.20)
project(rdp-mcp-client C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREERDP REQUIRED IMPORTED_TARGET freerdp3 freerdp-client3 winpr3)
find_package(SDL3 REQUIRED CONFIG)
find_package(Threads REQUIRED)

add_executable(rdp-mcp-client
  src/main.c
  src/control.c
  src/keymap.c
  src/clipboard.c
)
target_compile_options(rdp-mcp-client PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(rdp-mcp-client PRIVATE PkgConfig::FREERDP SDL3::SDL3 Threads::Threads)

install(TARGETS rdp-mcp-client RUNTIME DESTINATION bin)
