import type { ReconnectOptions } from "./backoff";
import { Connection, type WebSocketConstructor, type WireEncoding } from "./connection";
import { PodError } from "./errors";
import type {
  ICreateIndexResponse,
  IDropIndexResponse,
  IIndexDefinition,
  IIndexField,
  IListIndexesResponse,
  IRequestEvent,
} from "./network/ws/index";
import { collectionSegments, documentSegments } from "./path";
import { specToPayload } from "./query";
import { Listener, type ListenerHost } from "./realtime/listener";
import { canonicalJson, type ViewChange, type WireDocument } from "./realtime/view";
import { CollectionReference, DocumentReference, type ErrorCallback, type Query, type Unsubscribe } from "./references";
import { DocumentSnapshot, QuerySnapshot, snapshotFromWire, type DocumentChange } from "./snapshot";
import type { StorageAdapter } from "./storage/adapters";

export interface PodOptions {
  /** Server endpoint, e.g. "ws://localhost:3000/webchannel". */
  uri: string;
  /** Namespace (scope) for every operation; the server default when omitted. */
  namespace?: string;
  /** HS256 JWT sent on connect. */
  token?: string;
  /** Called before every (re)connect; takes precedence over `token`. */
  getToken?: () => Promise<string | null>;
  /** Wire encoding. Default "json"; "msgpack" sends binary frames. */
  encoding?: WireEncoding;
  reconnect?: ReconnectOptions;
  /** Per-request timeout. Default 30 s. */
  requestTimeoutMs?: number;
  /**
   * Persists listener views and resume tokens so a new listener (or a
   * reloaded page) shows cached data at once and resumes with only the
   * missed changes. Off by default.
   */
  cache?: StorageAdapter;
  /** WebSocket implementation; defaults to the global one. */
  WebSocket?: WebSocketConstructor;
}

export interface CreateIndexResult {
  readonly index: IIndexDefinition;
  /** False when an identical index already existed. */
  readonly created: boolean;
  readonly documentsIndexed: number;
}

type RequestType = IRequestEvent["meta"]["type"];

export type ListenSpec =
  | {
      kind: "document";
      ref: DocumentReference;
      onNext: (s: DocumentSnapshot) => void;
      onError?: ErrorCallback;
    }
  | {
      kind: "query";
      query: Query;
      onNext: (s: QuerySnapshot) => void;
      onError?: ErrorCallback;
    };

const NAMESPACE = /^[A-Za-z0-9_-]{1,64}$/;

/** A client for one Pod server and namespace. Connects lazily. */
export class Pod {
  private readonly connection: Connection;
  private readonly listeners = new Set<Listener>();
  private readonly routes = new Map<string, Listener>();

  constructor(private readonly options: PodOptions) {
    if (!options || typeof options.uri !== "string")
      throw new PodError("INVALID_ARGUMENT", "Pod options need a uri.", 400);
    let url: URL;
    try {
      url = new URL(options.uri);
    } catch {
      throw new PodError("INVALID_ARGUMENT", `Invalid uri '${options.uri}'.`, 400);
    }
    if (url.protocol !== "ws:" && url.protocol !== "wss:")
      throw new PodError("INVALID_ARGUMENT", "uri must use ws:// or wss://.", 400);
    // The server rejects a bad namespace at upgrade time, which a browser
    // cannot tell apart from a network failure: validate it here.
    if (options.namespace !== undefined && !NAMESPACE.test(options.namespace))
      throw new PodError("INVALID_ARGUMENT", "namespace must be 1-64 characters of [A-Za-z0-9_-].", 400);
    this.connection = new Connection(
      {
        uri: options.uri,
        ...(options.namespace !== undefined ? { namespace: options.namespace } : {}),
        ...(options.token !== undefined ? { token: options.token } : {}),
        ...(options.getToken ? { getToken: options.getToken } : {}),
        ...(options.WebSocket ? { WebSocket: options.WebSocket } : {}),
        encoding: options.encoding ?? "json",
        reconnect: options.reconnect ?? {},
        requestTimeoutMs: options.requestTimeoutMs ?? 30_000,
      },
      {
        onEvent: (e) => this.routes.get(e.meta.subscription_id)?.onEvent(e),
        onOpen: () => {
          for (const l of [...this.listeners]) l.onReconnect();
        },
        onDisconnect: () => {
          for (const l of this.listeners) l.onDisconnect();
        },
        wantsConnection: () => this.listeners.size > 0,
        onFatal: (error) => {
          for (const l of [...this.listeners]) l.onFatal(error);
        },
      },
    );
  }

  get namespace(): string | undefined {
    return this.options.namespace;
  }

  collection(path: string): CollectionReference {
    return new CollectionReference(this, collectionSegments(path));
  }

  doc(path: string): DocumentReference {
    return new DocumentReference(this, documentSegments(path));
  }

  /** Creates (or returns the existing) secondary index on a collection id. */
  async createIndex(collection: string, fields: (string | IIndexField)[]): Promise<CreateIndexResult> {
    const normalized = fields.map((f) => (typeof f === "string" ? { field: f } : f));
    const res = await this._request<ICreateIndexResponse>("CREATE_INDEX", { collection, fields: normalized });
    return {
      index: res.payload.index,
      created: res.meta.status === 201,
      documentsIndexed: res.payload.documents_indexed,
    };
  }

  async dropIndex(id: string): Promise<void> {
    await this._request<IDropIndexResponse>("DROP_INDEX", { id });
  }

  async listIndexes(): Promise<IIndexDefinition[]> {
    const res = await this._request<IListIndexesResponse>("LIST_INDEXES", {});
    return res.payload.indexes;
  }

  /** Stops every listener, rejects pending requests and closes the socket. */
  close(): void {
    for (const l of [...this.listeners]) l.stop();
    this.listeners.clear();
    this.routes.clear();
    this.connection.close();
  }

  /** For tests: drop the socket as if the network failed. */
  _dropConnection(): void {
    this.connection.dropConnection();
  }

  /** @internal */
  _request<T>(type: RequestType, payload: object): Promise<T> {
    return this.connection.request(type, payload) as Promise<T>;
  }

  /** @internal */
  _listen(spec: ListenSpec): Unsubscribe {
    if (this.connection.isClosed) throw new PodError("CANCELLED", "The Pod client has been closed.");
    const host: ListenerHost = {
      request: (type, payload) => this.connection.request(type, payload),
      sendIfOpen: (packet) => this.connection.sendIfOpen(packet),
      unsubscribe: (id) => {
        if (this.connection.isOpen) this.connection.request("UNSUBSCRIBE", { subscription_id: id }).catch(() => {});
      },
      route: (id, listener) => {
        if (listener) this.routes.set(id, listener);
        else this.routes.delete(id);
      },
      ...(this.options.cache
        ? { cache: this.options.cache, cacheKey: (payload: object) => this.cacheKey(payload) }
        : {}),
    };
    const payload =
      spec.kind === "document"
        ? { path: spec.ref.path }
        : (specToPayload(spec.query.path, spec.query.spec) as Record<string, unknown>);

    const emit = (view: readonly WireDocument[], changes: ViewChange[], fromCache: boolean) => {
      const metadata = { fromCache };
      if (spec.kind === "document") {
        const d = view[0];
        deliver(() => spec.onNext(new DocumentSnapshot(spec.ref, d ? d.data : null, d?._v, metadata)));
        return;
      }
      const docs = view.map((d) => snapshotFromWire(this, d, metadata));
      const docChanges: DocumentChange[] = changes.map((c) => ({
        type: c.type,
        doc: snapshotFromWire(this, c.doc, metadata),
        oldIndex: c.oldIndex,
        newIndex: c.newIndex,
      }));
      deliver(() => spec.onNext(new QuerySnapshot(spec.query, docs, metadata, false, null, docChanges)));
    };
    const fail = (error: PodError) => {
      this.listeners.delete(listener);
      if (spec.onError) deliver(() => spec.onError!(error));
      else console.error("[pod] listener failed:", error);
    };

    const listener = new Listener(host, payload, emit, fail);
    this.listeners.add(listener);
    listener.start().catch((e: unknown) => {
      // Only a throwing cache adapter can get here.
      fail(e instanceof PodError ? e : new PodError("INTERNAL", String(e)));
    });
    return () => {
      listener.stop();
      this.listeners.delete(listener);
    };
  }

  private async cacheKey(payload: object): Promise<string> {
    let token = this.options.token ?? null;
    if (this.options.getToken) token = await this.options.getToken().catch(() => null);
    return canonicalJson({
      uri: this.options.uri,
      ns: this.options.namespace ?? null,
      uid: tokenSubject(token),
      payload,
    });
  }
}

/** The (unverified) "sub" claim of a JWT, used only to partition the cache. */
export function tokenSubject(token: string | null): string | null {
  const part = token?.split(".")[1];
  if (!part) return null;
  try {
    const b64 = part.replace(/-/g, "+").replace(/_/g, "/");
    const json = JSON.parse(new TextDecoder().decode(Uint8Array.from(atob(b64), (c) => c.charCodeAt(0))));
    return typeof json?.sub === "string" ? json.sub : null;
  } catch {
    return null;
  }
}

/** Runs a user callback without letting its exception break event handling. */
function deliver(fn: () => void): void {
  try {
    fn();
  } catch (e) {
    queueMicrotask(() => {
      throw e;
    });
  }
}
