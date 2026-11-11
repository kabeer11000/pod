// Pod benchmark: closed-loop load generator over the WebSocket wire protocol.
//
//   bun bench/pod-bench.ts
//
// Environment:
//   POD_URL      ws://localhost:3000/webchannel
//   CONNECTIONS  concurrent client connections            (default 32)
//   DEPTH        in-flight requests per connection         (default 8)
//   DURATION_MS  duration of each phase                    (default 5000)
//   SUBSCRIBERS  listeners in the fan-out phase            (default 100)
//
// Each run uses a fresh namespace so results are not affected by older data.
// Note: the client is a single Bun process; on small machines the load
// generator itself can become the bottleneck before the server does.

const URL_BASE = process.env.POD_URL ?? "ws://localhost:3000/webchannel";
const CONNECTIONS = Number(process.env.CONNECTIONS ?? 32);
const DEPTH = Number(process.env.DEPTH ?? 8);
const DURATION_MS = Number(process.env.DURATION_MS ?? 5000);
const SUBSCRIBERS = Number(process.env.SUBSCRIBERS ?? 100);
const NS = `bench-${Date.now().toString(36)}`;
const PRELOAD = 10_000; // documents created before the read/query phases

type Msg = any;

class Conn {
  ws!: WebSocket;
  private pending = new Map<string, (m: Msg) => void>();
  private n = 0;
  onEvent?: (m: Msg) => void;

  static async open(): Promise<Conn> {
    const c = new Conn();
    c.ws = new WebSocket(`${URL_BASE}?ns=${NS}`);
    await new Promise<void>((res, rej) => {
      c.ws.onopen = () => res();
      c.ws.onerror = () => rej(new Error(`cannot connect to ${URL_BASE}`));
    });
    c.ws.onmessage = (e) => {
      const m = JSON.parse(String(e.data));
      if (m.meta.type === "EVENT") return c.onEvent?.(m);
      const r = c.pending.get(m.meta.request_id);
      if (r) {
        c.pending.delete(m.meta.request_id);
        r(m);
      }
    };
    return c;
  }

  request(type: string, payload: object): Promise<Msg> {
    const request_id = String(++this.n);
    return new Promise((resolve) => {
      this.pending.set(request_id, resolve);
      this.ws.send(JSON.stringify({ meta: { type, request_id }, payload }));
    });
  }

  close() {
    this.ws.close();
  }
}

function percentile(sorted: number[], p: number) {
  if (sorted.length === 0) return NaN;
  return sorted[Math.min(sorted.length - 1, Math.floor((p / 100) * sorted.length))]!;
}

interface Result {
  name: string;
  ops: number;
  errors: number;
  seconds: number;
  lat: number[];
}

function report(r: Result) {
  const s = r.lat.sort((a, b) => a - b);
  const fmt = (n: number) => n.toFixed(2).padStart(7);
  console.log(
    `${r.name.padEnd(26)} ${Math.round(r.ops / r.seconds)
      .toLocaleString("en-US")
      .padStart(9)} ops/s   p50 ${fmt(percentile(s, 50))} ms   p95 ${fmt(percentile(s, 95))} ms   p99 ${fmt(
      percentile(s, 99),
    )} ms   max ${fmt(s[s.length - 1] ?? NaN)} ms` + (r.errors ? `   errors ${r.errors}` : ""),
  );
}

// Runs `op` from every connection with DEPTH requests in flight for DURATION_MS.
async function phase(name: string, conns: Conn[], op: (c: Conn, i: number) => Promise<Msg>): Promise<Result> {
  const lat: number[] = [];
  let ops = 0;
  let errors = 0;
  let counter = 0;
  const end = performance.now() + DURATION_MS;
  const start = performance.now();
  const worker = async (c: Conn) => {
    while (performance.now() < end) {
      const t0 = performance.now();
      const r = await op(c, counter++);
      lat.push(performance.now() - t0);
      ops++;
      if (r.meta.type === "ERROR") errors++;
    }
  };
  await Promise.all(conns.flatMap((c) => Array.from({ length: DEPTH }, () => worker(c))));
  const res = { name, ops, errors, seconds: (performance.now() - start) / 1000, lat };
  report(res);
  return res;
}

const payload = (i: number) => ({
  title: `Item ${i}`,
  owner: `user-${i % 100}`,
  score: i % 1000,
  tags: ["alpha", "beta", i % 2 ? "odd" : "even"],
  body: "x".repeat(120),
});

console.log(
  `Pod benchmark → ${URL_BASE}  namespace=${NS}  connections=${CONNECTIONS}  depth=${DEPTH}  phase=${DURATION_MS}ms\n`,
);

const conns = await Promise.all(Array.from({ length: CONNECTIONS }, () => Conn.open()));

// Preload a collection for read/query phases.
{
  const t0 = performance.now();
  const per = Math.ceil(PRELOAD / CONNECTIONS);
  await Promise.all(
    conns.map(async (c, ci) => {
      for (let k = 0; k < per; k += DEPTH) {
        const batch = [];
        for (let j = k; j < Math.min(k + DEPTH, per); j++) {
          const i = ci * per + j;
          if (i < PRELOAD) batch.push(c.request("WRITE", { path: `items/i${i}`, merge: false, data: payload(i) }));
        }
        await Promise.all(batch);
      }
    }),
  );
  // 50 small sub-collections of 40 documents each for the small-query phase.
  await Promise.all(
    conns.map(async (_, ci) => {
      for (let k = ci; k < 50 * 40; k += CONNECTIONS)
        await conns[ci]!.request("WRITE", {
          path: `small/c${k % 50}/docs/d${k}`,
          merge: false,
          data: { ...payload(k), n: k },
        });
    }),
  );
  console.log(`preloaded ${(PRELOAD + 2000).toLocaleString("en-US")} documents in ${((performance.now() - t0) / 1000).toFixed(2)} s\n`);
}

await phase("WRITE (insert)", conns, (c, i) =>
  c.request("WRITE", { path: `inserts/d${i}`, merge: false, data: payload(i) }),
);
await phase("WRITE (merge update)", conns, (c, i) =>
  c.request("WRITE", { path: `items/i${i % PRELOAD}`, merge: true, data: { score: i, touched: true } }),
);
await phase("READ", conns, (c, i) => c.request("READ", { path: `items/i${(i * 7919) % PRELOAD}` }));
await phase("QUERY (small coll, limit 20)", conns, (c, i) =>
  c.request("QUERY", {
    path: `small/c${i % 50}/docs`,
    where: [{ field: "n", op: "GREATER_EQUAL", value: 0 }],
    orderBy: [{ field: "n", direction: "DESCENDING" }],
    limit: 20,
  }),
);

// Query over the full 10k-document collection (full scan; no secondary indexes yet).
{
  const qConns = conns.slice(0, Math.min(4, conns.length));
  await phase("QUERY (10k scan, limit 20)", qConns, (c, i) =>
    c.request("QUERY", {
      path: "items",
      where: [{ field: "owner", op: "EQUAL", value: `user-${i % 100}` }],
      orderBy: [{ field: "score", direction: "DESCENDING" }],
      limit: 20,
    }),
  );
}

// Subscription fan-out: SUBSCRIBERS listeners on one collection, one writer.
{
  const subConns = await Promise.all(Array.from({ length: SUBSCRIBERS }, () => Conn.open()));
  const sent = new Map<number, number>(); // write number -> send time
  const lat: number[] = [];
  let delivered = 0;
  for (const [k, c] of subConns.entries()) {
    c.onEvent = (m) => {
      const t = m.payload.event_type;
      if (t !== "ADDED" && t !== "MODIFIED") return;
      const t0 = sent.get(m.payload.delta.document.data.i);
      if (t0 !== undefined) lat.push(performance.now() - t0);
      delivered++;
    };
    await c.request("SUBSCRIBE", { subscription_id: `s${k}`, path: "feed", orderBy: [{ field: "n" }], limit: 50 });
  }
  const writer = conns[0]!;
  const writes = 500;
  const start = performance.now();
  for (let i = 0; i < writes; i++) {
    const path = `feed/f${i % 50}`;
    sent.set(i, performance.now());
    await writer.request("WRITE", { path, merge: false, data: { n: i % 50, i } });
  }
  const deadline = performance.now() + 10_000;
  while (delivered < writes * SUBSCRIBERS && performance.now() < deadline) await Bun.sleep(10);
  const seconds = (performance.now() - start) / 1000;
  report({ name: `FAN-OUT (${SUBSCRIBERS} subs) events`, ops: delivered, errors: 0, seconds, lat });
  console.log(`  ${writes} writes → ${delivered.toLocaleString("en-US")} / ${(writes * SUBSCRIBERS).toLocaleString("en-US")} events delivered (latency = write sent → event received)`);
  subConns.forEach((c) => c.close());
}

conns.forEach((c) => c.close());
process.exit(0);
