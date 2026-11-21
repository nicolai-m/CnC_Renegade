cmake_minimum_required(VERSION 3.20)
project(RenegadeLinuxPort LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This port build targets Linux. See README.md for the Win32 workspace.")
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|aarch64|arm64)$" OR
   NOT CMAKE_CXX_BYTE_ORDER STREQUAL "LITTLE_ENDIAN" OR
   NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The port build targets 64-bit little-endian Linux (amd64 or arm64).")
endif()

add_library(renegade_packet STATIC
    Code/wwbitpack/BitPacker.cpp
    Code/wwbitpack/bitstream.cpp
    Code/wwbitpack/encoderlist.cpp
    Code/wwbitpack/encodertypeentry.cpp
    Code/wwlib/systimer.cpp
    Code/wwnet/wwpacket.cpp
)
target_compile_features(renegade_packet PUBLIC cxx_std_17)
target_include_directories(renegade_packet PUBLIC
    Code/WWMath
    Code/wwbitpack
    Code/wwdebug
    Code/wwlib
    Code/wwnet
    Code/wwutil
)

include(CTest)
if(BUILD_TESTING)
    add_executable(renegade_packet_test Code/Tests/Linux/packet_test.cpp)
    target_link_libraries(renegade_packet_test PRIVATE renegade_packet)
    add_test(NAME renegade_packet_wire_format COMMAND renegade_packet_test)
endif()
