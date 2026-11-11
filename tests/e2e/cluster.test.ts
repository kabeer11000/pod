// Cluster, authentication and security rules, end to end. Needs the processes
// started by run-cluster.sh (skipped otherwise):
//
//   tests/e2e/run-cluster.sh /path/to/build/pod
import { describe, expect, test } from "bun:test";
import { createHmac } from "node:crypto";
import { readFileSync } from "node:fs";
import { applyEvents, Client, refusal, type Msg } from "./client";

const env = process.env;
const ROUTER = env.POD_CLUSTER_ROUTER ?? "";
const ROUTER_BAD = env.POD_CLUSTER_ROUTER_BAD ?? "";
const NODE_A = env.POD_CLUSTER_NODE_A ?? "";
const NODE_B = env.POD_CLUSTER_NODE_B ?? "";
const SECRET = env.POD_AUTH_SECRET_TEST ?? "";

const b64 = (s: string | Buffer) => Buffer.from(s).toString("base64url");
function token(claims: Record<string, unknown>, secret = SECRET): string {
  const head = b64(JSON.stringify({ alg: "HS256", typ: "JWT" }));
  const body = b64(JSON.stringify(claims));
  const sig = createHmac("sha256", secret).update(`${head}.${body}`).digest();
  return `${head}.${body}.${b64(sig)}`;
}
const now = () => Math.floor(Date.now() / 1000);
const alice = () => token({ sub: "alice", exp: now() + 3600 });
const bob = () => token({ sub: "bob" });
const admin = () => token({ sub: "root", admin: true });

const as = (host: string, ns: string, t?: string, binary = false) =>
  Client.connect(host, t ? { ns, t } : { ns }, binary);

async function routerStatus(): Promise<string> {
  return await (await fetch(`http://${ROUTER}/`)).text();
}

async function waitUntil(pred: () => Promise<boolean>, ms = 10000) {
  const deadline = Date.now() + ms;
  while (!(await pred())) {
    if (Date.now() > deadline) throw new Error("timed out");
    await Bun.sleep(100);
  }
}

// Which node stores each namespace, discovered through the data itself.
async function placement(namespaces: string[]): Promise<Map<string, "a" | "b">> {
  const out = new Map<string, "a" | "b">();
  for (const ns of namespaces) {
    const c = await as(ROUTER, ns, alice());
    const w = await c.request("WRITE", { path: "public/marker", merge: false, data: { ns } });
    expect(w.meta.status).toBeLessThan(300);
    c.close();
    const found: ("a" | "b")[] = [];
    for (const [name, host] of [["a", NODE_A], ["b", NODE_B]] as const) {
      const d = await as(host, ns);
      const r = await d.request("READ", { path: "public/marker" });
      if (r.payload.document.data) found.push(name);
      d.close();
    }
    expect(found.length).toBe(1); // exactly one owner
    out.set(ns, found[0]);
  }
  return out;
}

describe.skipIf(!ROUTER)("pods router", () => {
  test("namespaces are spread over both nodes and stay put", async () => {
    const names = Array.from({ length: 24 }, (_, i) => `spread${i}`);
    const where = await placement(names);
    const owners = new Set(where.values());
    expect(owners).toEqual(new Set(["a", "b"]));
    // The same namespace always routes to the same node.
    const again = await placement(names.slice(0, 6));
    for (const [ns, node] of again) expect(node).toBe(where.get(ns)!);
  });

  test("CRUD, queries and subscriptions through the router (JSON and MessagePack)", async () => {
    for (const binary of [false, true]) {
      const ns = binary ? "routed_bin" : "routed_json";
      const sub = await as(ROUTER, ns, alice(), binary);
      const w = await as(ROUTER, ns, bob(), binary);
      let r = await sub.request("SUBSCRIBE", { subscription_id: "s", path: "public", orderBy: [{ field: "n" }] });
      expect(r.payload.status).toBe("SUBSCRIBED");
      for (let i = 0; i < 5; i++) {
        r = await w.request("WRITE", { path: `public/p${i}`, merge: false, data: { n: 5 - i } });
        expect(r.meta.status).toBe(201);
      }
      await w.request("DELETE", { path: "public/p0" });
      const ev = await sub.eventsFor("s", 7); // SNAPSHOT + 5 ADDED + REMOVED
      expect(applyEvents(ev)).toEqual(['public/p4:{"n":1}', 'public/p3:{"n":2}', 'public/p2:{"n":3}', 'public/p1:{"n":4}']);
      r = await w.request("QUERY", { path: "public", where: [{ field: "n", op: "GREATER", value: 2 }] });
      expect(r.payload.documents.map((d: Msg) => d.path).sort()).toEqual(["public/p1", "public/p2"]);
      sub.close();
      w.close();
    }
  });

  test("a router with the wrong cluster secret never links", async () => {
    const r = await refusal(ROUTER_BAD, { ns: "anything" });
    expect(r.code).toBe(1013);
    const status = await (await fetch(`http://${ROUTER_BAD}/`)).text();
    expect(status).not.toContain(" up,");
  });

  test("losing a node closes its clients with 1013; they resume after it returns", async () => {
    const where = await placement(Array.from({ length: 16 }, (_, i) => `fail${i}`));
    const onB = [...where].find(([, n]) => n === "b")![0];
    const onA = [...where].find(([, n]) => n === "a")![0];

    const sub = await as(ROUTER, onB, alice());
    const unaffected = await as(ROUTER, onA, alice());
    await sub.request("SUBSCRIBE", { subscription_id: "f", path: "public" });
    let ev = await sub.eventsFor("f", 1);
    const w = await as(ROUTER, onB, alice());
    await w.request("WRITE", { path: "public/before", merge: false, data: { v: 1 } });
    ev = await sub.eventsFor("f", 2);
    let resume = "";
    for (const e of ev) if (e.payload.commit_complete) resume = e.payload.resume_token;

    process.kill(Number(readFileSync(env.POD_CLUSTER_B_PIDFILE!, "utf8")), "SIGKILL");
    expect(await sub.closed()).toBe(1013);
    expect(await w.closed()).toBe(1013);
    // Clients of the other node are untouched.
    const r = await unaffected.request("READ", { path: "public/marker" });
    expect(r.payload.document.data).toEqual({ ns: onA });

    // The restart loop brings b back; the router relinks on its own.
    await waitUntil(async () => (await routerStatus()).match(/ up,/g)?.length === 2);
    const w2 = await as(ROUTER, onB, alice());
    await w2.request("WRITE", { path: "public/after", merge: false, data: { v: 2 } });
    const sub2 = await as(ROUTER, onB, alice());
    const ack = await sub2.request("SUBSCRIBE", { subscription_id: "f", path: "public", resume_token: resume });
    expect(ack.payload.status).toBe("SUBSCRIBED");
    const n = ack.payload.resumed ? 2 : 3; // ADDED+SYNCED, or RESET+SNAPSHOT (log lost)
    const ev2 = await sub2.eventsFor("f", n - 1);
    const view = ack.payload.resumed ? applyEvents([...ev, ...ev2]) : applyEvents(ev2);
    expect(view).toEqual(['public/after:{"v":2}', 'public/before:{"v":1}', `public/marker:{"ns":"${onB}"}`]);
    for (const c of [unaffected, w2, sub2]) c.close();
  });
});

describe.skipIf(!ROUTER)("authentication", () => {
  for (const [label, host] of [["router", () => ROUTER], ["node", () => NODE_A]] as const) {
    test(`${label}: bad, expired, forged and wrong-namespace tokens are refused`, async () => {
      expect((await refusal(host(), { ns: "x", t: "not.a.token" })).code).toBe(4401);
      expect((await refusal(host(), { ns: "x", t: token({ sub: "u", exp: now() - 3600 }) })).code).toBe(4401);
      expect((await refusal(host(), { ns: "x", t: token({ sub: "u" }, "wrong-secret") })).code).toBe(4401);
      expect((await refusal(host(), { ns: "x", t: token({ sub: "u", ns: ["y", "z"] }) })).code).toBe(4403);
      expect((await refusal(host(), { ns: "bad ns" })).code).toBe(4400);
      const ok = await as(host(), "y", token({ sub: "u", ns: ["y", "z"] }));
      expect((await ok.request("READ", { path: "public/x" })).meta.status).toBe(404);
      ok.close();
    });
  }

  test("a token expiring mid-connection makes requests fail with UNAUTHENTICATED", async () => {
    const c = await as(ROUTER, "expiry", token({ sub: "alice", exp: now() + 1 }));
    await Bun.sleep(2200);
    const r = await c.request("READ", { path: "users/alice" });
    expect(r.meta.type).toBe("ERROR");
    expect(r.payload.code).toBe("UNAUTHENTICATED");
    expect(r.meta.status).toBe(401);
    c.close();
  });
});

describe.skipIf(!ROUTER)("security rules", () => {
  const denied = (r: Msg) => {
    expect(r.meta.type).toBe("ERROR");
    expect(r.meta.status).toBe(403);
    expect(r.payload.code).toBe("PERMISSION_DENIED");
  };

  test("owner-only documents", async () => {
    const a = await as(ROUTER, "rules", alice());
    const b = await as(ROUTER, "rules", bob());
    const anon = await as(ROUTER, "rules");
    denied(await anon.request("WRITE", { path: "users/alice", merge: false, data: { x: 1 } }));
    denied(await b.request("WRITE", { path: "users/alice", merge: false, data: { x: 1 } }));
    expect((await a.request("WRITE", { path: "users/alice", merge: false, data: { x: 1 } })).meta.status).toBe(201);
    expect((await a.request("READ", { path: "users/alice" })).payload.document.data).toEqual({ x: 1 });
    denied(await b.request("READ", { path: "users/alice" }));
    denied(await b.request("READ", { path: "users/nobody" })); // missing docs too
    denied(await b.request("DELETE", { path: "users/alice" }));
    denied(await b.request("DELETE", { path: "users/nobody" }));
    denied(await anon.request("READ", { path: "private/x" })); // no rule = denied
    // Public data is readable by anyone, writable by signed-in users.
    denied(await anon.request("WRITE", { path: "public/p", merge: false, data: {} }));
    expect((await b.request("WRITE", { path: "public/p", merge: false, data: {} })).meta.status).toBe(201);
    expect((await anon.request("READ", { path: "public/p" })).meta.status).toBe(200);
    for (const c of [a, b, anon]) c.close();
  });

  test("queries, bulk writes and subscriptions only see permitted documents", async () => {
    const a = await as(ROUTER, "notes", alice());
    const b = await as(ROUTER, "notes", bob());
    await a.request("SUBSCRIBE", { subscription_id: "mine", path: "notes" });
    for (const [c, owner, id] of [[a, "alice", "a1"], [a, "alice", "a2"], [b, "bob", "b1"]] as const)
      expect((await c.request("WRITE", { path: `notes/${id}`, merge: false, data: { owner, n: 0 } })).meta.status).toBe(201);
    // Writing a note owned by someone else is refused.
    denied(await b.request("WRITE", { path: "notes/b2", merge: false, data: { owner: "alice" } }));
    denied(await b.request("WRITE", { path: "notes/a1", merge: true, data: { n: 5 } }));

    let r = await a.request("QUERY", { path: "notes" });
    expect(r.payload.documents.map((d: Msg) => d.path)).toEqual(["notes/a1", "notes/a2"]);

    // MUTATE_MANY over documents she cannot write fails atomically.
    denied(await a.request("MUTATE_MANY", { path: "notes", set: { n: 1 } }));
    r = await b.request("READ", { path: "notes/b1" });
    expect(r.payload.document.data).toEqual({ owner: "bob", n: 0 });
    r = await a.request("MUTATE_MANY", { path: "notes", where: [{ field: "owner", op: "EQUAL", value: "alice" }], set: { n: 1 } });
    expect(r.payload.affected_count).toBe(2);

    // Bob's change is invisible to Alice's subscription; hers arrive.
    await b.request("WRITE", { path: "notes/b1", merge: true, data: { n: 9 } });
    await a.request("WRITE", { path: "notes/a3", merge: false, data: { owner: "alice", n: 3 } });
    await a.request("READ", { path: "notes/a3" }); // barrier
    await Bun.sleep(100);
    const ev = a.events.get("mine")!;
    expect(ev.every((e: Msg) => !JSON.stringify(e.payload).includes("notes/b1"))).toBe(true);
    expect(applyEvents(ev)).toEqual(['notes/a1:{"n":1,"owner":"alice"}', 'notes/a2:{"n":1,"owner":"alice"}', 'notes/a3:{"n":3,"owner":"alice"}']);

    // A document listener needs read permission up front.
    r = await b.request("SUBSCRIBE", { subscription_id: "x", path: "notes/a1" });
    denied(r);
    a.close();
    b.close();
  });

  test("index management requires an admin token", async () => {
    const a = await as(ROUTER, "idx", alice());
    denied(await a.request("CREATE_INDEX", { collection: "notes", fields: [{ field: "owner" }] }));
    denied(await a.request("LIST_INDEXES", {}));
    const root = await as(ROUTER, "idx", admin());
    expect((await root.request("CREATE_INDEX", { collection: "notes", fields: [{ field: "owner" }] })).meta.status).toBe(201);
    expect((await root.request("LIST_INDEXES", {})).payload.indexes.length).toBe(1);
    expect((await root.request("DROP_INDEX", { id: "notes:owner" })).payload.status).toBe("DROPPED");
    a.close();
    root.close();
  });
});
