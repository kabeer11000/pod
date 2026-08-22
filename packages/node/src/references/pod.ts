import { ImplementableStorageAdapter } from "storage/adapters";
interface _IWireEvent {
    data: Object;
    error: 0 | 1;
    meta: {
        action: "get-ack" | "set-ack" | "insert-ack";
        request_id: string;
    };
    path: string;
    status: number;
}
async function parseJSON(d: string): Promise<_IWireEvent> {
    return JSON.parse(d);
}
// async function parseMessagePack(d: string): Promise<Object> {
//     return JSON.parse(d);
// }
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
    p: Object;
    resolve: (d: Object) => void;
    reject: (d: Object) => void;
}
// insert handler will eventuallly become set handler as it makes more sense then a upsert insert
interface IOutbountEvent {
    type: "get" | "insert";
    d: Object;
}
export class PodReference {
    // Pending event queue, this queue holds reference Ids,
    // for dispatched events which have not recieved acknoldgements
    // first parameter is the id as a string
    _pending_events = new Map<string, _IPendingEventValue>();
    _connection: Promise<WebSocket> | null = null;
    _config: _PodConstructorConfiguration | null = null;
    constructor(configuration: _PodConstructorConfiguration) {
        if (!configuration) throw new Error("Pod: Configuration is invalid.");
        this._config = configuration;
    }
    async _ensure_connection() {
        this._connection = new Promise((resolve, reject) => {
            if (this._config === null)
                return reject(new Error("Pod: Configuration is invalid."));
            const ws = new WebSocket(
                `${this._config.server.uri}?t=${this._config.server.token}&v=1.0`,
            );
            ws.addEventListener("open", () => resolve(ws));
            ws.addEventListener("message", (event) => {
                this._downstream_handler(event.data);
            });
            ws.addEventListener("close", () => {
                this._connection = null;
                // promise is voided;
                // resubscribe all watch events after opening a new connection.
            });
        });
        this._connection.addEventListener("close", () => {});
    }
    // Replace generic Object with actual type sent by the backend
    async _downstream_handler(d: string) {
        const parsedEvent = await parseJSON(d);
        const pendingEvent = this._pending_events.get(
            parsedEvent.meta.request_id,
        );
        // if we recieve a unwanted acknoldgement ignore
        if (!pendingEvent) return console.log("[Pod]: Un-wanted acknoledgement recieved", parsedEvent);
        if (parsedEvent.error) pendingEvent.reject(parsedEvent);
        if (!parsedEvent.error) pendingEvent.resolve(parsedEvent);
        return this._pending_events.delete(parsedEvent.meta.request_id);
    }
    async _upstream_queue(e: IOutbountEvent) {
        this._pending_events.set(e., value)
    }
}
