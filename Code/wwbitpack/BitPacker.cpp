/*
**	Command & Conquer Renegade(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

//
// Filename:     bitpacker.cpp
// Project:      wwbitpack.lib
// Author:       Tom Spencer-Smith
// Date:         June 1998
// Description:  Minimal bit encoding
//

#include "BitPacker.h"

#include <string.h>	// for memset

#include "wwdebug.h"

//-----------------------------------------------------------------------------
//cBitPacker::cBitPacker(UINT buffer_size) :
cBitPacker::cBitPacker() :
	//BufferSize(buffer_size),
	BitWritePosition(0),
	BitReadPosition(0)
{
	//WWASSERT(BufferSize > 0);

	//Buffer = new BYTE[BufferSize];
	//WWASSERT(Buffer != NULL);
	//memset(Buffer, 0, BufferSize);
	memset(Buffer, 0, MAX_BUFFER_SIZE);
}

//-----------------------------------------------------------------------------
cBitPacker::~cBitPacker() 
{
	//delete [] Buffer;
}

//-----------------------------------------------------------------------------
cBitPacker& cBitPacker::operator=(const cBitPacker& rhs)
{
	//WWASSERT(BufferSize == rhs.BufferSize);

	//memcpy(Buffer, rhs.Buffer, rhs.BufferSize);
	memcpy(Buffer, rhs.Buffer, MAX_BUFFER_SIZE);
	BitReadPosition		= rhs.BitReadPosition;
	BitWritePosition		= rhs.BitWritePosition;

   return * this;
}

//-----------------------------------------------------------------------------
// Renegade writes the selected bits most-significant bit first.
void cBitPacker::Add_Bits(ULONG value, UINT num_bits)
{
	//
	// N.B. Presently you cannot use this class with an atomic type of more 
	// than 4 bytes, such as a double. Hopefully you would be using a float 
	// instead anyway.
	//
	WWASSERT(num_bits > 0 && num_bits <= MAX_BITS);
	WWASSERT(num_bits <= MAX_BUFFER_SIZE * 8 - BitWritePosition);
	if (num_bits == 0 || num_bits > MAX_BITS ||
		num_bits > MAX_BUFFER_SIZE * 8 - BitWritePosition) {
		return;
	}

	while (num_bits != 0) {
		const UINT byte_index = BitWritePosition / 8;
		const UINT bit_offset = BitWritePosition % 8;
		const UINT count = MIN(num_bits, 8 - bit_offset);
		const UINT shift = 8 - bit_offset - count;
		const UINT mask = (1u << count) - 1u;
		const UINT prefix_mask = 0xffu << (8 - bit_offset);

		num_bits -= count;
		const UINT bits = (value >> num_bits) & mask;
		Buffer[byte_index] = static_cast<BYTE>(
			(Buffer[byte_index] & prefix_mask) | (bits << shift));
		BitWritePosition += count;
	}
}

//-----------------------------------------------------------------------------
void cBitPacker::Get_Bits(ULONG & value, UINT num_bits)
{
	WWASSERT(num_bits > 0 && num_bits <= MAX_BITS);
	WWASSERT(num_bits <= BitWritePosition - BitReadPosition);
	value = 0;
	if (num_bits == 0 || num_bits > MAX_BITS ||
		num_bits > BitWritePosition - BitReadPosition) {
		return;
	}

	while (num_bits != 0) {
		const UINT byte_index = BitReadPosition / 8;
		const UINT bit_offset = BitReadPosition % 8;
		const UINT count = MIN(num_bits, 8 - bit_offset);
		const UINT shift = 8 - bit_offset - count;
		const UINT mask = (1u << count) - 1u;

		value = (value << count) | ((Buffer[byte_index] >> shift) & mask);
		BitReadPosition += count;
		num_bits -= count;
	}
}

//-----------------------------------------------------------------------------
//
// This method is only for use by a packet class when data is received.
//

void cBitPacker::Set_Bit_Write_Position(UINT position)
{
	//WWASSERT(position <= BufferSize * 8);
	WWASSERT(position <= MAX_BUFFER_SIZE * 8);
	BitWritePosition = MIN(position, static_cast<UINT>(MAX_BUFFER_SIZE * 8));
	BitReadPosition = MIN(BitReadPosition, BitWritePosition);
}










/*
//-----------------------------------------------------------------------------
void cBitPacker::Increment_Bit_Position(int num_bits)
{
	WWASSERT(num_bits >= 0);

	for (int i = 0; i < num_bits; i++) {
		Advance_Bit_Position();
		NumBits++;
	}
}

//-----------------------------------------------------------------------------
UINT cBitPacker::Get_Compressed_Size_Bytes() const 
{
	return (int) ceil(BitWritePosition / 8.0f);
}

//-----------------------------------------------------------------------------
inline void cBitPacker::Advance_Bit_Position()
{
	BitWritePosition++;
	
	//
	// If the following assert hits then our buffer is not large enough.
	// We can advance BitWritePosition one bit past the end of the buffer, but
	// we cannot write there.
	//
	//WWASSERT(BitWritePosition < BufferSize * 8);
	WWASSERT(BitWritePosition <= BufferSize * 8);
}

*/


