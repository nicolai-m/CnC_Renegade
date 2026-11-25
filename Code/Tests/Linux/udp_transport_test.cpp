// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>

#include "crc.h"
#include "netutil.h"
#include "packetmgr.h"
#include "packettype.h"
#include "wwpacket.h"

namespace {

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

struct LoopbackSocket {
    SOCKET Handle = INVALID_SOCKET;
    SOCKADDR_IN Address{};

    LoopbackSocket()
    {
        cNetUtil::Create_Unbound_Socket(Handle);
        Check(Handle != INVALID_SOCKET, "Could not create UDP socket");
        cNetUtil::String_To_Address(&Address, "127.0.0.1", 0);
        Check(bind(Handle, reinterpret_cast<LPSOCKADDR>(&Address), sizeof(Address)) == 0,
            "Could not bind loopback socket");
        SocketLength length = sizeof(Address);
        Check(getsockname(Handle, reinterpret_cast<LPSOCKADDR>(&Address), &length) == 0,
            "Could not read loopback port");
    }

    ~LoopbackSocket()
    {
        cNetUtil::Close_Socket(Handle);
    }

    LoopbackSocket(const LoopbackSocket &) = delete;
    LoopbackSocket &operator=(const LoopbackSocket &) = delete;
};

void Wait_For_Datagram(const LoopbackSocket &socket)
{
    pollfd descriptor{socket.Handle, POLLIN, 0};
    Check(poll(&descriptor, 1, 2000) == 1 && (descriptor.revents & POLLIN), "UDP datagram did not arrive");
}

void Send_Raw(const LoopbackSocket &source, const LoopbackSocket &target, const unsigned char *data, int size)
{
    Check(sendto(source.Handle, data, size, 0, reinterpret_cast<const sockaddr *>(&target.Address),
        sizeof(target.Address)) == size, "Could not send UDP datagram");
    Wait_For_Datagram(target);
}

void Enqueue(PacketManagerClass &manager, const LoopbackSocket &source, LoopbackSocket &target,
    unsigned char *data, int size)
{
    auto *ip = reinterpret_cast<unsigned char *>(&target.Address.sin_addr.s_addr);
    Check(manager.Take_Packet(data, size, ip, target.Address.sin_port, source.Handle),
        "Packet manager did not enqueue game frame");
}

void Expect_Received(PacketManagerClass &manager, const LoopbackSocket &source, const LoopbackSocket &target,
    const unsigned char *expected, int size)
{
    unsigned char received[PACKET_MANAGER_MTU];
    unsigned char ip[4];
    unsigned short port = 0;
    Check(manager.Get_Packet(target.Handle, received, sizeof(received), ip, port) == size,
        "Packet manager reported the wrong game-frame size");
    Check(std::memcmp(received, expected, size) == 0, "Game-frame payload changed during UDP transport");
    Check(port == source.Address.sin_port && std::memcmp(ip, &source.Address.sin_addr.s_addr, sizeof(ip)) == 0,
        "Packet manager did not preserve the sender address");
}

void Test_Wire_Datagram()
{
    LoopbackSocket source;
    LoopbackSocket target;
    PacketManagerClass manager;

    constexpr unsigned char expected[] = {
        0xea, 0xa3, 0xf0, 0x85, // CRC32 in network byte order
        0x21, 0x01,             // one game frame, nine bytes
        0x11, 0x23, 0x45, 0x67, 0xff, 0x00, 0x09, 0xd7, 0x80
    };
    cPacket payload;
    payload.Set_Type(PACKETTYPE_RELIABLE);
    payload.Set_Id(0x01234567);
    payload.Set_Sender_Id(-1);
    payload.Add(true);
    payload.Add(static_cast<BYTE>(0xaf));
    cPacket frame;
    Check(cPacket::Construct_Full_Packet(frame, payload), "Could not construct game frame");
    Enqueue(manager, source, target, reinterpret_cast<unsigned char *>(frame.Get_Data()),
        frame.Get_Compressed_Size_Bytes());
    manager.Flush(true);
    Wait_For_Datagram(target);

    unsigned char actual[PACKET_MANAGER_MTU];
    const int actual_size = recv(target.Handle, actual, sizeof(actual), 0);
    Check(actual_size == sizeof(expected) && std::memcmp(actual, expected, sizeof(expected)) == 0,
        "UDP wrapper differs from the Renegade wire bytes");

    Send_Raw(source, target, expected, sizeof(expected));
    Expect_Received(manager, source, target, expected + 6, sizeof(expected) - 6);

    actual[0] ^= 1;
    Send_Raw(source, target, actual, actual_size);
    unsigned char ip[4];
    unsigned short port = 0;
    Check(manager.Get_Packet(target.Handle, actual, sizeof(actual), ip, port) == 0,
        "UDP wrapper with wrong CRC was accepted");

    // A short datagram must be consumed without blocking the valid one that follows.
    Send_Raw(source, target, expected, 2);
    Check(manager.Get_Packet(target.Handle, actual, sizeof(actual), ip, port) == 0,
        "Short UDP datagram was accepted");
    Send_Raw(source, target, expected, sizeof(expected));
    Expect_Received(manager, source, target, expected + 6, sizeof(expected) - 6);
}

void Test_Coalescing_And_Deltas()
{
    LoopbackSocket source;
    LoopbackSocket target;
    PacketManagerClass manager;
    manager.Set_Is_Server(true);

    unsigned char first[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    unsigned char second[sizeof(first)];
    std::memcpy(second, first, sizeof(first));
    second[1] = 0xf1;
    second[16] = 0xfe;
    unsigned char third[] = {0x80, 0xc0, 0xf0, 0xff};
    Enqueue(manager, source, target, first, sizeof(first));
    Enqueue(manager, source, target, second, sizeof(second));
    Enqueue(manager, source, target, third, sizeof(third));
    manager.Flush(true);
    Wait_For_Datagram(target);

    // Both equal-sized frames are in the first run and a second run follows it.
    // The delta is three bytes of maps plus two patch bytes.
    constexpr unsigned char packed[] = {0x22, 0x82, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x03, 0xea, 0x07, 0xf1, 0xfe,
        0x81, 0x00, 0x80, 0xc0, 0xf0, 0xff};
    unsigned char datagram[PACKET_MANAGER_MTU];
    const int size = recv(target.Handle, datagram, sizeof(datagram), 0);
    Check(size == 4 + sizeof(packed) && std::memcmp(datagram + 4, packed, sizeof(packed)) == 0,
        "Coalesced runs or compressed delta differ from the wire format");

    Send_Raw(source, target, datagram, size);
    Expect_Received(manager, source, target, first, sizeof(first));
    Expect_Received(manager, source, target, second, sizeof(second));
    Expect_Received(manager, source, target, third, sizeof(third));

    // Include the first frame and delta maps, but only one of the two patch bytes.
    // A valid wrapper CRC ensures this exercises the delta reader's length check.
    constexpr int truncated_size = 4 + 2 + sizeof(first) + 3 + 1;
    const uint32 crc = htonl(CRC::Memory(datagram + 4, truncated_size - 4));
    std::memcpy(datagram, &crc, sizeof(crc));
    Send_Raw(source, target, datagram, truncated_size);
    unsigned char ip[4];
    unsigned short port = 0;
    unsigned char received[PACKET_MANAGER_MTU];
    Check(manager.Get_Packet(target.Handle, received, sizeof(received), ip, port) == 0,
        "Truncated delta was accepted");
    Check(manager.Get_Packet(target.Handle, received, sizeof(received), ip, port) == 0,
        "A frame from the rejected datagram remained queued");
}

void Test_Checksums()
{
    constexpr char digits[] = "123456789";
    Check(CRC::Memory(reinterpret_cast<const unsigned char *>(digits), 9) == 0xcbf43926,
        "CRC32 reference vector differs");
    Check(CRC::String(digits) == 0xcbf43926, "String CRC32 reference vector differs");

    constexpr unsigned char legacy[] = {0, 1, 2, 3, 4, 5, 6, 7, 0xff};
    CRCEngine chunks;
    chunks(legacy, sizeof(legacy));
    CRCEngine incremental;
    incremental(legacy, 3);
    incremental(legacy + 3, sizeof(legacy) - 3);
    Check(chunks() == 0x1a140f07 && incremental() == chunks(), "Legacy checksum lost the 32-bit word layout");
}

} // namespace

int main()
{
    Check(cNetUtil::Protocol_Init(false), "Could not initialize IPv4 transport");
    Test_Checksums();
    Test_Wire_Datagram();
    Test_Coalescing_And_Deltas();
}
