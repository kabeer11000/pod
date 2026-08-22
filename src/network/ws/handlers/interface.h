#include "expected"
#include "network/ws/wire_protocol.h"
#include "storage/interface.h"
// Read handler
std::expected<ReadResponse, ErrorResponse> read_handler(StorageDriver &d,
                                                        ReadRequest &packet);
