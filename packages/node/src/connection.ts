// WebSocket connection management: lazy connect, request/response
// correlation, queueing while (re)connecting, timeouts, and reconnection with
// exponential backoff.
import { decode as decodeMsgPack, encode as encodeMsgPack } from "@msgpack/msgpack";
import { backoffDelay, type ReconnectOptions } from "./backoff";
import { PodError, type PodErrorCode } from "./errors";
import type { IErrorResponse, IRequestEvent, IResponseEvent, ISubscriptionEventPush } from "./network/ws/index";
import { uuidv7 } from "./vendor/uuid7/index";

export type WireEncoding = "json" | "msgpack";

export interface ConnectionOptions {
  uri: string;
  namespace?: string;
  token?: string;
  getToken?: () => Promise<string | null>;
  encoding: WireEncoding;
  reconnect: ReconnectOptions;
  requestTimeoutMs: number;
  /** WebSocket implementation; defaults to the global one. */
  WebSocket?: WebSocketConstructor;
}

export type WebSocketConstructor = new (url: string) => WebSocket;

export interface ConnectionHooks {
  /** A subscription EVENT frame arrived. */
  onEvent(event: ISubscriptionEventPush): void;
  /** A socket became open (first connect or reconnect). */
  onOpen(): void;
  /** The socket was lost; a reconnect may follow. */
  onDisconnect(): void;
  /** True while something (e.g. live listeners) needs the connection kept up. */
  wantsConnection(): boolean;
  /** The server refused the connection for good (bad namespace / token). */
  onFatal(error: PodError): void;
}

// Close codes the server uses for authentication / namespace failures.
// Reconnecting with the same parameters cannot succeed.
const FATAL_CLOSE_CODES: Record<number, PodErrorCode> = {
  4400: "INVALID_ARGUMENT",
  4401: "UNAUTHENTICATED",
  4403: "PERMISSION_DENIED",
};

type RequestType = IRequestEvent["meta"]["type"];
type AckResponse = Exclude<IResponseEvent, IErrorResponse | ISubscriptionEventPush>;

interface PendingRequest {
  requestId: string;
  frame: string | Uint8Array;
  resolve: (m: AckResponse) => void;
  reject: (e: PodError) => void;
  timer: ReturnType<typeof setTimeout>;
  sent: boolean;
}

type State = "idle" | "connecting" | "open" | "closed";

export class Connection {
  private ws: WebSocket | null = null;
  private state: State = "idle";
  private attempt = 0;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private readonly pending = new Map<string, PendingRequest>();
  private readonly queue: PendingRequest[] = [];

  constructor(
    private readonly options: ConnectionOptions,
    private readonly hooks: ConnectionHooks,
  ) {}

  get isOpen(): boolean {
    return this.state === "open";
  }

  get isClosed(): boolean {
    return this.state === "closed";
  }

  /** Sends a request; resolves with the ACK frame, rejects with PodError. */
  request<T extends AckResponse = AckResponse>(type: RequestType, payload: object): Promise<T> {
    if (this.state === "closed") return Promise.reject(new PodError("CANCELLED", "The Pod client has been closed."));
    const requestId = uuidv7();
    const frame = this.encode({ meta: { type, request_id: requestId }, payload });
    return new Promise<T>((resolve, reject) => {
      const entry: PendingRequest = {
        requestId,
        frame,
        resolve: resolve as (m: AckResponse) => void,
        reject,
        sent: false,
        timer: setTimeout(() => {
          this.pending.delete(requestId);
          const q = this.queue.indexOf(entry);
          if (q !== -1) this.queue.splice(q, 1);
          reject(new PodError("DEADLINE_EXCEEDED", `${type} did not complete within ${this.options.requestTimeoutMs} ms.`));
        }, this.options.requestTimeoutMs),
      };
      this.pending.set(requestId, entry);
      if (this.state === "open" && this.ws) {
        this.ws.send(frame);
        entry.sent = true;
      } else {
        this.queue.push(entry);
        this.ensureConnected();
      }
    });
  }

  /** Fire-and-forget frame (client event ACKs). Dropped when not connected. */
  sendIfOpen(packet: object): void {
    if (this.state === "open" && this.ws) this.ws.send(this.encode(packet));
  }

  /** Opens the socket if it is not open or opening already. */
  ensureConnected(): void {
    if (this.state !== "idle" || this.reconnectTimer) return;
    void this.connect();
  }

  close(): void {
    this.shutdown(new PodError("CANCELLED", "The Pod client has been closed."));
  }

  private shutdown(error: PodError): void {
    if (this.state === "closed") return;
    this.state = "closed";
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
    const ws = this.ws;
    this.ws = null;
    if (ws) {
      ws.onopen = ws.onmessage = ws.onerror = ws.onclose = null;
      try {
        ws.close(1000, "client closed");
      } catch {
        // ignore
      }
    }
    for (const p of this.pending.values()) {
      clearTimeout(p.timer);
      p.reject(error);
    }
    this.pending.clear();
    this.queue.length = 0;
  }

  /** Test hook: drops the socket as if the network failed. */
  dropConnection(): void {
    if (this.ws && this.state === "open") this.ws.close(4000, "dropped for testing");
  }

  private encode(packet: object): string | Uint8Array {
    return this.options.encoding === "msgpack" ? encodeMsgPack(packet) : JSON.stringify(packet);
  }

  private async connect(): Promise<void> {
    this.state = "connecting";
    let token: string | null | undefined = this.options.token;
    if (this.options.getToken) {
      try {
        token = await this.options.getToken();
      } catch {
        // Connecting anonymously instead would silently change permissions.
        if (this.state === "connecting") this.onSocketGone();
        return;
      }
    }
    if (this.state !== "connecting") return; // closed while fetching the token

    let ws: WebSocket;
    try {
      ws = new (this.options.WebSocket ?? WebSocket)(this.buildUrl(token ?? null));
    } catch {
      this.onSocketGone();
      return;
    }
    ws.binaryType = "arraybuffer";
    this.ws = ws;
    ws.onopen = () => {
      if (this.ws !== ws) return;
      this.state = "open";
      this.attempt = 0;
      for (const entry of this.queue.splice(0)) {
        if (!this.pending.has(entry.requestId)) continue; // timed out meanwhile
        ws.send(entry.frame);
        entry.sent = true;
      }
      this.hooks.onOpen();
    };
    ws.onmessage = (ev: MessageEvent) => {
      if (this.ws !== ws) return;
      this.onFrame(ev.data);
    };
    ws.onclose = (ev: CloseEvent) => {
      if (this.ws !== ws) return;
      this.ws = null;
      const wasOpen = this.state === "open";
      const fatal = FATAL_CLOSE_CODES[ev.code];
      if (fatal) {
        const error = new PodError(fatal, ev.reason || `Connection refused (close code ${ev.code}).`, ev.code);
        this.shutdown(error);
        this.hooks.onFatal(error);
        return;
      }
      this.onSocketGone(wasOpen);
    };
    ws.onerror = () => {
      // onclose always follows; nothing to do here.
    };
  }

  private buildUrl(token: string | null): string {
    const url = new URL(this.options.uri);
    if (this.options.namespace) url.searchParams.set("ns", this.options.namespace);
    if (token) url.searchParams.set("t", token);
    url.searchParams.set("v", "1.0");
    return url.toString();
  }

  private onSocketGone(wasOpen = false): void {
    if (this.state === "closed") return;
    this.state = "idle";
    if (wasOpen) this.hooks.onDisconnect();
    // In-flight requests may or may not have been applied: never retry them.
    for (const p of [...this.pending.values()]) {
      if (!p.sent) continue;
      clearTimeout(p.timer);
      this.pending.delete(p.requestId);
      p.reject(new PodError("UNAVAILABLE", "The connection was lost before a response arrived."));
    }
    if (this.queue.length > 0 || this.hooks.wantsConnection()) {
      const delay = backoffDelay(this.attempt++, this.options.reconnect);
      this.reconnectTimer = setTimeout(() => {
        this.reconnectTimer = null;
        if (this.state === "idle") void this.connect();
      }, delay);
    }
  }

  // Synchronous on purpose: frames must be handled strictly in arrival order
  // (binaryType is "arraybuffer", so no Blob needs awaiting).
  private onFrame(data: unknown): void {
    let msg: IResponseEvent;
    try {
      if (typeof data === "string") msg = JSON.parse(data) as IResponseEvent;
      else if (data instanceof ArrayBuffer) msg = decodeMsgPack(new Uint8Array(data)) as IResponseEvent;
      else if (ArrayBuffer.isView(data))
        msg = decodeMsgPack(new Uint8Array(data.buffer, data.byteOffset, data.byteLength)) as IResponseEvent;
      else return;
    } catch {
      return; // undecodable frame
    }
    if (!msg || typeof msg !== "object" || !msg.meta) return;
    if (msg.meta.type === "EVENT") {
      this.hooks.onEvent(msg as ISubscriptionEventPush);
      return;
    }
    const entry = this.pending.get(msg.meta.request_id);
    if (!entry) return;
    this.pending.delete(entry.requestId);
    clearTimeout(entry.timer);
    if (msg.meta.type === "ERROR") {
      const err = msg as IErrorResponse;
      entry.reject(new PodError(err.payload.code, err.payload.message, err.meta.status, err.payload.details));
    } else {
      entry.resolve(msg as AckResponse);
    }
  }
}
