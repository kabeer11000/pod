import {
    type _IRequestEventWithoutSubscription,
    type _IResponseEventWithoutSubscription,
} from "network/ws";
import { ImplementableStorageAdapter } from "storage/adapters";
import { decode as decodeMsgPack } from "@msgpack/msgpack";
import { uuidv7 } from "vendor/uuid7";

function parseJSON(d: string): _IResponseEventWithoutSubscription {
    return JSON.parse(d);
}

function parseMessagePack(
    d: ArrayBuffer | Uint8Array,
): _IResponseEventWithoutSubscription {
    const bytes = d instanceof ArrayBuffer ? new Uint8Array(d) : d;
    return decodeMsgPack(bytes) as _IResponseEventWithoutSubscription;
}

interface _PodConstructorConfiguration {
    server: {
        uri: string;
        token: string;
    };
    config: {
        upgradeToBackgroundSync: boolean;
    };
    cache: {
        adapter: ImplementableStorageAdapter;
    };
}

interface _IPendingEventValue {
    _packet: Object;
    resolve: (d: _IResponseEventWithoutSubscription) => void; // fixed: responses, not requests
    reject: (d: _IResponseEventWithoutSubscription) => void; // fixed: responses, not requests
}

export class PodReference {
    _pending_events = new Map<string, _IPendingEventValue>();
    _connection: Promise<WebSocket> | null = null;
    _config: _PodConstructorConfiguration | null = null;

    constructor(configuration: _PodConstructorConfiguration) {
        if (!configuration) throw new Error("Pod: Configuration is invalid.");
        this._config = configuration;
    }

    // Lazily opens a connection, and reuses the in-flight/open one if a
    // connection attempt is already underway — prevents duplicate sockets
    // when multiple _request() calls race before the first one connects.
    _ensure_connection(): Promise<WebSocket> {
        if (this._connection) return this._connection;

        this._connection = new Promise((resolve, reject) => {
            if (this._config === null) {
                return reject(new Error("Pod: Configuration is invalid."));
            }
            const ws = new WebSocket(
                `${this._config.server.uri}?t=${this._config.server.token}&v=1.0`,
            );
            ws.binaryType = "arraybuffer"; // ensures binary frames arrive as ArrayBuffer, not Blob

            ws.addEventListener("open", () => resolve(ws));
            ws.addEventListener("error", (event) => reject(event));
            ws.addEventListener("message", (event) =>
                this._default_handler(event.data),
            );
            ws.addEventListener("close", () => {
                this._connection = null;
                // TODO: resubscribe all active watch/subscribe listeners
                // against the new connection once reconnect logic exists.
            });
        });

        return this._connection;
    }

    // Accepts either a JSON string frame or a binary MessagePack frame,
    // and routes the parsed response to the matching pending request.
    _default_handler(d: string | ArrayBuffer | Blob) {
        void this._handle_raw_message(d);
    }

    async _handle_raw_message(d: string | ArrayBuffer | Blob) {
        let parsedEvent: _IResponseEventWithoutSubscription;

        if (typeof d === "string") {
            parsedEvent = parseJSON(d);
        } else if (d instanceof ArrayBuffer) {
            parsedEvent = parseMessagePack(d);
        } else {
            // Blob — only reachable if ws.binaryType wasn't set to "arraybuffer"
            const buf = await d.arrayBuffer();
            parsedEvent = parseMessagePack(buf);
        }

        const pendingEvent = this._pending_events.get(
            parsedEvent.meta.request_id,
        );
        if (!pendingEvent) {
            console.warn(
                "[Pod]: Unwanted acknowledgement received",
                parsedEvent,
            );
            return;
        }

        this._pending_events.delete(parsedEvent.meta.request_id);

        if (parsedEvent.meta.type === "ERROR") {
            pendingEvent.reject(parsedEvent);
        } else if (parsedEvent.meta.type === "ACK") {
            pendingEvent.resolve(parsedEvent);
        } else {
            // exhaustiveness guard — surfaces loudly if a new meta.type
            // is ever added to the response union without being handled here
            const _exhaustive: never = parsedEvent.meta;
            console.error("[Pod]: Unhandled response meta.type", _exhaustive);
        }
    }

    // Actually sends a request over the wire and returns a promise that
    // resolves/rejects once the matching response arrives.
    async _request(
        packet: Omit<_IRequestEventWithoutSubscription, "meta"> & {
            meta: Omit<_IRequestEventWithoutSubscription["meta"], "request_id">;
        },
    ): Promise<_IResponseEventWithoutSubscription> {
        const ws = await this._ensure_connection();
        const request_id = uuidv7();

        const fullPacket = {
            ...packet,
            meta: { ...packet.meta, request_id },
        } as _IRequestEventWithoutSubscription;

        return new Promise((resolve, reject) => {
            this._pending_events.set(request_id, {
                _packet: fullPacket,
                resolve,
                reject,
            });
            ws.send(JSON.stringify(fullPacket));

            // optional: timeout so a dropped/unanswered request doesn't
            // leak forever in _pending_events
            setTimeout(() => {
                if (this._pending_events.has(request_id)) {
                    this._pending_events.delete(request_id);
                    reject(new Error(`Pod: request ${request_id} timed out`));
                }
            }, 100_000);
        });
    }
}
