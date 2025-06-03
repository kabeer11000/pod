#include <evpp/tcp_server.h>
#include <evpp/buffer.h>
#include <evpp/tcp_conn.h>
#include <evpp/timestamp.h>
#include <nlohmann/json.hpp>
#include "./uuid.h"
#include "./constants.h"
#include <string>
#include <sstream>
#include "./base64.h"
#include <vector>
#include <openssl/sha.h>

using json = nlohmann::json;

// WebSocket GUID constant as per RFC 6455
const std::string WEBSOCKET_MAGIC_STRING = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string sha1_hash(const std::string &input)
{
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char *>(input.c_str()), input.length(), hash);
    return std::string(reinterpret_cast<char *>(hash), SHA_DIGEST_LENGTH);
}
struct WebSocketConnection
{
    evpp::TCPConnPtr connection;
    std::string headers;
    // Equality operator - compares the underlying connection pointers
    bool operator==(const WebSocketConnection &other) const
    {
        return connection.get() == other.connection.get();
    }

    // Optional: inequality operator for completeness
    bool operator!=(const WebSocketConnection &other) const
    {
        return !(*this == other);
    }
};
std::vector<WebSocketConnection> connections;
void establish_websocket(const evpp::TCPConnPtr &conn, evpp::Buffer *msg)
{
    std::string headers = msg->ToString();
    std::istringstream headers_stream(headers);
    std::string header_line;
    std::string sec_websocket_key;
    std::cout << "New Connection, Recieved: " << msg->ToString() << '\n';

    // Parse headers to find Sec-WebSocket-Key
    while (std::getline(headers_stream, header_line))
    {
        // Remove carriage return if present
        if (!header_line.empty() && header_line.back() == '\r')
        {
            header_line.pop_back();
        }

        if (header_line.find("Sec-WebSocket-Key: ") == 0)
        {
            sec_websocket_key = header_line.substr(19); // Length of "Sec-WebSocket-Key: "
            break;
        }
    }

    if (sec_websocket_key.empty())
    {
        std::cout << "No Sec-WebSocket-Key found in request" << std::endl;
        conn->Close();
        return;
    }

    // Generate Sec-WebSocket-Accept according to RFC 6455
    std::string combined = sec_websocket_key + WEBSOCKET_MAGIC_STRING;
    std::string sha1_result = sha1_hash(combined);
    std::string sec_websocket_accept = base64_encode(sha1_result, false);

    // Build HTTP response with proper CRLF line endings
    std::string response_str =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        sec_websocket_accept + "\r\n"
                               "\r\n"; // Empty line to end headers

    std::cout << "Sending WebSocket handshake response and establishing connection. " << std::endl;
    conn->Send(response_str);
    connections.push_back((WebSocketConnection){conn, headers : response_str});
}
struct WebSocketFrame
{
    bool fin;
    uint8_t opcode;
    bool masked;
    uint64_t payload_length;
    uint8_t mask[4];
    std::vector<uint8_t> payload;
};
void on_socket_message(const WebSocketConnection &connection, evpp::Buffer *msg)
{
    std::cout << "Existing Connection, Recieved: " << msg->ToString() << '\n';
    // Connection already exists.
    std::cout << msg->ToString();
    auto conn = connection.connection;
    conn->Send(msg->ToString() + connection.headers);
}
void tcp_handler(const evpp::TCPConnPtr &conn, evpp::Buffer *msg)
{
    auto connection = std::find(connections.begin(), connections.end(), (WebSocketConnection){conn});
    if (connection == connections.end())
        establish_websocket(conn, msg);
    else
        on_socket_message(*connection, msg);
}
