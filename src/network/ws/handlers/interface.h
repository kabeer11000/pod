#include "expected"
#include "indexing/engine.h"
#include "network/ws/wire_protocol.h"
#include "storage/interface.h"
// Read handler
std::expected<ReadResponse, ErrorResponse> read_handler(StorageDriver &d,
                                                        ReadRequest &packet);
// Write handler. When `index_engine` is non-null, keeps every index
// registered on the document's collection in sync with the write (see
// indexing/engine.h).
std::expected<WriteResponse, ErrorResponse>
write_handler(StorageDriver &d, WriteRequest &packet,
             IndexEngine *index_engine = nullptr);
// Delete handler (document-level only). When `index_engine` is non-null,
// removes the deleted document's entries from every index registered on its
// collection.
std::expected<DeleteResponse, ErrorResponse>
delete_handler(StorageDriver &d, DeleteRequest &packet,
              IndexEngine *index_engine = nullptr);
