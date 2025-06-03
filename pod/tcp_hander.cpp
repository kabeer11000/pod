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
#include "./socket/frame.h"

using json = nlohmann::json;

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
    std::vector<std::string> extensions;
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
// Function to trim leading/trailing whitespace from a string
std::string trim(const std::string &str)
{
    size_t first = str.find_first_not_of(" \t\n\r");
    if (std::string::npos == first)
    {
        return str;
    }
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}
std::vector<WebSocketConnection> connections;
void establish_websocket(const evpp::TCPConnPtr &conn, evpp::Buffer *msg)
{

    std::cout << "New Connection, Received Request:\n"
              << msg->ToString() << '\n';

    bool connection_upgrade_found = false;
    bool upgrade_websocket_found = false;
    std::string sec_websocket_key;
    std::string sec_websocket_version;
    std::vector<std::string> client_requested_extensions; // Extensions requested by client
    std::string sec_websocket_protocol;                   // Optional: for subprotocols

    std::istringstream headers_stream(msg->ToString());
    std::string header_line;

    // Read and parse each header line
    while (std::getline(headers_stream, header_line))
    {
        // Remove carriage return if present (important for HTTP parsing)
        if (!header_line.empty() && header_line.back() == '\r')
        {
            header_line.pop_back();
        }

        // Ignore empty lines (end of headers)
        if (header_line.empty())
        {
            break;
        }

        std::string lower_case_line = header_line;
        std::transform(lower_case_line.begin(), lower_case_line.end(), lower_case_line.begin(), ::tolower);

        // --- Parsing specific headers ---
        // Headers are typically "Field-Name: Field-Value"
        size_t colon_pos = lower_case_line.find(':');
        if (colon_pos == std::string::npos)
        {
            // Not a standard header line, could be request line or malformed
            continue;
        }
        std::string field_name = trim(lower_case_line.substr(0, colon_pos));
        std::string field_value = trim(header_line.substr(colon_pos + 1)); // Use original case for value if needed later

        if (field_name == "connection")
        {
            // A Connection header can contain multiple tokens, e.g., "Connection: keep-alive, Upgrade"
            std::istringstream conn_value_stream(field_value);
            std::string token;
            while (std::getline(conn_value_stream, token, ','))
            {
                if (trim(token) == "upgrade")
                {
                    connection_upgrade_found = true;
                    break;
                }
            }
        }
        else if (field_name == "upgrade")
        {
            if (trim(field_value) == "websocket")
            {
                upgrade_websocket_found = true;
            }
        }
        else if (field_name == "sec-websocket-key")
        {
            sec_websocket_key = field_value;
        }
        else if (field_name == "sec-websocket-version")
        {
            sec_websocket_version = field_value;
        }
        else if (field_name == "sec-websocket-extensions")
        {
            std::istringstream ext_stream(field_value);
            std::string ext_token;
            while (std::getline(ext_stream, ext_token, ','))
            {
                client_requested_extensions.push_back(trim(ext_token));
            }
        }
        else if (field_name == "sec-websocket-protocol")
        {
            sec_websocket_protocol = field_value;
            // You would typically parse this into a list if multiple protocols are offered
        }
    }

    // --- WebSocket Handshake Validation ---
    // If any of the mandatory WebSocket handshake headers are missing or invalid,
    // it's not a WebSocket upgrade request.
    if (!connection_upgrade_found || !upgrade_websocket_found || sec_websocket_key.empty() || sec_websocket_version != "13")
    {
        std::cerr << "Not a valid WebSocket Handshake Request. Sending 400 Bad Request." << std::endl;
        conn->Send("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        conn->Close();
        return; // Exit, not a WebSocket connection
    }

    // --- If it IS a WebSocket Handshake Request, proceed with response ---

    // 1. Calculate Sec-WebSocket-Accept
    std::string combined = sec_websocket_key + WEBSOCKET_MAGIC_STRING;
    std::string sha1_result = sha1_hash(combined);
    std::string sec_websocket_accept = base64_encode(sha1_result);

    // 2. Negotiate Extensions (example: only support permessage-deflate)
    std::string negotiated_extensions_header = "";
    std::vector<std::string> final_extensions;
    bool permessage_deflate_supported = false;

    for (const auto &ext : client_requested_extensions)
    {
        // A robust server would parse parameters like "client_max_window_bits"
        // For simplicity, just check for "permessage-deflate" string
        if (ext.find("permessage-deflate") != std::string::npos)
        {
            permessage_deflate_supported = true;
            final_extensions.push_back("permessage-deflate"); // Add the agreed extension
            break;                                            // For simplicity, only confirm one permessage-deflate instance
        }
        // Add logic for other extensions you might support
    }

    if (permessage_deflate_supported)
    {
        // You might add specific parameters here if you support them
        negotiated_extensions_header = "Sec-WebSocket-Extensions: permessage-deflate\r\n";
    }

    // 3. Negotiate Subprotocols (example: only accept "chat" if requested)
    std::string negotiated_protocol_header = "";
    if (!sec_websocket_protocol.empty())
    {
        // Split client_offered_protocols by comma and check if server supports any
        std::istringstream proto_stream(sec_websocket_protocol);
        std::string proto_token;
        while (std::getline(proto_stream, proto_token, ','))
        {
            if (trim(proto_token) == "chat")
            { // Assume "chat" is the only supported subprotocol
                negotiated_protocol_header = "Sec-WebSocket-Protocol: chat\r\n";
                break;
            }
        }
    }

    // 4. Build HTTP response with proper CRLF line endings
    std::string response_str =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        sec_websocket_accept + "\r\n" +
        negotiated_extensions_header + // Add if negotiated
        negotiated_protocol_header +   // Add if negotiated
        "\r\n";                        // Empty line to end headers

    std::cout << "Sending WebSocket handshake response:\n"
              << response_str << std::endl;
    conn->Send(response_str);

    // 5. Store the WebSocket connection state
    // This is where you transition the TCP connection to a WebSocket connection.
    // You would typically associate this connection with a WebSocket object that
    // will now handle WebSocket frames (parse_frame function) instead of HTTP requests.
    connections.push_back(WebSocketConnection{conn, msg->ToString(), final_extensions});

    std::cout << "WebSocket connection established for " << conn->name() << std::endl;
}
void on_socket_message(const WebSocketConnection &connection, evpp::Buffer *msg)
{
    std::cout << "Existing Connection, Recieved: " << msg->ToString() << '\n';
    // Connection already exists.
    parse_frame(msg, connection.extensions);
    std::cout << msg->ToString();
    auto conn = connection.connection;
    conn->Send(msg->ToString() + connection.headers);
}
void tcp_handler(const evpp::TCPConnPtr &conn, evpp::Buffer *msg)
{
    std::cout << generate_uuid();
    auto connection = std::find(connections.begin(), connections.end(), (WebSocketConnection){conn});
    if (connection == connections.end())
        establish_websocket(conn, msg);
    else
        on_socket_message(*connection, msg);
}
