    //   0                   1                   2                   3
    //   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    //  +-+-+-+-+-------+-+-------------+-------------------------------+
    //  |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
    //  |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
    //  |N|V|V|V|       |S|             |   (if payload len==126/127)   |
    //  | |1|2|3|       |K|             |                               |
    //  +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
    //  |     Extended payload length continued, if payload len == 127  |
    //  + - - - - - - - - - - - - - - - +-------------------------------+
    //  |                               |Masking-key, if MASK set to 1  |
    //  +-------------------------------+-------------------------------+
    //  | Masking-key (continued)       |          Payload Data         |
    //  +-------------------------------- - - - - - - - - - - - - - - - +
    //  :                     Payload Data continued ...                :
    //  + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
    //  |                     Payload Data continued ...                |
    //  +---------------------------------------------------------------+

#include <vector>
#include <cstring>
#include <string>
#include <cstdint>
#include <evpp/buffer.h>
#include <iomanip>      // For std::setw, std::setfill
#include <stdexcept>    // For std::runtime_error (if throwing exceptions)
#include <optional>     // For std::optional (C++17)
#include "../utils.h"

// WebSocket frame parsing and creation functions
struct WebSocketFrame
{
    bool fin;
    uint8_t opcode;
    bool masked;
    uint64_t payload_length;
    uint8_t mask[4];
    std::vector<uint8_t> payload;


    // Optional: a flag to indicate if the frame is invalid/error
    bool is_valid = true;
};
// Error codes for WebSocket Opcode (for clarity)
enum WebSocketOpcode : uint8_t {
    CONTINUATION = 0x0,
    TEXT_FRAME = 0x1,
    BINARY_FRAME = 0x2,
    CLOSE = 0x8,
    PING = 0x9,
    PONG = 0xA
};


WebSocketFrame parse_frame(evpp::Buffer *data, const std::vector<std::string> &extensions);
std::string create_frame(const std::string &payload, unsigned opcode);