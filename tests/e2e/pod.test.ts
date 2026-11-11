// End-to-end tests against a real `pod` binary.
//
//   POD_BIN=/path/to/build/pod bun test
//
// Each run starts the server on a random port with a throwaway storage dir.
// Alternatively point the tests at an already running server (started with
// DEFAULT_NAMESPACE=fallback and an empty storage dir):
//
//   POD_EXTERNAL_PORT=3000 bun test
import { afterAll, beforeAll, describe, expect, test } from "bun:test";
import { applyEvents, Client as BaseClient, refusal, type Msg } from "./client";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const POD_BIN = process.env.POD_BIN ?? "../../build/pod";
const EXTERNAL_PORT = process.env.POD_EXTERNAL_PORT;
const PORT = EXTERNAL_PORT ? Number(EXTERNAL_PORT) : 20000 + Math.floor(Math.random() * 20000);
const storageDir = mkdtempSync(join(tmpdir(), "pod-e2e-"));
let server: ReturnType<typeof Bun.spawn> | undefined;
const HOST = `127.0.0.1:${PORT}`;

class Client extends BaseClient {
  static open(ns?: string, binary = false): Promise<BaseClient> {
    return BaseClient.connect(HOST, ns !== undefined ? { ns } : {}, binary);
  }
}

beforeAll(async () => {
  if (!EXTERNAL_PORT) server = Bun.spawn([POD_BIN], {
    env: {
      ...process.env,
      POD_PORT: String(PORT),
      POD_STORAGE_PATH: storageDir,
      DEFAULT_NAMESPACE: "fallback",
      POD_LOG_LEVEL: "warn",
    },
    stdout: "inherit",
    stderr: "inherit",
  });
  const deadline = Date.now() + 10000;
  while (true) {
    try {
      const r = await fetch(`http://127.0.0.1:${PORT}/`);
      if (r.ok) break;
    } catch {}
    if (Date.now() > deadline) throw new Error("server did not start");
    await Bun.sleep(100);
  }
});

afterAll(() => {
  server?.kill();
  rmSync(storageDir, { recursive: true, force: true });
});

describe("CRUD", () => {
  test("READ of a missing document is a 404 ACK with null data", async () => {
    const c = await Client.open("crud");
    const r = await c.request("READ", { path: "users/nobody" });
    expect(r.meta.type).toBe("ACK");
    expect(r.meta.status).toBe(404);
    expect(r.payload.document).toEqual({ path: "users/nobody", data: null, _v: null });
    c.close();
  });

  test("WRITE create / merge / replace with versions", async () => {
    const c = await Client.open("crud");
    let r = await c.request("WRITE", { path: "users/u1", merge: false, data: { name: "Ada", profile: { a: 1 } } });
    expect(r.meta.status).toBe(201);
    expect(r.payload).toEqual({ path: "users/u1", _v: 1 });

    r = await c.request("WRITE", { path: "/users/u1/", merge: true, data: { profile: { b: 2 }, age: 36 } });
    expect(r.meta.status).toBe(200);
    expect(r.payload._v).toBe(2);

    r = await c.request("READ", { path: "users/u1" });
    expect(r.meta.status).toBe(200);
    expect(r.payload.document).toEqual({
      path: "users/u1",
      data: { name: "Ada", profile: { a: 1, b: 2 }, age: 36 },
      _v: 2,
    });

    r = await c.request("WRITE", { path: "users/u1", merge: false, data: { replaced: true } });
    expect(r.payload._v).toBe(3);
    r = await c.request("READ", { path: "users/u1" });
    expect(r.payload.document.data).toEqual({ replaced: true });
    c.close();
  });

  test("validation errors use meta.type ERROR", async () => {
    const c = await Client.open("crud");
    let r = await c.request("WRITE", { path: "users", merge: false, data: {} });
    expect(r.meta.type).toBe("ERROR");
    expect(r.meta.status).toBe(400);
    expect(r.payload.code).toBe("INVALID_PATH");

    r = await c.request("WRITE", { path: "users/u9", merge: false, data: [1] });
    expect(r.payload.code).toBe("INVALID_ARGUMENT");

    r = await c.request("READ", { path: "a//b" });
    expect(r.payload.code).toBe("INVALID_PATH");

    r = await c.request("FROBNICATE", {});
    expect(r.payload.code).toBe("UNKNOWN_TYPE");
    expect(r.meta.request_id).toBe(`r${c.seq}`);

    c.ws.send("{not json");
    await c.waitFor(() => c.inbox.some((m) => m.payload?.code === "MALFORMED_REQUEST"));
    c.close();
  });

  test("DELETE is recursive for documents and collections", async () => {
    const c = await Client.open("del");
    for (const p of ["a/1", "a/1/sub/x", "a/1/sub/x/deeper/y", "a/2", "ab/1"]) {
      await c.request("WRITE", { path: p, merge: false, data: { p } });
    }
    let r = await c.request("DELETE", { path: "a/1" });
    expect(r.payload).toEqual({ target: "DOCUMENT", path: "a/1", affected_count: 3 });
    r = await c.request("READ", { path: "a/1/sub/x/deeper/y" });
    expect(r.meta.status).toBe(404);

    r = await c.request("DELETE", { path: "a" });
    expect(r.payload).toEqual({ target: "COLLECTION", path: "a", affected_count: 1 });
    r = await c.request("READ", { path: "ab/1" });
    expect(r.meta.status).toBe(200); // sibling collection untouched

    r = await c.request("DELETE", { path: "a/1" });
    expect(r.payload.affected_count).toBe(0);
    c.close();
  });

  test("requests on one connection are processed in order", async () => {
    const c = await Client.open("order");
    const ps: Promise<Msg>[] = [];
    for (let i = 1; i <= 50; i++) {
      ps.push(c.request("WRITE", { path: "o/doc", merge: true, data: { i } }));
      ps.push(c.request("READ", { path: "o/doc" }));
    }
    const rs = await Promise.all(ps);
    for (let i = 0; i < 50; i++) {
      expect(rs[2 * i].payload._v).toBe(i + 1);
      expect(rs[2 * i + 1].payload.document.data.i).toBe(i + 1);
    }
    c.close();
  });

  test("large documents (>16KB frames) are accepted", async () => {
    const c = await Client.open("big");
    const blob = "x".repeat(200_000);
    let r = await c.request("WRITE", { path: "big/1", merge: false, data: { blob } });
    expect(r.meta.status).toBe(201);
    r = await c.request("READ", { path: "big/1" });
    expect(r.payload.document.data.blob.length).toBe(200_000);
    c.close();
  });
});

describe("QUERY and MUTATE_MANY", () => {
  test("where / orderBy / limit / project / cursor pagination", async () => {
    const c = await Client.open("q");
    for (let i = 0; i < 10; i++) {
      await c.request("WRITE", {
        path: `items/i${i}`,
        merge: false,
        data: { n: i % 4, tag: i % 2 ? "odd" : "even", tags: ["t" + (i % 3)], meta: { i } },
      });
    }
    await c.request("WRITE", { path: "items/i0/child/c", merge: false, data: { n: 0 } });

    let r = await c.request("QUERY", { path: "items" });
    expect(r.payload.documents.length).toBe(10); // sub-collection excluded
    expect(r.payload.has_more).toBe(false);
    expect(r.payload.next_cursor).toBe(null);

    r = await c.request("QUERY", {
      path: "items",
      where: [
        { field: "tag", op: "EQUAL", value: "even" },
        { field: "n", op: "GREATER_EQUAL", value: 2 },
      ],
    });
    expect(r.payload.documents.map((d: Msg) => d.path)).toEqual(["items/i2", "items/i6"]);

    r = await c.request("QUERY", { path: "items", where: [{ field: "tags", op: "ARRAY.IN", value: "t1" }] });
    expect(r.payload.documents.map((d: Msg) => d.path)).toEqual(["items/i1", "items/i4", "items/i7"]);

    r = await c.request("QUERY", {
      path: "items",
      where: [{ field: "tags", op: "ARRAY.ELEMENT_AT", value: { index: 0, value: "t2" } }],
      project: ["meta.i"],
    });
    expect(r.payload.documents.map((d: Msg) => d.data)).toEqual([{ meta: { i: 2 } }, { meta: { i: 5 } }, { meta: { i: 8 } }]);

    const pages: string[] = [];
    let cursor: string | undefined;
    for (let guard = 0; guard < 10; guard++) {
      r = await c.request("QUERY", {
        path: "items",
        orderBy: [{ field: "n", direction: "DESCENDING" }],
        limit: 3,
        cursor,
      });
      expect(r.meta.status).toBe(200);
      pages.push(...r.payload.documents.map((d: Msg) => d.path.split("/")[1]));
      if (!r.payload.has_more) break;
      cursor = r.payload.next_cursor;
    }
    expect(pages).toEqual(["i7", "i3", "i6", "i2", "i9", "i5", "i1", "i8", "i4", "i0"]);

    r = await c.request("QUERY", { path: "items", cursor: "does-not-exist" });
    expect(r.payload.code).toBe("INVALID_CURSOR");
    r = await c.request("QUERY", { path: "items", limit: 0 });
    expect(r.payload.code).toBe("INVALID_ARGUMENT");
    r = await c.request("QUERY", { path: "items/i1" });
    expect(r.payload.code).toBe("INVALID_PATH");
    c.close();
  });

  test("MUTATE_MANY applies remove then set to matching documents", async () => {
    const c = await Client.open("mm");
    for (let i = 0; i < 6; i++) {
      await c.request("WRITE", { path: `m/d${i}`, merge: false, data: { group: i % 2, old: true, i } });
    }
    let r = await c.request("MUTATE_MANY", {
      path: "m",
      where: [{ field: "group", op: "EQUAL", value: 1 }],
      remove: ["old"],
      set: { "status.done": true, i: -1 },
    });
    expect(r.payload).toEqual({ path: "m", affected_count: 3 });
    r = await c.request("READ", { path: "m/d1" });
    expect(r.payload.document).toEqual({ path: "m/d1", data: { group: 1, i: -1, status: { done: true } }, _v: 2 });
    r = await c.request("READ", { path: "m/d0" });
    expect(r.payload.document._v).toBe(1);

    r = await c.request("MUTATE_MANY", { path: "m", remove: ["a"], set: { "a.b": 1 } });
    expect(r.payload.code).toBe("INVALID_ARGUMENT");
    r = await c.request("MUTATE_MANY", { path: "m" });
    expect(r.payload.code).toBe("INVALID_ARGUMENT");
    c.close();
  });
});

describe("namespaces", () => {
  test("data is isolated per namespace", async () => {
    const a = await Client.open("tenant_a");
    const b = await Client.open("tenant_b");
    await a.request("WRITE", { path: "docs/shared", merge: false, data: { owner: "a" } });
    let r = await b.request("READ", { path: "docs/shared" });
    expect(r.meta.status).toBe(404);
    await b.request("WRITE", { path: "docs/shared", merge: false, data: { owner: "b" } });
    r = await a.request("READ", { path: "docs/shared" });
    expect(r.payload.document.data.owner).toBe("a");
    r = await b.request("DELETE", { path: "docs" });
    expect(r.payload.affected_count).toBe(1);
    r = await a.request("QUERY", { path: "docs" });
    expect(r.payload.documents.length).toBe(1);
    a.close();
    b.close();
  });

  test("connections without ?ns use DEFAULT_NAMESPACE", async () => {
    const implicit = await Client.open();
    const explicit = await Client.open("fallback");
    await implicit.request("WRITE", { path: "x/y", merge: false, data: { v: 1 } });
    const r = await explicit.request("READ", { path: "x/y" });
    expect(r.meta.status).toBe(200);
    implicit.close();
    explicit.close();
  });

  test("invalid namespaces are refused with close code 4400", async () => {
    for (const ns of ["bad/ns", "x".repeat(65)]) {
      const r = await refusal(HOST, { ns });
      expect(r.code).toBe(4400);
      expect(r.reason.length).toBeGreaterThan(0);
    }
  });

  test("a token is refused when the server has no secret configured", async () => {
    expect((await refusal(HOST, { ns: "a", t: "x.y.z" })).code).toBe(4401);
  });
});

describe("subscriptions", () => {
  test("collection subscription: ACK, SNAPSHOT, then ordered deltas", async () => {
    const sub = await Client.open("rt");
    const writer = await Client.open("rt");
    const other = await Client.open("rt_other");
    await writer.request("WRITE", { path: "scores/a", merge: false, data: { s: 10 } });
    await writer.request("WRITE", { path: "scores/b", merge: false, data: { s: 30 } });

    const ack = await sub.request("SUBSCRIBE", {
      subscription_id: "top",
      path: "scores",
      orderBy: [{ field: "s", direction: "DESCENDING" }],
    });
    expect(ack.meta.type).toBe("ACK");
    expect(ack.payload).toEqual({ subscription_id: "top", status: "SUBSCRIBED", resumed: false });
    let ev = await sub.eventsFor("top", 1);
    expect(ev[0].payload.event_type).toBe("SNAPSHOT");
    expect(ev[0].payload.snapshot.is_synced).toBe(true);
    expect(ev[0].payload.snapshot.documents.map((d: Msg) => [d.index, d.path])).toEqual([
      [0, "scores/b"],
      [1, "scores/a"],
    ]);
    // The ACK frame arrives before the SNAPSHOT frame.
    expect(sub.inbox.findIndex((m) => m === ack)).toBeLessThan(sub.inbox.indexOf(ev[0]));

    await writer.request("WRITE", { path: "scores/c", merge: false, data: { s: 20 } });
    ev = await sub.eventsFor("top", 2);
    expect(ev[1].payload.event_type).toBe("ADDED");
    expect(ev[1].payload.delta).toMatchObject({ path: "scores/c", old_index: null, new_index: 1 });
    expect(ev[1].payload.delta.document).toEqual({ path: "scores/c", data: { s: 20 }, _v: 1 });

    await writer.request("WRITE", { path: "scores/a", merge: true, data: { s: 40 } });
    ev = await sub.eventsFor("top", 3);
    expect(ev[2].payload.event_type).toBe("MODIFIED");
    expect(ev[2].payload.delta).toMatchObject({ path: "scores/a", old_index: 2, new_index: 0 });

    await writer.request("DELETE", { path: "scores/b" });
    ev = await sub.eventsFor("top", 4);
    expect(ev[3].payload.event_type).toBe("REMOVED");
    expect(ev[3].payload.delta).toMatchObject({ path: "scores/b", old_index: 1, new_index: null });

    // Writes in another namespace never reach this subscription.
    await other.request("WRITE", { path: "scores/z", merge: false, data: { s: 99 } });
    // resume tokens are increasing commit sequence numbers
    const tokens = ev.map((e: Msg) => BigInt(e.payload.resume_token));
    for (let i = 1; i < tokens.length; i++) expect(tokens[i] > tokens[i - 1]).toBe(true);

    // Client ACK has no reply.
    const before = sub.inbox.length;
    sub.sendRaw({
      meta: { type: "ACK", subscription_id: "top", timestamp: new Date().toISOString() },
      payload: { resume_token: ev[3].payload.resume_token },
    });

    const dup = await sub.request("SUBSCRIBE", { subscription_id: "top", path: "scores" });
    expect(dup.payload.code).toBe("ALREADY_EXISTS");
    expect(sub.inbox.length).toBe(before + 1); // only the error above

    expect(applyEvents(sub.events.get("top")!)).toEqual(['scores/a:{"s":40}', 'scores/c:{"s":20}']);

    const un = await sub.request("UNSUBSCRIBE", { subscription_id: "top" });
    expect(un.payload).toEqual({ subscription_id: "top", status: "UNSUBSCRIBED" });
    await writer.request("WRITE", { path: "scores/d", merge: false, data: { s: 1 } });
    await sub.request("READ", { path: "scores/d" }); // round-trip barrier
    expect(sub.events.get("top")!.length).toBe(4);
    const again = await sub.request("UNSUBSCRIBE", { subscription_id: "top" });
    expect(again.payload.code).toBe("NOT_FOUND");
    sub.close();
    writer.close();
    other.close();
  });

  test("document subscription and resume_token RESET", async () => {
    const sub = await Client.open("rt_doc");
    const writer = await Client.open("rt_doc");
    await sub.request("SUBSCRIBE", { subscription_id: "d", path: "profiles/p1", project: ["name"] });
    let ev = await sub.eventsFor("d", 1);
    expect(ev[0].payload.snapshot.documents).toEqual([]);

    await writer.request("WRITE", { path: "profiles/p1", merge: false, data: { name: "x", secret: 1 } });
    ev = await sub.eventsFor("d", 2);
    expect(ev[1].payload.event_type).toBe("ADDED");
    expect(ev[1].payload.delta.document.data).toEqual({ name: "x" }); // projected

    await writer.request("DELETE", { path: "profiles/p1" });
    ev = await sub.eventsFor("d", 3);
    expect(ev[2].payload.event_type).toBe("REMOVED");

    // A token still covered by the commit log resumes with deltas only.
    let r = await sub.request("SUBSCRIBE", { subscription_id: "d2", path: "profiles/p1", resume_token: "0" });
    expect(r.payload.resumed).toBe(true);
    const ev2 = await sub.eventsFor("d2", 1);
    expect(ev2.map((e: Msg) => e.payload.event_type)).toEqual(["SYNCED"]); // created+deleted since: no net change
    expect(ev2[0].payload.commit_complete).toBe(true);

    // An unknown token falls back to RESET + SNAPSHOT.
    r = await sub.request("SUBSCRIBE", { subscription_id: "d4", path: "profiles/p1", resume_token: "999999999" });
    expect(r.payload.resumed).toBe(false);
    const ev4 = await sub.eventsFor("d4", 2);
    expect(ev4.map((e: Msg) => e.payload.event_type)).toEqual(["RESET", "SNAPSHOT"]);
    expect(ev4.map((e: Msg) => e.payload.commit_complete)).toEqual([false, true]);

    const bad = await sub.request("SUBSCRIBE", { subscription_id: "d3", path: "profiles/p1", limit: 1 });
    expect(bad.payload.code).toBe("INVALID_ARGUMENT");
    sub.close();
    writer.close();
  });

  test("limited subscription back-fills when a document leaves the window", async () => {
    const sub = await Client.open("rt_lim");
    const w = await Client.open("rt_lim");
    for (let i = 1; i <= 5; i++) await w.request("WRITE", { path: `l/d${i}`, merge: false, data: { n: i } });
    await sub.request("SUBSCRIBE", { subscription_id: "lim", path: "l", orderBy: [{ field: "n" }], limit: 2 });
    await sub.eventsFor("lim", 1);
    await w.request("DELETE", { path: "l/d1" });
    await w.request("WRITE", { path: "l/d0", merge: false, data: { n: 0 } });
    await w.request("MUTATE_MANY", { path: "l", where: [{ field: "n", op: "GREATER", value: 3 }], set: { n: -1 } });
    await w.request("READ", { path: "l/d0" });
    await sub.waitFor(() => applyEvents(sub.events.get("lim")!).join() === 'l/d4:{"n":-1},l/d5:{"n":-1}');
    sub.close();
    w.close();
  });

  test("resume after reconnect delivers only the missed deltas", async () => {
    const w = await Client.open("rt_resume");
    for (let i = 1; i <= 4; i++) await w.request("WRITE", { path: `r/d${i}`, merge: false, data: { n: i } });
    let c = await Client.open("rt_resume");
    await c.request("SUBSCRIBE", { subscription_id: "s", path: "r", orderBy: [{ field: "n" }] });
    const first = await c.eventsFor("s", 1);
    // State the client would persist at a commit_complete boundary.
    const savedToken = first[0].payload.resume_token;
    const savedView = first[0].payload.snapshot.documents.map((d: Msg) => d.path);
    c.close();

    // While offline: one update, one delete, one insert.
    await w.request("WRITE", { path: "r/d1", merge: true, data: { n: 10 } });
    await w.request("DELETE", { path: "r/d2" });
    await w.request("WRITE", { path: "r/d5", merge: false, data: { n: 5 } });

    c = await Client.open("rt_resume");
    const ack = await c.request("SUBSCRIBE", {
      subscription_id: "s",
      path: "r",
      orderBy: [{ field: "n" }],
      resume_token: savedToken,
    });
    expect(ack.payload.resumed).toBe(true);
    const ev = await c.eventsFor("s", 4);
    expect(ev.map((e: Msg) => e.payload.event_type)).toEqual(["REMOVED", "ADDED", "MODIFIED", "SYNCED"]);
    // Apply to the saved view.
    const docs = savedView.map((p: string) => ({ path: p }));
    for (const e of ev) {
      const d = e.payload.delta;
      if (e.payload.event_type === "REMOVED") docs.splice(d.old_index, 1);
      if (e.payload.event_type === "ADDED") docs.splice(d.new_index, 0, d.document);
      if (e.payload.event_type === "MODIFIED") {
        docs.splice(d.old_index, 1);
        docs.splice(d.new_index, 0, d.document);
      }
    }
    expect(docs.map((d: Msg) => d.path)).toEqual(["r/d3", "r/d4", "r/d5", "r/d1"]);
    c.close();
    w.close();
  });

  test("closing a connection drops its subscriptions", async () => {
    const sub = await Client.open("rt_close");
    const w = await Client.open("rt_close");
    await sub.request("SUBSCRIBE", { subscription_id: "s", path: "c" });
    sub.close();
    await Bun.sleep(100);
    const r = await w.request("WRITE", { path: "c/x", merge: false, data: {} });
    expect(r.meta.status).toBe(201); // server keeps running fine
    w.close();
  });
});

describe("indexes", () => {
  test("create / list / query with index / drop", async () => {
    const c = await Client.open("idx");
    for (let i = 0; i < 200; i++) {
      await c.request("WRITE", {
        path: `users/u${i % 4}/posts/p${i}`,
        merge: false,
        data: { author: `a${i % 10}`, score: i, tags: i % 2 ? ["odd"] : ["even"] },
      });
    }
    let r = await c.request("CREATE_INDEX", {
      collection: "posts",
      fields: [{ field: "author" }, { field: "score" }],
    });
    expect(r.meta.status).toBe(201);
    expect(r.payload.index.id).toBe("posts:author,score");
    expect(r.payload.documents_indexed).toBe(200); // backfilled across all users/*/posts
    r = await c.request("CREATE_INDEX", { collection: "posts", fields: [{ field: "author" }, { field: "score" }] });
    expect(r.meta.status).toBe(200);

    const q = {
      path: "users/u1/posts",
      where: [{ field: "author", op: "EQUAL", value: "a5" }],
      orderBy: [{ field: "score", direction: "DESCENDING" }],
      limit: 2,
    };
    r = await c.request("QUERY", q);
    expect(r.payload.plan.index).toBe("posts:author,score");
    expect(r.payload.documents.map((d: Msg) => d.data.score)).toEqual([185, 165]);
    expect(r.payload.plan.documents_scanned).toBeLessThanOrEqual(3);

    // Index maintenance on write.
    await c.request("WRITE", { path: "users/u1/posts/new", merge: false, data: { author: "a5", score: 1000 } });
    r = await c.request("QUERY", q);
    expect(r.payload.documents.map((d: Msg) => d.data.score)).toEqual([1000, 185]);

    r = await c.request("LIST_INDEXES", {});
    expect(r.payload.indexes.map((i: Msg) => i.id)).toEqual(["posts:author,score"]);

    r = await c.request("CREATE_INDEX", { collection: "users/u1/posts", fields: [{ field: "x" }] });
    expect(r.payload.code).toBe("INVALID_ARGUMENT");

    r = await c.request("DROP_INDEX", { id: "posts:author,score" });
    expect(r.payload.status).toBe("DROPPED");
    r = await c.request("QUERY", q);
    expect(r.payload.plan.index).toBe(null);
    expect(r.payload.documents.map((d: Msg) => d.data.score)).toEqual([1000, 185]);
    r = await c.request("DROP_INDEX", { id: "posts:author,score" });
    expect(r.payload.code).toBe("NOT_FOUND");
    c.close();
  });
});

describe("MessagePack", () => {
  test("binary frames get binary responses and events", async () => {
    const c = await Client.open("mp", true);
    const big = 2n ** 40n;
    let r = await c.request("WRITE", { path: "bin/1", merge: false, data: { n: Number(big), f: 1.5, ok: true, nil: null } });
    expect(r.meta.status).toBe(201);
    r = await c.request("READ", { path: "bin/1" });
    expect(r.payload.document.data).toEqual({ n: Number(big), f: 1.5, ok: true, nil: null });
    await c.request("SUBSCRIBE", { subscription_id: "b", path: "bin" });
    const ev = await c.eventsFor("b", 1);
    expect(ev[0].payload.snapshot.documents.length).toBe(1);
    expect(c.inbox.every((m) => typeof m === "object")).toBe(true);
    c.close();
  });
});
