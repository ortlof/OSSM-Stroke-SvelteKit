#ifndef ModbusRtu_h
#define ModbusRtu_h

/*
 *   Hand-rolled Modbus RTU framing: function 3 reads, and one narrow write.
 *
 *   This header deliberately pulls in nothing but <stdint.h> and <stddef.h>.
 *   The firmware uses it from ModbusProbeService.cpp and the host test compiles
 *   the very same functions, so there is exactly one implementation of the CRC
 *   and of the frame layout - no copy inside the test that can quietly drift
 *   away from what runs on the device.
 *
 *   The only write that exists here is function 6 (write single register) and
 *   the callers in ModbusProbeService.cpp may only ever point it at the two
 *   torque-limit registers, 0x01FE and 0x01FF, with a value the guard below
 *   accepts. There is no function 5, no function 15 and no generic
 *   write-register API anywhere in this build: the code that can put a frame on
 *   the wire can read any holding register and can write exactly two.
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
// The two read function codes. Only these may ever be emitted: the write path is
// still hard-wired to the torque registers and function 6, and nothing here adds
// a third alternative.
constexpr uint8_t MODBUS_FUNC_READ_HOLDING = 0x03;
constexpr uint8_t MODBUS_FUNC_READ_INPUT = 0x04;

inline uint8_t buildReadRequest(uint8_t *buf, uint8_t unit, uint16_t start, uint16_t count,
                                uint8_t func = MODBUS_FUNC_READ_HOLDING)
{
    buf[0] = unit;
    // Whatever the caller passes, only the two read functions can be emitted.
    buf[1] = (func == MODBUS_FUNC_READ_INPUT) ? MODBUS_FUNC_READ_INPUT : MODBUS_FUNC_READ_HOLDING;
    buf[2] = (uint8_t)(start >> 8);
    buf[3] = (uint8_t)(start & 0x00FF);
    buf[4] = (uint8_t)(count >> 8);
    buf[5] = (uint8_t)(count & 0x00FF);
    uint16_t crc = crc16(buf, 6);
    buf[6] = (uint8_t)(crc & 0x00FF);  // low byte first on the wire
    buf[7] = (uint8_t)(crc >> 8);
    return MODBUS_RTU_REQUEST_LEN;
}

// Outcome of checkReadReply / checkWriteReply.
enum ReadReply
{
    REPLY_OK = 0,
    REPLY_TOO_SHORT,   // fewer bytes than the smallest legal reply
    REPLY_BAD_CRC,     // checksum mismatch
    REPLY_BAD_UNIT,    // answered, but not the unit we asked
    REPLY_BAD_FUNCTION,// function byte is neither the expected one nor an exception
    REPLY_EXCEPTION,   // function byte has 0x80 set; the code is in *exceptionCode
    REPLY_BAD_ECHO     // write reply did not echo the register and value we sent
};

// Validate a reply to a function 3 request. Checks length, CRC and the unit id,
// then the function byte, and decodes an exception reply. The CRC is over every
// byte except the two CRC bytes themselves. `expectedUnit` and the CRC are the
// gates a real device has to pass before anything is believed. The smallest
// legal reply is 5 bytes: unit, function, byte count, crc lo, crc hi.
inline ReadReply checkReadReply(const uint8_t *frame, size_t len, uint8_t expectedUnit, uint8_t *exceptionCode,
                                uint8_t expectedFunc = MODBUS_FUNC_READ_HOLDING)
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

    if (frame[1] != expectedFunc)
    {
        return REPLY_BAD_FUNCTION;
    }

    return REPLY_OK;
}

// ---------------------------------------------------------------------------
// Read request validation. A range read is a diagnostic: it may ask for any
// block of holding registers the drive exposes, but only one 32-register window
// that stays inside the map this project considers safe to look at.
// ---------------------------------------------------------------------------

#define MODBUS_READ_REG_MAX 0x1FFF
#define MODBUS_READ_COUNT_MAX 32
// Longest function-3 reply: unit + function + byte count + 2*count data + crc.
#define MODBUS_RTU_READ_REPLY_MAX (3 + 2 * MODBUS_READ_COUNT_MAX + 2)

// True when [start, start+count-1] is a legal read window. Both ends are checked
// so a request can never walk off the top of the allowed register map.
inline bool readFuncValid(int32_t func)
{
    return func == MODBUS_FUNC_READ_HOLDING || func == MODBUS_FUNC_READ_INPUT;
}

inline bool readRangeValid(int32_t start, int32_t count)
{
    return start >= 0 && start <= MODBUS_READ_REG_MAX &&
           count >= 1 && count <= MODBUS_READ_COUNT_MAX &&
           (start + count - 1) <= MODBUS_READ_REG_MAX;
}

// ---------------------------------------------------------------------------
// Write path: function 6 (write single register) and nothing else.
//
// The iHSV57 torque limit is a mechanical safety limit. It lives in 0x01FE
// (P05-10, forward) and 0x01FF (P05-11, reverse) with a scale of 0.1 %, so
// 2000 is 200.0 %. The drive stores the two directions with opposite signs:
// the vendor parameter table declares P05-10 as 0 .. +300.0 % and P05-11 as
// -300.0 % .. 0, and the drive at its factory default reads +2000 on 0x01FE
// and -2000 (0xF830) on 0x01FF. Read back and checked on the machine.
//
// Everything above this link therefore speaks a MAGNITUDE in 0.1 % of rated,
// and only the wire value carries the sign (torqueLimitWireValue below).
//
// This window is the whole point of the guard: eat a step at 300 % and the rack
// takes the load. torqueLimitValid is deliberately a validator, not a clamper -
// an out-of-range request must be refused, never quietly moved to the boundary,
// because the operator has to know their number was not the one that got used.
//
// 0 is a legal limit and means "no torque in this direction". On a vertical
// axis that is the direction gravity pulls in, so the operator can turn the
// carriage into a falling load. The UI warns; the drive obeys.
// ---------------------------------------------------------------------------

#define MODBUS_TORQUE_REG_FORWARD 0x01FE
#define MODBUS_TORQUE_REG_REVERSE 0x01FF
#define MODBUS_TORQUE_MIN 0     // no torque in this direction
#define MODBUS_TORQUE_MAX 3000  // 300.0 % of rated

inline bool torqueLimitValid(int32_t value)
{
    return value >= MODBUS_TORQUE_MIN && value <= MODBUS_TORQUE_MAX;
}

// The wire value for a torque limit: the magnitude for the forward register,
// the two's complement of the magnitude for the reverse register. The drive
// stores the reverse limit as a negative number, so writing 2000 to 0x01FF
// would hand it a value outside its declared range.
inline uint16_t torqueLimitWireValue(bool forward, int32_t magnitude)
{
    int32_t raw = forward ? magnitude : -magnitude;
    return (uint16_t)(int16_t)raw;
}

// Signed interpretation of a torque-limit read-back, returned as a magnitude.
// The reverse register answers 0xF830 for -200.0 %, and the caller wants 2000.
inline int32_t torqueLimitMagnitude(uint16_t wire)
{
    int16_t signedRaw = (int16_t)wire;
    return signedRaw < 0 ? -(int32_t)signedRaw : (int32_t)signedRaw;
}

// A function 6 request is exactly 8 bytes: unit, function, reg hi, reg lo,
// value hi, value lo, crc lo, crc hi. The reply the drive sends echoes the same
// eight bytes (or a five-byte exception).
#define MODBUS_RTU_WRITE_REQUEST_LEN 8

inline uint8_t buildWriteRequest(uint8_t *buf, uint8_t unit, uint16_t reg, uint16_t value)
{
    buf[0] = unit;
    buf[1] = 0x06;  // write single register
    buf[2] = (uint8_t)(reg >> 8);
    buf[3] = (uint8_t)(reg & 0x00FF);
    buf[4] = (uint8_t)(value >> 8);
    buf[5] = (uint8_t)(value & 0x00FF);
    uint16_t crc = crc16(buf, 6);
    buf[6] = (uint8_t)(crc & 0x00FF);  // low byte first on the wire
    buf[7] = (uint8_t)(crc >> 8);
    return MODBUS_RTU_WRITE_REQUEST_LEN;
}

// Validate the echo to a function 6 write. Same gates as a read reply (length,
// CRC, unit id, exception decode) plus the two that matter for a write: the
// function byte must be 6, and the register and value must be the ones we sent.
// A drive that answers something else must not be believed.
inline ReadReply checkWriteReply(const uint8_t *frame, size_t len, uint8_t expectedUnit,
                                 uint16_t expectedReg, uint16_t expectedValue, uint8_t *exceptionCode)
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

    if (frame[1] != 0x06)
    {
        return REPLY_BAD_FUNCTION;
    }

    if (len != MODBUS_RTU_WRITE_REQUEST_LEN)
    {
        return REPLY_TOO_SHORT;  // a normal function 6 reply is exactly eight bytes
    }

    uint16_t reg = ((uint16_t)frame[2] << 8) | (uint16_t)frame[3];
    uint16_t value = ((uint16_t)frame[4] << 8) | (uint16_t)frame[5];
    if (reg != expectedReg || value != expectedValue)
    {
        return REPLY_BAD_ECHO;
    }

    return REPLY_OK;
}

// Human text for a Modbus exception code, for the probe log. These are the codes
// from the exception-response section of the specification, not function codes:
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
