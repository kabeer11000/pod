// Shared WebSocket test client for the e2e suites.
import { expect } from "bun:test";
import { decode, encode } from "@msgpack/msgpack";

// Opens a connection and returns the close code the server ends it with
// (connections refused for auth/namespace reasons are accepted, then closed).
export async function refusal(host: string, query: Record<string, string>): Promise<{ code: number; reason: string }> {
  const qs = new URLSearchParams(query).toString();
  const ws = new WebSocket(`ws://${host}/webchannel?${qs}`);
  return await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("connection was not refused")), 3000);
    ws.onclose = (e) => {
      clearTimeout(timer);
      resolve({ code: e.code, reason: e.reason });
    };
  });
}

// Untyped wire frames: tests assert on their exact shape.
export type Msg = any;

export class Client {
  ws!: WebSocket;
  binary: boolean;
  pending = new Map<string, (m: Msg) => void>();
  events = new Map<string, Msg[]>(); // subscription_id -> EVENT frames
  inbox: Msg[] = []; // every frame, in arrival order
  waiters: Array<() => void> = [];
  seq = 0;
  closeCode: number | undefined;
  closeReason = "";

  constructor(binary = false) {
    this.binary = binary;
  }

  // Connects to ws://<host>/webchannel?<query>; rejects if the server closes
  // the connection before or right after opening it.
  static async connect(host: string, query: Record<string, string> = {}, binary = false): Promise<Client> {
    const c = new Client(binary);
    const qs = new URLSearchParams(query).toString();
    c.ws = new WebSocket(`ws://${host}/webchannel` + (qs ? `?${qs}` : ""));
    c.ws.binaryType = "arraybuffer";
    c.ws.onmessage = (e) => c.onFrame(e.data);
    await new Promise<void>((resolve, reject) => {
      c.ws.onopen = () => resolve();
      c.ws.onerror = (e) => reject(e);
      c.ws.onclose = (e) => reject(new Error(`closed before open (${e.code})`));
    });
    c.ws.onclose = (e) => {
      c.closeCode = e.code;
      c.closeReason = e.reason;
      c.notify();
    };
    return c;
  }

  onFrame(data: string | ArrayBuffer) {
    const m: Msg = typeof data === "string" ? JSON.parse(data) : decode(new Uint8Array(data));
    this.inbox.push(m);
    if (m.meta?.type === "EVENT") {
      const list = this.events.get(m.meta.subscription_id) ?? [];
      list.push(m);
      this.events.set(m.meta.subscription_id, list);
    } else {
      const r = this.pending.get(m.meta?.request_id);
      if (r) {
        this.pending.delete(m.meta.request_id);
        r(m);
      }
    }
    this.notify();
  }

  notify() {
    const w = this.waiters;
    this.waiters = [];
    w.forEach((f) => f());
  }

  sendRaw(packet: Msg) {
    this.ws.send(this.binary ? encode(packet) : JSON.stringify(packet));
  }

  request(type: string, payload: Msg): Promise<Msg> {
    const request_id = `r${++this.seq}`;
    const p = new Promise<Msg>((resolve) => this.pending.set(request_id, resolve));
    this.sendRaw({ meta: { type, request_id }, payload });
    return p;
  }

  async waitFor(pred: () => boolean, ms = 3000) {
    const deadline = Date.now() + ms;
    while (!pred()) {
      if (Date.now() > deadline) throw new Error("timed out waiting");
      await new Promise<void>((r) => {
        this.waiters.push(r);
        setTimeout(r, 50);
      });
    }
  }

  async eventsFor(sub: string, count: number): Promise<Msg[]> {
    await this.waitFor(() => (this.events.get(sub)?.length ?? 0) >= count);
    return this.events.get(sub)!;
  }

  close() {
    this.ws.close();
  }

  // Resolves with the close code once the server closes the connection.
  async closed(ms = 3000): Promise<number> {
    await this.waitFor(() => this.closeCode !== undefined, ms);
    return this.closeCode!;
  }
}

// Applies EVENT frames like a real client would.
export function applyEvents(events: Msg[]): string[] {
  let docs: Msg[] = [];
  for (const e of events) {
    const p = e.payload;
    switch (p.event_type) {
      case "RESET":
        docs = [];
        break;
      case "SNAPSHOT":
        docs = p.snapshot.documents.map((d: Msg) => d);
        break;
      case "REMOVED":
        expect(docs[p.delta.old_index].path).toBe(p.delta.path);
        docs.splice(p.delta.old_index, 1);
        break;
      case "ADDED":
        docs.splice(p.delta.new_index, 0, p.delta.document);
        break;
      case "MODIFIED":
        expect(docs[p.delta.old_index].path).toBe(p.delta.path);
        docs.splice(p.delta.old_index, 1);
        docs.splice(p.delta.new_index, 0, p.delta.document);
        break;
    }
  }
  return docs.map((d) => `${d.path}:${JSON.stringify(d.data)}`);
}

