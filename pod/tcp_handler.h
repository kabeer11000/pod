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

void tcp_handler(
    const evpp::TCPConnPtr &conn,
    evpp::Buffer *msg);
