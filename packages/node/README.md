# @pod-db/node

Client SDK for Pod DB. Works in Node 22+, Bun and browsers (any runtime with a
global `WebSocket`; on older Node pass one in, e.g. from the `ws` package).

```ts
import { Pod } from "@pod-db/node";

const pod = new Pod({
  uri: "ws://localhost:3000/webchannel",
  namespace: "team-a",          // optional; the server default otherwise
  token: jwt,                   // optional HS256 JWT, or getToken: async () => jwt
  encoding: "json",             // or "msgpack"
});

// Documents
const ref = pod.collection("users").doc("u1");
await ref.set({ name: "Ann", profile: { age: 30 } });             // { version, created }
await ref.set({ profile: { age: 31 } }, { merge: true });
const snap = await ref.get();                                      // snap.exists(), snap.data(), snap.version
await ref.delete();                                                // also deletes sub-collections
const added = await pod.collection("users").add({ name: "Bo" });   // generated uuidv7 id

// Queries (immutable builders)
const q = pod.collection("people").where("age", "GREATER_EQUAL", 18).orderBy("age", "desc").limit(20);
const page = await q.get();                                        // page.docs, page.hasMore, page.nextCursor
const next = await q.startAfter(page.nextCursor!).get();
await pod.collection("people").where("team", "EQUAL", "x").mutateMany({ set: { active: true }, remove: ["tmp"] });

// Real-time
const stop = q.onSnapshot(
  (s) => s.docChanges().forEach((c) => console.log(c.type, c.doc.id, c.oldIndex, c.newIndex)),
  (err) => console.error(err.code, err.message),
);
stop();

// Secondary indexes (admin token required when the server has rules)
const { index } = await pod.createIndex("posts", ["author", "createdAt"]);
await pod.listIndexes();
await pod.dropIndex(index.id);

pod.close();
```

Behaviour

- Connects lazily on the first request or listener. Requests are matched to
  responses by `request_id` and time out after `requestTimeoutMs` (30 s) with
  `DEADLINE_EXCEEDED`.
- Errors are `PodError` with a `code` (every server `IErrorCode`, plus
  `DEADLINE_EXCEEDED` and `CANCELLED`) and the server `status`.
- Reconnects with exponential backoff while listeners are active or requests
  are queued. A request already sent when the connection drops is rejected
  with `UNAVAILABLE` and never retried (it may or may not have been applied).
- Listeners deliver a snapshot per server commit. After a reconnect they
  re-subscribe with the resume token of the last complete commit, so only the
  missed changes are delivered; if the server no longer has them it resets
  and the SDK reports the difference as normal `docChanges()`.
- Close codes 4400 / 4401 / 4403 (bad namespace, token, or namespace not
  allowed by the token) stop the client: pending requests and listeners fail
  with `INVALID_ARGUMENT` / `UNAUTHENTICATED` / `PERMISSION_DENIED`.
- `cache: new MemoryStorageAdapter()` or `new IndexedDBStorageAdapter()` keeps
  listener views and resume tokens (per server, namespace and token `sub`),
  so a new listener shows cached data immediately (`metadata.fromCache`) and
  resumes from there.

Development

```bash
bun install
bun run typecheck
bun run test                                   # unit tests
POD_URL=ws://127.0.0.1:3000/webchannel bun run test:integration   # needs a running server
bun run build
```
