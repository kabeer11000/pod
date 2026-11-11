// Minimal Pod wire-protocol client used by the example. Works in browsers and
// in Bun/Node (anything with a global WebSocket).
//
// It speaks the protocol in protocol/defination.ts directly; the
// @pod-db/node SDK will eventually provide the same surface.

export type FilterOp =
  | "EQUAL"
  | "GREATER"
  | "LESSER"
  | "GREATER_EQUAL"
  | "LESSER_EQUAL"
  | "ARRAY.IN"
  | "ARRAY.NOT_IN"
  | "ARRAY.EQUALS"
  | "ARRAY.ELEMENT_AT";

export interface PodDoc<T = Record<string, any>> {
  path: string;
  data: T;
  _v: number;
}

export interface QueryOptions {
  where?: { field: string; op: FilterOp; value: any }[];
  orderBy?: { field: string; direction?: "ASCENDING" | "DESCENDING" }[];
  limit?: number;
  project?: string[];
}

export class PodError extends Error {
  constructor(
    readonly code: string,
    readonly status: number,
    message: string,
  ) {
    super(`${code} (${status}): ${message}`);
  }
}

export type ChangeType = "SNAPSHOT" | "ADDED" | "MODIFIED" | "REMOVED" | "RESET";
export type SnapshotListener<T> = (docs: PodDoc<T>[], change: ChangeType) => void;

interface Pending {
  resolve: (m: any) => void;
  reject: (e: unknown) => void;
}

interface ActiveSubscription {
  docs: PodDoc<any>[];
  listener: SnapshotListener<any>;
}

export class PodClient {
  private ws!: WebSocket;
  private pending = new Map<string, Pending>();
  private subs = new Map<string, ActiveSubscription>();
  private nextId = 0;
  onClose?: () => void;

  private constructor() {}

  /** Connects to `url` (ws://host:port/webchannel) scoped to `namespace`. */
  static async connect(url: string, namespace: string): Promise<PodClient> {
    const c = new PodClient();
    const full = `${url}?ns=${encodeURIComponent(namespace)}`;
    c.ws = new WebSocket(full);
    await new Promise<void>((resolve, reject) => {
      c.ws.onopen = () => resolve();
      c.ws.onerror = () => reject(new Error(`Could not connect to ${full}`));
    });
    c.ws.onmessage = (e) => c.onFrame(String(e.data));
    c.ws.onclose = () => {
      for (const p of c.pending.values()) p.reject(new Error("connection closed"));
      c.pending.clear();
      c.onClose?.();
    };
    return c;
  }

  close() {
    this.ws.close();
  }

  private onFrame(raw: string) {
    const msg = JSON.parse(raw);
    if (msg.meta.type === "EVENT") return this.onEvent(msg);
    const p = this.pending.get(msg.meta.request_id);
    if (!p) return;
    this.pending.delete(msg.meta.request_id);
    if (msg.meta.type === "ERROR") p.reject(new PodError(msg.payload.code, msg.meta.status, msg.payload.message));
    else p.resolve(msg);
  }

  private onEvent(msg: any) {
    const sub = this.subs.get(msg.meta.subscription_id);
    if (!sub) return;
    const p = msg.payload;
    const t: ChangeType = p.event_type;
    if (t === "RESET") sub.docs = [];
    else if (t === "SNAPSHOT") sub.docs = p.snapshot.documents.map(({ index, ...d }: any) => d);
    else if (t === "REMOVED") sub.docs.splice(p.delta.old_index, 1);
    else if (t === "ADDED") sub.docs.splice(p.delta.new_index, 0, p.delta.document);
    else if (t === "MODIFIED") {
      sub.docs.splice(p.delta.old_index, 1);
      sub.docs.splice(p.delta.new_index, 0, p.delta.document);
    }
    // Acknowledge receipt so the server knows how far this listener got.
    this.ws.send(
      JSON.stringify({
        meta: { type: "ACK", subscription_id: msg.meta.subscription_id, timestamp: new Date().toISOString() },
        payload: { resume_token: p.resume_token },
      }),
    );
    sub.listener([...sub.docs], t);
  }

  private request(type: string, payload: object): Promise<any> {
    const request_id = `${Date.now().toString(36)}-${++this.nextId}`;
    return new Promise((resolve, reject) => {
      this.pending.set(request_id, { resolve, reject });
      this.ws.send(JSON.stringify({ meta: { type, request_id }, payload }));
    });
  }

  async read<T = any>(path: string): Promise<PodDoc<T> | null> {
    const r = await this.request("READ", { path });
    return r.meta.status === 404 ? null : r.payload.document;
  }

  async write(path: string, data: object, merge = false): Promise<number> {
    const r = await this.request("WRITE", { path, merge, data });
    return r.payload._v;
  }

  async delete(path: string): Promise<number> {
    const r = await this.request("DELETE", { path });
    return r.payload.affected_count;
  }

  async query<T = any>(
    path: string,
    opts: QueryOptions & { cursor?: string } = {},
  ): Promise<{ docs: PodDoc<T>[]; hasMore: boolean; nextCursor: string | null }> {
    const r = await this.request("QUERY", { path, ...opts });
    return { docs: r.payload.documents, hasMore: r.payload.has_more, nextCursor: r.payload.next_cursor };
  }

  async mutateMany(
    path: string,
    opts: { where?: QueryOptions["where"]; set?: Record<string, any>; remove?: string[] },
  ): Promise<number> {
    const r = await this.request("MUTATE_MANY", { path, ...opts });
    return r.payload.affected_count;
  }

  /** Live query. Returns an unsubscribe function. */
  async subscribe<T = any>(
    path: string,
    opts: QueryOptions,
    listener: SnapshotListener<T>,
  ): Promise<() => Promise<void>> {
    const subscription_id = `sub-${++this.nextId}`;
    this.subs.set(subscription_id, { docs: [], listener });
    try {
      await this.request("SUBSCRIBE", { subscription_id, path, ...opts });
    } catch (e) {
      this.subs.delete(subscription_id);
      throw e;
    }
    return async () => {
      this.subs.delete(subscription_id);
      await this.request("UNSUBSCRIBE", { subscription_id });
    };
  }
}
