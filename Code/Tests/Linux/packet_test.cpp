// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

void Test_Bit_Fields()
{
    constexpr ULONG value = 0x89abcdefu;
    for (UINT offset = 0; offset < 8; ++offset) {
        for (UINT width = 1; width <= 32; ++width) {
            cBitPacker packer;
            if (offset != 0) {
                packer.Add_Bits(0xff, offset);
            }
            packer.Add_Bits(value, width);

            for (UINT position = 0; position < offset + width; ++position) {
                const auto byte = static_cast<BYTE>(packer.Get_Data()[position / 8]);
                const UINT actual = (byte >> (7 - position % 8)) & 1u;
                const UINT expected = position < offset ? 1u : (value >> (offset + width - position - 1)) & 1u;
                Check(actual == expected, "A bit field differs from the MSB-first wire representation");
            }

            ULONG decoded = 0;
            if (offset != 0) {
                packer.Get_Bits(decoded, offset);
                Check(decoded == (1u << offset) - 1, "Prefix decoded incorrectly");
            }
            packer.Get_Bits(decoded, width);
            Check(decoded == (value & (0xffffffffu >> (32 - width))), "Field decoded incorrectly");
            Check(packer.Is_Flushed(), "Unexpected unread field bits");
        }
    }

    cBitPacker full;
    for (int i = 0; i < MAX_BUFFER_SIZE - 1; ++i) {
        full.Add_Bits(0x5a, 8);
    }
    full.Add_Bits(1, 1);
    full.Add_Bits(0x7f, 7);
    Check(full.Get_Bit_Write_Position() == MAX_BUFFER_SIZE * 8, "Exact-capacity write failed");
    Check(static_cast<BYTE>(full.Get_Data()[MAX_BUFFER_SIZE - 1]) == 0xff, "Last byte was overwritten");
    for (int i = 0; i < MAX_BUFFER_SIZE; ++i) {
        ULONG decoded = 0;
        full.Get_Bits(decoded, 8);
        Check(decoded == (i == MAX_BUFFER_SIZE - 1 ? 0xffu : 0x5au), "Exact-capacity read failed");
    }
}

void Test_Wire_Scalars_And_Strings()
{
    constexpr BYTE expected[] = {
        0xf1, 0xfe, 0xdc, 0x89, 0xab, 0xcd, 0xef, 0xbf, 0xc0, 0x00, 0x00,
        0x00, 0x02, 0x43, 0x43, 0x00, 0x03, 0x00, 0xdf, 0xd8, 0x3d, 0xde, 0x80
    };
    constexpr WCHAR wide_text[] = u"\u00df\U0001F680";

    BitStreamClass stream;
    stream.Add(static_cast<BYTE>(0xf1));
    stream.Add(static_cast<USHORT>(0xfedc));
    stream.Add(0x89abcdefu);
    stream.Add(-1.5f);
    stream.Add_Terminated_String("CC");
    stream.Add_Wide_Terminated_String(wide_text);
    Check(stream.Get_Compressed_Size_Bytes() == sizeof(expected), "Unexpected scalar/string wire size");
    Check(std::memcmp(stream.Get_Data(), expected, sizeof(expected)) == 0, "Scalar/string wire bytes changed");

    BitStreamClass received;
    std::memcpy(received.Get_Data(), expected, sizeof(expected));
    received.Set_Bit_Write_Position(sizeof(expected) * 8);
    BYTE one = 0;
    USHORT two = 0;
    ULONG four = 0;
    float fraction = 0;
    char narrow[3];
    WCHAR wide[4];
    Check(received.Get(one) == 0xf1 && received.Get(two) == 0xfedc, "Narrow scalars decoded incorrectly");
    Check(received.Get(four) == 0x89abcdefu && received.Get(fraction) == -1.5f, "32-bit scalars decoded incorrectly");
    received.Get_Terminated_String(narrow, 3);
    received.Get_Wide_Terminated_String(wide, 4);
    Check(std::strcmp(narrow, "CC") == 0, "Narrow string decoded incorrectly");
    Check(std::memcmp(wide, wide_text, sizeof(wide_text)) == 0, "UTF-16 string decoded incorrectly");
    Check(received.Is_Flushed(), "Unexpected unread scalar/string bits");
}

void Test_Encoder_Range()
{
    cEncoderTypeEntry encoder;
    encoder.Init(0.0, 4294967295.0, 1.0);
    ULONG encoded = 0;
    Check(encoder.Get_Bit_Precision() == 32, "Unexpected precision for 32-bit unsigned range");
    Check(encoder.Scale(4294967295.0, encoded) && encoded == 0xffffffffu, "Encoder lost high unsigned bits");
    Check(encoder.Unscale(encoded) == 4294967295.0, "Encoder incorrectly restored high unsigned bits");
}

void Test_Game_Packet()
{
    constexpr BYTE expected[] = {0x11, 0x23, 0x45, 0x67, 0xff, 0x00, 0x09, 0xd7, 0x80};
    cPacket payload;
    payload.Set_Type(PACKETTYPE_RELIABLE);
    payload.Set_Id(0x01234567);
    payload.Set_Sender_Id(-1);
    payload.Add(true);
    payload.Add(static_cast<BYTE>(0xaf));

    cPacket frame;
    Check(cPacket::Construct_Full_Packet(frame, payload), "Game packet framing failed");
    Check(frame.Get_Compressed_Size_Bytes() == sizeof(expected), "Unexpected game packet wire size");
    Check(std::memcmp(frame.Get_Data(), expected, sizeof(expected)) == 0, "Game packet header or payload differs");

    cPacket received;
    std::memcpy(received.Get_Data(), expected, sizeof(expected));
    received.Set_Bit_Length(sizeof(expected) * 8);
    cPacket decoded;
    Check(cPacket::Construct_App_Packet(decoded, received), "Game packet decoding failed");
    Check(decoded.Get_Type() == PACKETTYPE_RELIABLE, "Incorrect packet type");
    Check(decoded.Get_Id() == 0x01234567, "Incorrect packet ID");
    Check(decoded.Get_Sender_Id() == -1, "Incorrect signed sender ID");
    Check(decoded.Get_Bit_Length() == 9, "Incorrect payload bit length");
    bool flag = false;
    BYTE byte = 0;
    Check(decoded.Get(flag) && decoded.Get(byte) == 0xaf, "Incorrect packet payload");

    cPacket short_header;
    std::memcpy(short_header.Get_Data(), expected, 6);
    short_header.Set_Bit_Length(6 * 8);
    cPacket skipped;
    Check(!cPacket::Construct_App_Packet(skipped, short_header), "Truncated header was accepted");

    cPacket short_payload;
    std::memcpy(short_payload.Get_Data(), expected, sizeof(expected) - 1);
    short_payload.Set_Bit_Length((sizeof(expected) - 1) * 8);
    Check(!cPacket::Construct_App_Packet(skipped, short_payload), "Truncated payload was accepted");

    payload.Set_Bit_Length(MAX_BUFFER_SIZE * 8);
    cPacket too_large;
    Check(!cPacket::Construct_Full_Packet(too_large, payload), "Oversized frame was accepted");
}

} // namespace

int main()
{
    static_assert(sizeof(ULONG) == 4 && sizeof(WCHAR) == 2, "Unexpected Renegade wire type widths");
    Test_Bit_Fields();
    Test_Wire_Scalars_And_Strings();
    Test_Encoder_Range();
    Test_Game_Packet();
}
