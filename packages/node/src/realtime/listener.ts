// One real-time listener: owns a server subscription and keeps the client
// view consistent across reconnects (protocol/defination.ts, ISubscribeRequest).
//
//   committed  the view as of `token` (the last event with commit_complete);
//              what the user last saw and what a resume is relative to
//   working    committed + the events of the commit in progress
//
// Every (re)subscribe uses a fresh subscription_id, so frames from an
// abandoned attempt can never be applied to the new one.
import type { PodError } from "../errors";
import type { ISubscriptionEventPush } from "../network/ws/index";
import type { StorageAdapter } from "../storage/adapters";
import { uuidv7 } from "../vendor/uuid7/index";
import { applyDelta, diffViews, ViewDesyncError, type ViewChange, type WireDocument } from "./view";

export interface ListenerHost {
  request(type: "SUBSCRIBE", payload: object): Promise<unknown>;
  /** Fire-and-forget frame; dropped when not connected. */
  sendIfOpen(packet: object): void;
  /** Best-effort UNSUBSCRIBE of a still-registered server subscription. */
  unsubscribe(subscriptionId: string): void;
  /** Routes EVENT frames for `subscriptionId` to `listener` (null = stop). */
  route(subscriptionId: string, listener: Listener | null): void;
  readonly cache?: StorageAdapter;
  /** Cache key for a subscription payload (includes server, namespace, user). */
  cacheKey?(payload: object): Promise<string>;
}

export type ListenerEmit = (view: readonly WireDocument[], changes: ViewChange[], fromCache: boolean) => void;

interface CacheEntry {
  v: 1;
  token: string;
  view: WireDocument[];
}

function isCacheEntry(x: unknown): x is CacheEntry {
  const e = x as CacheEntry | null;
  return (
    !!e &&
    e.v === 1 &&
    typeof e.token === "string" &&
    Array.isArray(e.view) &&
    e.view.every((d) => !!d && typeof d.path === "string" && !!d.data && typeof d.data === "object")
  );
}

type Delta = NonNullable<ISubscriptionEventPush["payload"]["delta"]>;

export class Listener {
  private subId: string | null = null;
  private committed: WireDocument[] = [];
  private token: string | null = null;
  private working: WireDocument[] = [];
  private changes: ViewChange[] = [];
  private emittedFromServer = false;
  private started = false;
  private stopped = false;
  // The server subscription died with the socket (requests queued while
  // disconnected are sent on the next socket and need no resubscribe).
  private lost = false;
  private key: string | null = null;

  constructor(
    private readonly host: ListenerHost,
    /** SUBSCRIBE payload without subscription_id / resume_token. */
    private readonly payload: Readonly<Record<string, unknown>>,
    private readonly emit: ListenerEmit,
    private readonly fail: (error: PodError) => void,
  ) {}

  get subscriptionId(): string | null {
    return this.subId;
  }

  get resumeToken(): string | null {
    return this.token;
  }

  async start(): Promise<void> {
    const { cache } = this.host;
    if (cache && this.host.cacheKey) {
      this.key = await this.host.cacheKey(this.payload);
      const entry = await cache.get(this.key);
      if (this.stopped) return;
      if (isCacheEntry(entry)) {
        this.committed = entry.view;
        this.token = entry.token;
        this.emit(this.committed, diffViews([], this.committed), true);
      }
    }
    if (this.stopped) return;
    this.started = true;
    this.subscribe(false);
  }

  onDisconnect(): void {
    this.lost = true;
  }

  /** A socket opened: resubscribe if the subscription was lost. */
  onReconnect(): void {
    if (this.started && !this.stopped && this.lost) this.subscribe(false);
  }

  /** The connection failed permanently. */
  onFatal(error: PodError): void {
    if (this.stopped) return;
    this.stop();
    this.fail(error);
  }

  stop(): void {
    if (this.stopped) return;
    this.stopped = true;
    if (this.subId) {
      this.host.route(this.subId, null);
      this.host.unsubscribe(this.subId);
      this.subId = null;
    }
  }

  private subscribe(replacing: boolean): void {
    if (this.subId) {
      this.host.route(this.subId, null);
      if (replacing) this.host.unsubscribe(this.subId);
    }
    const id = uuidv7();
    this.subId = id;
    this.lost = false;
    this.host.route(id, this);
    this.working = [...this.committed];
    this.changes = [];
    const payload = { ...this.payload, subscription_id: id, ...(this.token ? { resume_token: this.token } : {}) };
    this.host.request("SUBSCRIBE", payload).catch((err: PodError) => {
      if (this.stopped || this.subId !== id) return;
      switch (err.code) {
        case "UNAVAILABLE": // the connection dropped; onReconnect resubscribes
        case "CANCELLED": // the client was closed
          return;
        case "DEADLINE_EXCEEDED":
          this.subscribe(true);
          return;
        default:
          this.stop();
          this.fail(err);
      }
    });
  }

  onEvent(event: ISubscriptionEventPush): void {
    if (this.stopped || event.meta.subscription_id !== this.subId) return;
    const p = event.payload;
    try {
      switch (p.event_type) {
        case "RESET":
          this.working = [];
          break;
        case "SNAPSHOT": {
          const docs = [...(p.snapshot?.documents ?? [])]
            .sort((a, b) => a.index - b.index)
            .map(({ path, data, _v }) => ({ path, data, ...(_v !== undefined ? { _v } : {}) }));
          this.changes = diffViews(this.committed, docs);
          this.working = docs;
          break;
        }
        case "ADDED":
        case "MODIFIED":
        case "REMOVED":
          if (!p.delta) throw new ViewDesyncError(`${p.event_type} without a delta`);
          this.changes.push(applyDelta(this.working, p.event_type, p.delta as Delta));
          break;
        case "SYNCED":
          break;
      }
    } catch (e) {
      if (!(e instanceof ViewDesyncError)) throw e;
      // Our view no longer matches the server's: start over from a snapshot.
      this.token = null;
      this.subscribe(true);
      return;
    }
    if (p.commit_complete) this.commit(p.resume_token);
  }

  private commit(token: string): void {
    this.committed = [...this.working];
    this.token = token;
    this.host.sendIfOpen({
      meta: { type: "ACK", subscription_id: this.subId, timestamp: new Date().toISOString() },
      payload: { resume_token: token },
    });
    const changes = this.changes;
    this.changes = [];
    if (changes.length > 0 || !this.emittedFromServer) {
      this.emittedFromServer = true;
      this.emit(this.committed, changes, false);
    }
    if (this.host.cache && this.key) {
      const entry: CacheEntry = { v: 1, token, view: this.committed };
      void this.host.cache.set(this.key, entry);
    }
  }
}
