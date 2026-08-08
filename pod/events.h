// Copyright (c) Kabeer Jaffri & Kabeer's Network | Sources are under GNU 3.0 License
//
// This file contains event names and map which Pod supports
// I have learned from `cloudstore` (github.com/kabeer11000/cloudstore) and came up with a better wire protocol
// Instead of relying on dynamic event names e.g. inc-insert-<reference_id> we now only have one event called `message`
// We push reference ID and intent / action inside the request body (The Structured Query) this envoplope helps us avoid-
// -client side memory leaks, and avoids expensive string concatination on our backend.
//
// This also removes the need to return a reserved type such as `__cl_error__` which had caused issues on the client in
// cloudstore
