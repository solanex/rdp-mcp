cmake_minimum_required(VERSION 3.20)
project(rdp-mcp-client C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREERDP REQUIRED IMPORTED_TARGET freerdp3 freerdp-client3 winpr3)
find_package(SDL3 REQUIRED CONFIG)
find_package(Threads REQUIRED)

# Gedeelde kern (ook gebruikt door de Mac-app)
add_library(rdpmcp-core STATIC
  ../core/src/session.c
  ../core/src/control.c
  ../core/src/clipboard.c
)
target_include_directories(rdpmcp-core PUBLIC ../core/include)
target_compile_options(rdpmcp-core PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(rdpmcp-core PUBLIC PkgConfig::FREERDP Threads::Threads)

add_executable(rdp-mcp-client
  src/main.c
  src/keymap.c
)
target_compile_options(rdp-mcp-client PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(rdp-mcp-client PRIVATE rdpmcp-core SDL3::SDL3)

install(TARGETS rdp-mcp-client RUNTIME DESTINATION bin)
