#pragma once
#include "storage/interface.h"

auto msgPackResponder(auto &ws, auto &response);
int entry_standalone(StorageDriver &d);
