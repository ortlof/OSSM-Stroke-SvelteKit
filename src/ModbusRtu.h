#ifndef ModbusRtu_h
#define ModbusRtu_h

/*
 *   Hand-rolled Modbus RTU framing, read side only.
 *
 *   This header deliberately pulls in nothing but <stdint.h> and <stddef.h>.
 *   The firmware uses it from ModbusProbeService.cpp and the host test compiles
 *   the very same functions, so there is exactly one implementation of the CRC
 *   and of the frame layout - no copy inside the test that can quietly drift
 *   away from what runs on the device.
 *
 *   Only function 3 (read holding registers) exists here. There is no write
 *   path on purpose: this build must not be able to change anything on the
 *   drive, so the code that can put a frame on the wire can only ever ask.
 *
 *   The Modbus 3.5 character silent interval is not modelled: the caller flushes
 *   the receive buffer before a request and keeps a single request in flight at
 *   a time, which is enough for a point-to-point link.
 */

#include <stdint.h>
#include <stddef.h>

namespace modbus_rtu
{

// CRC16 as Modbus RTU sends it: polynomial 0xA001 (the reflected form of the
// polynomial the standard writes as 0x8005), initial value 0xFFFF, no final xor.
// The caller appends the result low byte first.
inline uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i];
        for (uint8_t bit = 0; bit < 8; bit++)
        {
            if (crc & 0x0001)
            {
                crc = (crc >> 1) ^ 0xA001;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

// A function 3 request is exactly 8 bytes: unit, function, start hi, start lo,
// count hi, count lo, crc lo, crc hi. The reply buffer is the caller's business.
#define MODBUS_RTU_REQUEST_LEN 8

// Build a read-holding-registers (function 3) request. Returns the frame length
// in bytes (always MODBUS_RTU_REQUEST_LEN).
inline uint8_t buildReadRequest(uint8_t *buf, uint8_t unit, uint16_t start, uint16_t count)
{
    buf[0] = unit;
    buf[1] = 0x03;  // read holding registers
    buf[2] = (uint8_t)(start >> 8);
    buf[3] = (uint8_t)(start & 0x00FF);
    buf[4] = (uint8_t)(count >> 8);
    buf[5] = (uint8_t)(count & 0x00FF);
    uint16_t crc = crc16(buf, 6);
    buf[6] = (uint8_t)(crc & 0x00FF);  // low byte first on the wire
    buf[7] = (uint8_t)(crc >> 8);
    return MODBUS_RTU_REQUEST_LEN;
}

// Outcome of checkReadReply.
enum ReadReply
{
    REPLY_OK = 0,
    REPLY_TOO_SHORT,   // fewer bytes than the smallest legal reply
    REPLY_BAD_CRC,     // checksum mismatch
    REPLY_BAD_UNIT,    // answered, but not the unit we asked
    REPLY_BAD_FUNCTION,// function byte is neither the expected one nor an exception
    REPLY_EXCEPTION    // function byte has 0x80 set; the code is in *exceptionCode
};

// Validate a reply to a function 3 request. Checks length, CRC and the unit id,
// then the function byte, and decodes an exception reply. The CRC is over every
// byte except the two CRC bytes themselves. `expectedUnit` and the CRC are the
// gates a real device has to pass before anything is believed. The smallest
// legal reply is 5 bytes: unit, function, byte count, crc lo, crc hi.
inline ReadReply checkReadReply(const uint8_t *frame, size_t len, uint8_t expectedUnit, uint8_t *exceptionCode)
{
    if (len < 5)
    {
        return REPLY_TOO_SHORT;
    }

    uint16_t crc = crc16(frame, len - 2);
    uint16_t wire = (uint16_t)frame[len - 2] | ((uint16_t)frame[len - 1] << 8);
    if (crc != wire)
    {
        return REPLY_BAD_CRC;
    }

    if (frame[0] != expectedUnit)
    {
        return REPLY_BAD_UNIT;
    }

    if (frame[1] & 0x80)
    {
        if (exceptionCode != nullptr)
        {
            *exceptionCode = frame[2];
        }
        return REPLY_EXCEPTION;
    }

    if (frame[1] != 0x03)
    {
        return REPLY_BAD_FUNCTION;
    }

    return REPLY_OK;
}

// Human text for a Modbus exception code, for the probe log. These are the codes
// from the exception-response section of the specification, not function codes:
// there is no function code here that changes anything.
inline const char *exceptionText(uint8_t code)
{
    switch (code)
    {
    case 0x01:
        return "illegal function";
    case 0x02:
        return "illegal address";
    case 0x03:
        return "illegal value";
    case 0x04:
        return "slave failure";
    case 0x08:
        return "memory parity";
    case 0x0A:
        return "gateway path";
    case 0x0B:
        return "gateway no reply";
    default:
        return "exception";
    }
}

}  // namespace modbus_rtu

#endif  // ModbusRtu_h
