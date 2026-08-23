#pragma once
#include "storage/interface.h"

// Single-node entry point: serves every meta.type out of the local StorageDriver.
int entry_standalone(StorageDriver &d);
// Router entry point: forwards each meta.type to whichever Pod node owns the
// key (not yet implemented - see entry/pods.cpp).
int entry_pods(StorageDriver &d);