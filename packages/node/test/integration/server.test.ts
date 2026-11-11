// Integration tests against a running Pod server (no rules / open mode).
//
//   POD_URL=ws://127.0.0.1:3000/webchannel bun test test/integration
//
// Every test uses its own random namespace, so the server's storage does not
// need to be empty.
import { afterEach, describe, expect, test } from "bun:test";
import { Pod, type DocumentSnapshot, type PodOptions, type QuerySnapshot } from "../../src/index";

const URL = process.env.POD_URL ?? "ws://127.0.0.1:38555/webchannel";
const opened: Pod[] = [];

function client(namespace: string, extra: Partial<PodOptions> = {}): Pod {
  const pod = new Pod({ uri: URL, namespace, reconnect: { initialDelayMs: 20, maxDelayMs: 100 }, ...extra });
  opened.push(pod);
  return pod;
}
const freshNs = () => `sdk-${Math.random().toString(36).slice(2, 10)}`;

afterEach(() => {
  for (const p of opened.splice(0)) p.close();
});

/** Collects listener snapshots and lets a test wait for a condition. */
function collector<T>() {
  const items: T[] = [];
  let wake: (() => void) | null = null;
  return {
    items,
    push: (x: T) => {
      items.push(x);
      wake?.();
    },
    async until(pred: (items: T[]) => boolean, ms = 3000): Promise<void> {
      const deadline = Date.now() + ms;
      while (!pred(items)) {
        if (Date.now() > deadline) throw new Error(`timed out; got ${items.length} snapshots`);
        await new Promise<void>((r) => {
          wake = r;
          setTimeout(r, 50);
        });
      }
    },
  };
}

describe.each(["json", "msgpack"] as const)("CRUD and queries (%s)", (encoding) => {
  test("set / get / merge / delete", async () => {
    const pod = client(freshNs(), { encoding });
    const ref = pod.collection("users").doc("u1");
    expect((await ref.get()).exists()).toBe(false);

    const w1 = await ref.set({ name: "Ann", profile: { age: 30, city: "Lahore" } });
    expect(w1).toEqual({ version: 1, created: true });
    const w2 = await ref.set({ profile: { age: 31 } }, { merge: true });
    expect(w2).toEqual({ version: 2, created: false });

    const snap = await ref.get();
    expect(snap.version).toBe(2);
    expect(snap.data()).toEqual({ name: "Ann", profile: { age: 31, city: "Lahore" } });
    expect(snap.get("profile.city")).toBe("Lahore");

    await ref.set({ only: true });
    expect((await ref.get()).data()).toEqual({ only: true });

    await ref.collection("posts").doc("p1").set({ t: 1 });
    await ref.delete();
    expect((await ref.get()).exists()).toBe(false);
    // Deleting a document removes its sub-collections too.
    expect((await ref.collection("posts").doc("p1").get()).exists()).toBe(false);
  });

  test("where / orderBy / limit / select / pagination / mutateMany", async () => {
    const pod = client(freshNs(), { encoding });
    const people = pod.collection("people");
    await Promise.all(
      Array.from({ length: 7 }, (_, i) =>
        people.doc(`p${i}`).set({ age: 20 + i, team: i % 2 ? "odd" : "even", tags: i < 3 ? ["new"] : [] }),
      ),
    );

    const q = people.where("age", "GREATER_EQUAL", 22).orderBy("age", "desc");
    const all = await q.get();
    expect(all.docs.map((d) => d.id)).toEqual(["p6", "p5", "p4", "p3", "p2"]);

    const page1 = await q.limit(2).get();
    expect(page1.docs.map((d) => d.id)).toEqual(["p6", "p5"]);
    expect(page1.hasMore).toBe(true);
    const page2 = await q.limit(2).startAfter(page1.nextCursor!).get();
    expect(page2.docs.map((d) => d.id)).toEqual(["p4", "p3"]);
    const page3 = await q.limit(2).startAfter(page2.docs.at(-1)!).get();
    expect(page3.docs.map((d) => d.id)).toEqual(["p2"]);
    expect(page3.hasMore).toBe(false);

    const projected = await people.where("tags", "ARRAY.IN", "new").select("age").orderBy("age").get();
    expect(projected.docs.map((d) => d.data())).toEqual([{ age: 20 }, { age: 21 }, { age: 22 }]);

    const changed = await people.where("team", "EQUAL", "odd").mutateMany({ set: { flag: true }, remove: ["tags"] });
    expect(changed).toBe(3);
    const p1 = (await people.doc("p1").get()).data();
    expect(p1).toEqual({ age: 21, team: "odd", flag: true });

    await expect(people.orderBy("age").startAfter("missing").get()).rejects.toMatchObject({ code: "INVALID_CURSOR" });
    expect(await people.delete()).toBe(7);
    expect((await people.get()).empty).toBe(true);
  });
});

describe("namespaces", () => {
  test("the same path in two namespaces is two documents", async () => {
    const a = client(freshNs());
    const b = client(freshNs());
    await a.doc("shared/x").set({ owner: "a" });
    expect((await b.doc("shared/x").get()).exists()).toBe(false);
    expect((await b.collection("shared").get()).size).toBe(0);
    await b.doc("shared/x").set({ owner: "b" });
    expect((await a.doc("shared/x").get()).get("owner")).toBe("a");
  });

  test("listeners only see commits in their namespace", async () => {
    const a = client(freshNs());
    const b = client(freshNs());
    const seen = collector<QuerySnapshot>();
    a.collection("feed").onSnapshot(seen.push);
    await seen.until((s) => s.length === 1);
    await b.doc("feed/1").set({ x: 1 });
    await a.doc("feed/2").set({ x: 2 });
    await seen.until((s) => s.length === 2);
    expect(seen.items[1]!.docs.map((d) => d.id)).toEqual(["2"]);
  });
});

describe("indexes", () => {
  test("create / list / use / drop", async () => {
    const pod = client(freshNs());
    const posts = pod.collection("posts");
    await posts.doc("a").set({ author: "x", n: 1 });
    await posts.doc("b").set({ author: "y", n: 2 });
    const created = await pod.createIndex("posts", ["author"]);
    expect(created.created).toBe(true);
    expect(created.documentsIndexed).toBe(2);
    expect((await pod.createIndex("posts", [{ field: "author", mode: "VALUE" }])).created).toBe(false);
    expect((await pod.listIndexes()).map((i) => i.id)).toContain(created.index.id);

    const res = await posts.where("author", "EQUAL", "y").get();
    expect(res.docs.map((d) => d.id)).toEqual(["b"]);
    expect(res.metadata.index).toBe(created.index.id);

    await pod.dropIndex(created.index.id);
    expect((await pod.listIndexes()).map((i) => i.id)).not.toContain(created.index.id);
    expect((await posts.where("author", "EQUAL", "y").get()).metadata.index).toBeNull();
  });
});

describe("real-time", () => {
  test("query listener: initial snapshot then ordered live changes", async () => {
    const ns = freshNs();
    const listenerPod = client(ns);
    const writer = client(ns);
    await writer.doc("tasks/b").set({ rank: 2 });
    const seen = collector<QuerySnapshot>();
    listenerPod.collection("tasks").orderBy("rank").onSnapshot(seen.push);
    await seen.until((s) => s.length === 1);
    expect(seen.items[0]!.docs.map((d) => d.id)).toEqual(["b"]);

    await writer.doc("tasks/a").set({ rank: 1 });
    await seen.until((s) => s.at(-1)!.size === 2);
    await writer.doc("tasks/a").set({ rank: 3 });
    await seen.until((s) => s.at(-1)!.docs[0]!.id === "b" && s.at(-1)!.docs[1]!.get("rank") === 3);
    await writer.doc("tasks/b").delete();
    await seen.until((s) => s.at(-1)!.size === 1);

    const last = seen.items.at(-1)!;
    expect(last.docs.map((d) => d.id)).toEqual(["a"]);
    expect(last.docChanges().map((c) => [c.type, c.doc.id])).toEqual([["removed", "b"]]);
    expect((await writer.collection("tasks").orderBy("rank").get()).docs.map((d) => d.id)).toEqual(["a"]);
  });

  test("document listener", async () => {
    const ns = freshNs();
    const pod = client(ns);
    const seen = collector<DocumentSnapshot>();
    const stop = pod.doc("cfg/main").onSnapshot(seen.push);
    await seen.until((s) => s.length === 1);
    expect(seen.items[0]!.exists()).toBe(false);
    await client(ns).doc("cfg/main").set({ v: 1 });
    await seen.until((s) => s.at(-1)!.exists());
    stop();
    await client(ns).doc("cfg/main").set({ v: 2 });
    await new Promise((r) => setTimeout(r, 150));
    expect(seen.items.at(-1)!.get("v")).toBe(1);
  });

  test("resume after a forced disconnect delivers only the missed changes", async () => {
    const ns = freshNs();
    const frames: string[] = [];
    // Records every server frame so the test can see the resumed handshake.
    class SpySocket extends WebSocket {
      constructor(url: string) {
        super(url);
        this.addEventListener("message", (e) => frames.push(String(e.data)));
      }
    }
    const pod = client(ns, { WebSocket: SpySocket });
    const writer = client(ns);
    for (const id of ["a", "b", "c"]) await writer.doc(`items/${id}`).set({ id });

    const seen = collector<QuerySnapshot>();
    pod.collection("items").onSnapshot(seen.push);
    await seen.until((s) => s.length === 1 && s[0]!.size === 3);

    pod._dropConnection();
    // Changes while the listener is disconnected.
    await writer.doc("items/b").delete();
    await writer.doc("items/d").set({ id: "d" });
    await writer.doc("items/a").set({ id: "a", edited: true });

    await seen.until((s) => s.at(-1)!.docs.map((d) => d.id).join() === "a,c,d" && s.at(-1)!.docs[0]!.get("edited") === true);
    const last = seen.items.at(-1)!;
    expect(last.docChanges().map((c) => [c.type, c.doc.id]).sort()).toEqual([
      ["added", "d"],
      ["modified", "a"],
      ["removed", "b"],
    ]);
    const parsed = frames.map((f) => JSON.parse(f));
    const resumedAcks = parsed.filter((m) => m.payload?.status === "SUBSCRIBED" && m.payload?.resumed === true);
    expect(resumedAcks).toHaveLength(1);
    expect(parsed.some((m) => m.payload?.event_type === "SYNCED")).toBe(true);
    expect(parsed.some((m) => m.payload?.event_type === "RESET")).toBe(false);
  });
});
