#include <iostream> // For std::cout
#include <evpp/tcp_server.h>
#include <evpp/buffer.h>
#include <evpp/tcp_conn.h>
#include <evpp/timestamp.h> // Included for consistency, but not directly used here
#include <glog/logging.h>   // For LOG_INFO, LOG_ERROR, etc.
#include "./tcp_handler.h"

// You might still want a simple context to track if this is the first message
// on a connection, to avoid logging "HTTP handshake" for every message.
struct ConnectionLogContext {
    bool first_message_received = false;
};
enum class ConnectionState {
    HTTP_HANDSHAKE_PENDING,
    WEBSOCKET_OPEN,
    WEBSOCKET_CLOSING,
    CLOSED
};
int main(int argc, char *argv[]) {
    // Initialize Google Logging
    google::InitGoogleLogging(argv[0]);

    std::string addr = "0.0.0.0:9099";
    int thread_num = 4;
    evpp::EventLoop loop;
    evpp::TCPServer server(&loop, addr, "TCPEchoServer", thread_num);

    // This callback is triggered when data arrives on the connection.
    server.SetMessageCallback(tcp_handler);

    // This callback is triggered when a TCP connection is established or lost.
    // No HTTP data is available here yet, only the fact of a connection.
    server.SetConnectionCallback([](const evpp::TCPConnPtr &conn) {
        if (conn->IsConnected()) {
            LOG_INFO << "A new TCP connection established from " << conn->remote_addr();
            // A new ConnectionLogContext will be created on the first message for this connection.
        } else {
            LOG_INFO << "Lost the TCP connection from " << conn->remote_addr();
            // Context will be automatically cleaned up by evpp when the connection closes.
        }
    });

    server.Init();
    server.Start();
    loop.Run();

    // Shutdown Google Logging when the loop stops
    google::ShutdownGoogleLogging();
    return 0;
}