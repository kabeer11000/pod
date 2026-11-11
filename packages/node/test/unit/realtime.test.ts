// Connection, reconnect and resume behaviour against a scripted fake socket.
import { describe, expect, test } from "bun:test";
import { MemoryStorageAdapter, Pod, PodError, type DocumentSnapshot, type QuerySnapshot } from "../../src/index";

type Frame = { meta: Record<string, any>; payload: Record<string, any> };

class FakeSocket {
  static all: FakeSocket[] = [];
  binaryType = "blob";
  onopen: ((ev: unknown) => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  onclose: ((ev: { code: number; reason: string }) => void) | null = null;
  onerror: ((ev: unknown) => void) | null = null;
  sent: Frame[] = [];
  closed = false;

  constructor(readonly url: string) {
    FakeSocket.all.push(this);
  }
  send(data: string) {
    this.sent.push(JSON.parse(data));
  }
  close(code = 1000, reason = "") {
    if (this.closed) return;
    this.closed = true;
    queueMicrotask(() => this.onclose?.({ code, reason }));
  }
  // Server side
  accept() {
    this.onopen?.({});
  }
  push(frame: object) {
    this.onmessage?.({ data: JSON.stringify(frame) });
  }
  serverClose(code: number, reason = "") {
    this.closed = true;
    this.onclose?.({ code, reason });
  }
  last(type: string): Frame {
    const f = [...this.sent].reverse().find((x) => x.meta.type === type);
    if (!f) throw new Error(`no ${type} frame sent`);
    return f;
  }
}

const tick = (ms = 0) => new Promise((r) => setTimeout(r, ms));

function makePod(extra: Partial<ConstructorParameters<typeof Pod>[0]> = {}) {
  FakeSocket.all = [];
  return new Pod({
    uri: "ws://fake/webchannel",
    namespace: "team-a",
    WebSocket: FakeSocket as unknown as new (url: string) => WebSocket,
    reconnect: { initialDelayMs: 1, maxDelayMs: 2 },
    requestTimeoutMs: 200,
    ...extra,
  });
}

async function socket(n: number): Promise<FakeSocket> {
  for (let i = 0; i < 200 && FakeSocket.all.length <= n; i++) await tick(1);
  const s = FakeSocket.all[n];
  if (!s) throw new Error(`socket ${n} never opened`);
  return s;
}

const ack = (rid: string, status = 200, payload: object = {}) => ({
  meta: { type: "ACK", request_id: rid, timestamp: "t", status },
  payload,
});
const wdoc = (id: string, v = 1, extra = {}) => ({ path: `tasks/${id}`, data: { id, ...extra }, _v: v });
const ev = (sid: string, event_type: string, token: string, commit_complete: boolean, extra = {}) => ({
  meta: { type: "EVENT", subscription_id: sid, timestamp: "t" },
  payload: { event_type, resume_token: token, commit_complete, ...extra },
});
const snapshotEv = (sid: string, token: string, docs: ReturnType<typeof wdoc>[]) =>
  ev(sid, "SNAPSHOT", token, true, {
    snapshot: { is_synced: true, documents: docs.map((d, index) => ({ ...d, index })) },
  });
const deltaEv = (sid: string, type: string, token: string, done: boolean, delta: object) =>
  ev(sid, type, token, done, { delta });

describe("connection", () => {
  test("url carries namespace and token; requests correlate by request_id", async () => {
    const pod = makePod({ token: "abc" });
    const p = pod.doc("tasks/t1").get();
    const s = await socket(0);
    expect(new URL(s.url).searchParams.get("ns")).toBe("team-a");
    expect(new URL(s.url).searchParams.get("t")).toBe("abc");
    s.accept();
    const req = s.last("READ");
    expect(req.payload).toEqual({ path: "tasks/t1" });
    s.push(ack("someone-else", 200, {}));
    s.push(ack(req.meta.request_id, 404, { document: { path: "tasks/t1", data: null } }));
    const snap = await p;
    expect(snap.exists()).toBe(false);
    expect(snap.data()).toBeUndefined();
    pod.close();
  });

  test("server ERROR frames reject with typed PodError", async () => {
    const pod = makePod();
    const p = pod.doc("tasks/t1").set({ a: 1 });
    const s = await socket(0);
    s.accept();
    const rid = s.last("WRITE").meta.request_id;
    s.push({
      meta: { type: "ERROR", request_id: rid, timestamp: "t", status: 403 },
      payload: { code: "PERMISSION_DENIED", message: "nope" },
    });
    const err = (await p.catch((e) => e)) as PodError;
    expect(err).toBeInstanceOf(PodError);
    expect(err.code).toBe("PERMISSION_DENIED");
    expect(err.status).toBe(403);
    pod.close();
  });

  test("in-flight requests fail UNAVAILABLE on disconnect and are not resent", async () => {
    const pod = makePod();
    const write = pod.doc("tasks/t1").set({ a: 1 });
    const s0 = await socket(0);
    s0.accept();
    expect(s0.sent).toHaveLength(1);
    s0.serverClose(1006);
    await expect(write).rejects.toMatchObject({ code: "UNAVAILABLE" });
    // A request issued while disconnected is queued and sent after reconnect.
    const read = pod.doc("tasks/t1").get();
    const s1 = await socket(1);
    s1.accept();
    expect(s1.sent.map((f) => f.meta.type)).toEqual(["READ"]);
    s1.push(ack(s1.last("READ").meta.request_id, 200, { document: { path: "tasks/t1", data: { a: 1 }, _v: 3 } }));
    expect((await read).version).toBe(3);
    pod.close();
  });

  test("requests time out with DEADLINE_EXCEEDED", async () => {
    const pod = makePod({ requestTimeoutMs: 20 });
    const p = pod.doc("tasks/t1").get();
    (await socket(0)).accept();
    await expect(p).rejects.toMatchObject({ code: "DEADLINE_EXCEEDED" });
    pod.close();
  });

  test("close() rejects pending requests with CANCELLED", async () => {
    const pod = makePod();
    const p = pod.doc("tasks/t1").get();
    pod.close();
    await expect(p).rejects.toMatchObject({ code: "CANCELLED" });
    await expect(pod.doc("tasks/t1").get()).rejects.toMatchObject({ code: "CANCELLED" });
  });

  test.each([
    [4400, "INVALID_ARGUMENT"],
    [4401, "UNAUTHENTICATED"],
    [4403, "PERMISSION_DENIED"],
  ])("close code %d is fatal: no reconnect, listeners get %s", async (closeCode, errCode) => {
    const pod = makePod();
    const errors: PodError[] = [];
    pod.collection("tasks").onSnapshot(
      () => {},
      (e) => errors.push(e),
    );
    const pending = pod.doc("tasks/x").get();
    const s = await socket(0);
    s.serverClose(closeCode, "denied");
    await expect(pending).rejects.toMatchObject({ code: errCode });
    expect(errors.map((e) => e.code as string)).toEqual([errCode]);
    await tick(20);
    expect(FakeSocket.all).toHaveLength(1);
  });
});

describe("listeners", () => {
  test("snapshot, deltas grouped by commit, client ACKs", async () => {
    const pod = makePod();
    const snaps: QuerySnapshot[] = [];
    const unsub = pod
      .collection("tasks")
      .orderBy("id")
      .onSnapshot((s) => snaps.push(s));
    const s = await socket(0);
    s.accept();
    await tick();
    const sub = s.last("SUBSCRIBE");
    expect(sub.payload.path).toBe("tasks");
    expect(sub.payload.orderBy).toEqual([{ field: "id", direction: "ASCENDING" }]);
    expect(sub.payload.resume_token).toBeUndefined();
    const sid = sub.payload.subscription_id;
    s.push(ack(sub.meta.request_id, 200, { subscription_id: sid, status: "SUBSCRIBED", resumed: false }));
    s.push(snapshotEv(sid, "5", [wdoc("a"), wdoc("c")]));
    expect(snaps).toHaveLength(1);
    expect(snaps[0]!.docs.map((d) => d.id)).toEqual(["a", "c"]);
    expect(snaps[0]!.docChanges().map((c) => c.type)).toEqual(["added", "added"]);
    expect(s.last("ACK").payload.resume_token).toBe("5");

    // One commit with two events: emitted once, at commit_complete.
    s.push(deltaEv(sid, "ADDED", "6", false, { path: "tasks/b", old_index: null, new_index: 1, document: wdoc("b") }));
    expect(snaps).toHaveLength(1);
    s.push(deltaEv(sid, "REMOVED", "6", true, { path: "tasks/c", old_index: 2, new_index: null }));
    expect(snaps).toHaveLength(2);
    expect(snaps[1]!.docs.map((d) => d.id)).toEqual(["a", "b"]);
    expect(snaps[1]!.docChanges().map((c) => [c.type, c.doc.id, c.oldIndex, c.newIndex])).toEqual([
      ["added", "b", -1, 1],
      ["removed", "c", 2, -1],
    ]);
    expect(s.last("ACK").payload.resume_token).toBe("6");

    unsub();
    expect(s.last("UNSUBSCRIBE").payload.subscription_id).toBe(sid);
    s.push(deltaEv(sid, "REMOVED", "7", true, { path: "tasks/a", old_index: 0, new_index: null }));
    expect(snaps).toHaveLength(2);
    pod.close();
  });

  test("reconnect resumes from the last complete commit and applies resume deltas", async () => {
    const pod = makePod();
    const snaps: QuerySnapshot[] = [];
    pod.collection("tasks").onSnapshot((s) => snaps.push(s));
    const s0 = await socket(0);
    s0.accept();
    await tick();
    const sid0 = s0.last("SUBSCRIBE").payload.subscription_id;
    s0.push(snapshotEv(sid0, "10", [wdoc("a"), wdoc("b")]));
    // A commit that is cut off half way: must not become the resume point.
    s0.push(deltaEv(sid0, "REMOVED", "11", false, { path: "tasks/a", old_index: 0, new_index: null }));
    s0.serverClose(1006);

    const s1 = await socket(1);
    s1.accept();
    await tick();
    const sub = s1.last("SUBSCRIBE");
    expect(sub.payload.resume_token).toBe("10");
    const sid1 = sub.payload.subscription_id;
    expect(sid1).not.toBe(sid0);
    s1.push(ack(sub.meta.request_id, 200, { subscription_id: sid1, status: "SUBSCRIBED", resumed: true }));
    // Deltas relative to the view at token 10 ([a, b]).
    s1.push(deltaEv(sid1, "REMOVED", "12", false, { path: "tasks/a", old_index: 0, new_index: null }));
    s1.push(deltaEv(sid1, "MODIFIED", "12", false, { path: "tasks/b", old_index: 0, new_index: 0, document: wdoc("b", 2) }));
    // Late frame from the dead subscription id is ignored.
    s1.push(deltaEv(sid0, "REMOVED", "12", true, { path: "tasks/b", old_index: 0, new_index: null }));
    s1.push(ev(sid1, "SYNCED", "12", true));
    expect(snaps).toHaveLength(2);
    expect(snaps[1]!.docs.map((d) => [d.id, d.version])).toEqual([["b", 2]]);
    expect(snaps[1]!.docChanges().map((c) => c.type)).toEqual(["removed", "modified"]);
    pod.close();
  });

  test("a listener created after a drop subscribes exactly once", async () => {
    const pod = makePod();
    const read = pod.doc("tasks/a").get();
    const s0 = await socket(0);
    s0.accept();
    s0.push(ack(s0.last("READ").meta.request_id, 404, { document: { path: "tasks/a", data: null } }));
    await read;
    s0.serverClose(1006); // nothing wants the connection: no reconnect yet
    await tick(10);
    expect(FakeSocket.all).toHaveLength(1);
    pod.collection("tasks").onSnapshot(() => {});
    const s1 = await socket(1);
    s1.accept();
    await tick(10);
    expect(s1.sent.filter((f) => f.meta.type === "SUBSCRIBE")).toHaveLength(1);
    pod.close();
  });

  test("failed connect attempts do not duplicate a queued subscribe", async () => {
    const pod = makePod();
    pod.collection("tasks").onSnapshot(() => {});
    const s0 = await socket(0);
    s0.serverClose(1006); // never opened
    const s1 = await socket(1);
    s1.accept();
    await tick(10);
    expect(s1.sent.filter((f) => f.meta.type === "SUBSCRIBE")).toHaveLength(1);
    pod.close();
  });

  test("RESET + SNAPSHOT replaces the view with the minimal changes", async () => {
    const pod = makePod();
    const snaps: QuerySnapshot[] = [];
    pod.collection("tasks").onSnapshot((s) => snaps.push(s));
    const s0 = await socket(0);
    s0.accept();
    await tick();
    s0.push(snapshotEv(s0.last("SUBSCRIBE").payload.subscription_id, "3", [wdoc("a"), wdoc("b")]));
    s0.serverClose(1006);
    const s1 = await socket(1);
    s1.accept();
    await tick();
    const sid = s1.last("SUBSCRIBE").payload.subscription_id;
    s1.push(ev(sid, "RESET", "90", false));
    s1.push(snapshotEv(sid, "90", [wdoc("b"), wdoc("c")]));
    expect(snaps).toHaveLength(2);
    expect(snaps[1]!.docs.map((d) => d.id)).toEqual(["b", "c"]);
    expect(snaps[1]!.docChanges().map((c) => [c.type, c.doc.id])).toEqual([
      ["removed", "a"],
      ["added", "c"],
    ]);
    pod.close();
  });

  test("a delta that does not match the view triggers a fresh subscribe", async () => {
    const pod = makePod();
    const snaps: QuerySnapshot[] = [];
    pod.collection("tasks").onSnapshot((s) => snaps.push(s));
    const s = await socket(0);
    s.accept();
    await tick();
    const sid = s.last("SUBSCRIBE").payload.subscription_id;
    s.push(snapshotEv(sid, "1", [wdoc("a")]));
    s.push(deltaEv(sid, "REMOVED", "2", true, { path: "tasks/zzz", old_index: 0, new_index: null }));
    expect(s.last("UNSUBSCRIBE").payload.subscription_id).toBe(sid);
    const again = s.last("SUBSCRIBE");
    expect(again.payload.subscription_id).not.toBe(sid);
    expect(again.payload.resume_token).toBeUndefined();
    s.push(snapshotEv(again.payload.subscription_id, "2", [wdoc("a")]));
    // Same content as before: nothing to report.
    expect(snaps).toHaveLength(1);
    pod.close();
  });

  test("document listener and subscribe errors", async () => {
    const pod = makePod();
    const snaps: DocumentSnapshot[] = [];
    const errors: PodError[] = [];
    pod.doc("tasks/a").onSnapshot((d) => snaps.push(d));
    pod.doc("secret/x").onSnapshot(
      () => {},
      (e) => errors.push(e),
    );
    const s = await socket(0);
    s.accept();
    await tick();
    const [subA, subX] = s.sent.filter((f) => f.meta.type === "SUBSCRIBE");
    s.push({
      meta: { type: "ERROR", request_id: subX!.meta.request_id, timestamp: "t", status: 403 },
      payload: { code: "PERMISSION_DENIED", message: "no" },
    });
    await tick();
    expect(errors.map((e) => e.code)).toEqual(["PERMISSION_DENIED"]);
    const sid = subA!.payload.subscription_id;
    s.push(snapshotEv(sid, "1", []));
    s.push(deltaEv(sid, "ADDED", "2", true, { path: "tasks/a", old_index: null, new_index: 0, document: wdoc("a", 1, { t: 1 }) }));
    expect(snaps.map((d) => [d.exists(), d.get("t") ?? null])).toEqual([
      [false, null],
      [true, 1],
    ]);
    pod.close();
  });

  test("cache: a new listener shows cached data and resumes from its token", async () => {
    const cache = new MemoryStorageAdapter();
    const pod = makePod({ cache });
    const first: QuerySnapshot[] = [];
    const stop = pod.collection("tasks").onSnapshot((s) => first.push(s));
    const s = await socket(0);
    s.accept();
    await tick(5);
    s.push(snapshotEv(s.last("SUBSCRIBE").payload.subscription_id, "7", [wdoc("a")]));
    stop();
    await tick(5);

    const second: QuerySnapshot[] = [];
    pod.collection("tasks").onSnapshot((snap) => second.push(snap));
    await tick(5);
    expect(second).toHaveLength(1);
    expect(second[0]!.metadata.fromCache).toBe(true);
    expect(second[0]!.docs.map((d) => d.id)).toEqual(["a"]);
    const sub = s.last("SUBSCRIBE");
    expect(sub.payload.resume_token).toBe("7");
    s.push(ev(sub.payload.subscription_id, "SYNCED", "8", true));
    // First server confirmation is always delivered.
    expect(second).toHaveLength(2);
    expect(second[1]!.metadata.fromCache).toBe(false);
    pod.close();
  });
});
