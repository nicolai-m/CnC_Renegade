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

find_package(Threads REQUIRED)

add_library(renegade_packet STATIC
    Code/wwbitpack/BitPacker.cpp
    Code/wwbitpack/bitstream.cpp
    Code/wwbitpack/encoderlist.cpp
    Code/wwbitpack/encodertypeentry.cpp
    Code/wwlib/crc.cpp
    Code/wwlib/systimer.cpp
    Code/wwnet/BWBalance.cpp
    Code/wwnet/connect.cpp
    Code/wwnet/msgstat.cpp
    Code/wwnet/msgstatlist.cpp
    Code/wwnet/netstats.cpp
    Code/wwnet/netutil.cpp
    Code/wwnet/packetmgr.cpp
    Code/wwnet/rhost.cpp
    Code/wwnet/singlepl.cpp
    Code/wwnet/wwpacket.cpp
)
target_link_libraries(renegade_packet PUBLIC Threads::Threads)
target_compile_features(renegade_packet PUBLIC cxx_std_17)
target_include_directories(renegade_packet PUBLIC
    Code/WWMath
    Code/wwbitpack
    Code/wwdebug
    Code/wwlib
    Code/wwnet
    Code/wwutil
)

add_library(renegade_level STATIC
    Code/Commando/headless_collision.cpp
    Code/Commando/headless_level.cpp
    Code/Commando/headless_player_session.cpp
    Code/Commando/headless_simulation.cpp
    Code/WWMath/colmathline.cpp
    Code/WWMath/colmathobbobb.cpp
    Code/WWMath/colmathobbtri.cpp
    Code/WWMath/matrix3.cpp
    Code/WWMath/matrix3d.cpp
    Code/WWMath/quat.cpp
    Code/WWMath/tri.cpp
    Code/wwlib/chunkio.cpp
    Code/wwlib/mixarchive.cpp
)
target_link_libraries(renegade_level PUBLIC renegade_packet)
target_include_directories(renegade_level PUBLIC Code/Commando Code/Combat Code/ww3d2 Code/wwsaveload)

add_executable(renegade_fds Code/Commando/linux_fds.cpp)
target_link_libraries(renegade_fds PRIVATE renegade_level)

include(CTest)
if(BUILD_TESTING)
    add_executable(renegade_packet_test Code/Tests/Linux/packet_test.cpp)
    target_link_libraries(renegade_packet_test PRIVATE renegade_packet)
    add_test(NAME renegade_packet_wire_format COMMAND renegade_packet_test)

    add_executable(renegade_udp_test Code/Tests/Linux/udp_transport_test.cpp)
    target_link_libraries(renegade_udp_test PRIVATE renegade_packet)
    add_test(NAME renegade_udp_transport COMMAND renegade_udp_test)

    add_executable(renegade_connection_test Code/Tests/Linux/connection_test.cpp)
    target_link_libraries(renegade_connection_test PRIVATE renegade_packet)
    add_test(NAME renegade_connection_handshake COMMAND renegade_connection_test)

    add_executable(renegade_fds_test Code/Tests/Linux/fds_bootstrap_test.cpp)
    target_link_libraries(renegade_fds_test PRIVATE renegade_level)
    add_test(NAME renegade_fds_bootstrap COMMAND renegade_fds_test $<TARGET_FILE:renegade_fds>)
endif()
