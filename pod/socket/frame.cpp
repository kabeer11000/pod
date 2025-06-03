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

// FIN:  1 bit

//       Indicates that this is the final fragment in a message.  The first
//       fragment MAY also be the final fragment.

// RSV1, RSV2, RSV3:  1 bit each

//       MUST be 0 unless an extension is negotiated that defines meanings
//       for non-zero values.  If a nonzero value is received and none of
//       the negotiated extensions defines the meaning of such a nonzero
//       value, the receiving endpoint MUST _Fail the WebSocket
//       Connection_.

#include <vector>
#include <cstring>
#include <string>
#include <cstdint>
#include <evpp/buffer.h>
#include <iomanip>      // For std::setw, std::setfill
#include "./frame.h"
#include <stdexcept>    // For std::runtime_error (if throwing exceptions)
#include <optional>     // For std::optional (C++17)
#include "../utils.h"

WebSocketFrame parse_frame(evpp::Buffer *data, const std::vector<std::string> &extentions) {
    WebSocketFrame frame;
    uint8_t byte0 = data->ReadByte();
     // Ensure at least 2 bytes are available for the basic header
    if (data->length() < 2) {
        // Not enough data for even the basic header
        throw ("Error: Not enough data for WebSocket header.");
    }
    frame.fin = (byte0 & 0b10000000);
      // Check RSV bits
    bool rsv1_set = (byte0 & 0x40) != 0; // RSV1 (0100 0000)
    bool rsv2_set = (byte0 & 0x20) != 0; // RSV2 (0010 0000)
    bool rsv3_set = (byte0 & 0x10) != 0; // RSV3 (0001 0000)
    print_vector(std::cout, extentions);

    // if (extentions.size() == 0 && (rsv1_set || rsv2_set || rsv3_set)) {
    //     std::cerr << "Error: RSV bits are set in WebSocket frame. RSV1=" << rsv1_set
    //               << ", RSV2=" << rsv2_set << ", RSV3=" << rsv3_set
    //               << ". This indicates a protocol error (close connection)." << std::endl;
    //     throw ("Either one of RSV1, RSV2 or RSV3 turned to 1, This may indicate an error");
    // }

    frame.opcode = (byte0 & 0b00001111);
    

    // --- Byte 1: Masked bit, Payload Length (initial) ---
    uint8_t byte1 = data->ReadByte();

    // Print raw byte 1 in hex
    std::cout << "Raw Header Byte 1: 0x" << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<unsigned int>(byte1) << std::endl;
    std::cout << std::dec; // Reset to decimal for general output

    frame.masked = (byte1 & 0x80) != 0; // Mask bit (1000 0000)
    uint8_t length_code = byte1 & 0x7F; // Payload Length (0111 1111)

    // --- Print readable header info (partial at this point) ---
    std::cout << "--- Parsed Frame Header (Initial) ---" << std::endl;
    std::cout << "  FIN: " << (frame.fin ? "true" : "false") << std::endl;
    std::cout << "  Opcode: 0x" << std::hex << static_cast<unsigned int>(frame.opcode) << " (";
    switch(frame.opcode) {
        case WebSocketOpcode::CONTINUATION: std::cout << "CONTINUATION"; break;
        case WebSocketOpcode::TEXT_FRAME: std::cout << "TEXT"; break;
        case WebSocketOpcode::BINARY_FRAME: std::cout << "BINARY"; break;
        case WebSocketOpcode::CLOSE: std::cout << "CLOSE"; break;
        case WebSocketOpcode::PING: std::cout << "PING"; break;
        case WebSocketOpcode::PONG: std::cout << "PONG"; break;
        default: std::cout << "UNKNOWN/RESERVED"; break;
    }
    std::cout << ")" << std::endl;
    std::cout << "  Masked: " << (frame.masked ? "true" : "false") << std::endl;
    std::cout << "  Payload Length Code: " << std::dec << static_cast<unsigned int>(length_code) << std::endl;
    

    return frame;

}
std::string create_frame(const std::string &payload, unsigned opcode) {
    return (std::string) "kabeer";
};