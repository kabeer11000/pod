// Do not modify this file, this is copied from protocol/defination.ts
/* ==========================================================================
   Pod wire protocol. See ./README for semantics.

   Connection:  ws://<host>:<port>/webchannel?ns=<namespace>&t=<token>
     ns  namespace (scope) for the whole connection, [A-Za-z0-9_-]{1,64}
     t   optional HS256 JWT (see README "Authentication")
   ========================================================================== */

export type IFilterOperations =
  | "EQUAL"
  | "GREATER"
  | "LESSER"
  | "GREATER_EQUAL"
  | "LESSER_EQUAL"
  | "ARRAY.IN"
  | "ARRAY.NOT_IN"
  | "ARRAY.EQUALS"
  | "ARRAY.ELEMENT_AT";

export type IOrderByDirections = "ASCENDING" | "DESCENDING";
export type IDocumentVersion = number;

export type ISubscriptionEventType =
  | "SNAPSHOT"
  | "ADDED"
  | "MODIFIED"
  | "REMOVED"
  | "RESET"
  | "SYNCED";

export type IErrorCode =
  | "MALFORMED_REQUEST"
  | "UNKNOWN_TYPE"
  | "INVALID_ARGUMENT"
  | "INVALID_PATH"
  | "INVALID_CURSOR"
  | "UNAUTHENTICATED"
  | "PERMISSION_DENIED"
  | "NOT_FOUND"
  | "ALREADY_EXISTS"
  | "DOCUMENT_TOO_LARGE"
  | "UNAVAILABLE"
  | "INTERNAL"
  | "DATA_CORRUPTION";

/* ==========================================================================
   Base Envelopes & Discriminators
   ========================================================================== */

export interface IAckMetaGeneric {
  type: "ACK";
  request_id: string;
  timestamp: string;
  status: number; // 200, 201, 400, 404, etc.
}

export interface IErrorResponse {
  meta: {
    type: "ERROR";
    request_id: string;
    timestamp: string;
    status: number;
  };
  payload: {
    code: IErrorCode;
    message: string;
    details?: Record<string, any>;
  };
}

export interface IFilter {
  field: string;
  op: IFilterOperations;
  value: any;
}

export interface IOrderBy {
  field: string;
  direction?: IOrderByDirections; // default ASCENDING
}

/* ==========================================================================
   CRUD & Mutation Operations
   ========================================================================== */

export interface IReadRequest {
  meta: { type: "READ"; request_id: string };
  payload: { path: string };
}

export interface IReadResponse {
  meta: IAckMetaGeneric;
  payload: {
    document: {
      path: string;
      data: Record<string, any> | null;
      _v?: IDocumentVersion | null;
    };
  };
}

export interface IQueryRequest {
  meta: { type: "QUERY"; request_id: string };
  payload: {
    path: string; // Odd path (Collection)
    project?: string[]; // Optional field projection
    where?: IFilter[];
    orderBy?: IOrderBy[];
    limit?: number;
    cursor?: string;
  };
}

export interface IQueryResponse {
  meta: IAckMetaGeneric;
  payload: {
    documents: {
      path: string;
      data: Record<string, any>;
      _v?: IDocumentVersion;
    }[];
    has_more: boolean;
    next_cursor: string | null;
    // How the server executed the query (diagnostics).
    plan: {
      index: string | null; // id of the secondary index used, null = collection scan
      documents_scanned: number;
    };
  };
}

export interface IWriteRequest {
  meta: { type: "WRITE"; request_id: string };
  payload: {
    path: string; // Even path (Document)
    merge: boolean;
    data: Record<string, any>;
  };
}

export interface IWriteResponse {
  meta: IAckMetaGeneric;
  payload: {
    path: string;
    _v?: IDocumentVersion;
  };
}

export interface IDeleteRequest {
  meta: { type: "DELETE"; request_id: string };
  payload: { path: string }; // Even = Document, Odd = Collection
}

export interface IDeleteResponse {
  meta: IAckMetaGeneric;
  payload: {
    target: "COLLECTION" | "DOCUMENT";
    path: string;
    affected_count: number;
  };
}

export interface IMutateManyRequest {
  meta: { type: "MUTATE_MANY"; request_id: string };
  payload: {
    path: string; // Odd path (Collection)
    where?: IFilter[];
    remove?: string[]; // Field paths to strip across matched docs
    set?: Record<string, any>; // Field updates to merge across matched docs
  };
}

export interface IMutateManyResponse {
  meta: IAckMetaGeneric;
  payload: {
    path: string;
    affected_count: number;
  };
}

/* ==========================================================================
   Secondary indexes (require an admin token unless no rules are configured)
   ========================================================================== */

export interface IIndexField {
  field: string;
  // VALUE indexes the field's value; ARRAY_CONTAINS indexes each element of
  // an array field (serves ARRAY.IN filters). At most one per index.
  mode?: "VALUE" | "ARRAY_CONTAINS";
}

export interface IIndexDefinition {
  id: string; // deterministic, derived from collection + fields
  collection: string; // collection id (last path segment), e.g. "posts"
  fields: IIndexField[];
}

export interface ICreateIndexRequest {
  meta: { type: "CREATE_INDEX"; request_id: string };
  payload: { collection: string; fields: IIndexField[] };
}

export interface ICreateIndexResponse {
  meta: IAckMetaGeneric; // 201 created, 200 already existed
  payload: { index: IIndexDefinition; documents_indexed: number };
}

export interface IDropIndexRequest {
  meta: { type: "DROP_INDEX"; request_id: string };
  payload: { id: string };
}

export interface IDropIndexResponse {
  meta: IAckMetaGeneric;
  payload: { id: string; status: "DROPPED" };
}

export interface IListIndexesRequest {
  meta: { type: "LIST_INDEXES"; request_id: string };
  payload: {};
}

export interface IListIndexesResponse {
  meta: IAckMetaGeneric;
  payload: { indexes: IIndexDefinition[] };
}

/* ==========================================================================
   Subscription System
   ========================================================================== */

/**
 * Client initiates a real-time listener on a Document (even path)
 * or a Collection Query (odd path).
 *
 * resume_token: the token of the last event the client fully applied (an
 * event with commit_complete = true). If the server still retains the commit
 * log from that point, it answers with resumed = true and sends only the
 * deltas between the client's view and the current view, followed by SYNCED.
 * Otherwise it sends RESET followed by a full SNAPSHOT.
 */
export interface ISubscribeRequest {
  meta: { type: "SUBSCRIBE"; request_id: string };
  payload: {
    subscription_id: string;
    path: string;
    project?: string[];
    where?: IFilter[];
    orderBy?: IOrderBy[];
    limit?: number;
    resume_token?: string; // For seamless reconnects
  };
}

/**
 * Server confirms the subscription registration.
 */
export interface ISubscribeResponse {
  meta: IAckMetaGeneric;
  payload: {
    subscription_id: string;
    status: "SUBSCRIBED";
    resumed: boolean;
  };
}

/**
 * Server pushes real-time events (Initial Snapshot or Live Deltas).
 */
export interface ISubscriptionEventPush {
  meta: {
    type: "EVENT";
    subscription_id: string;
    timestamp: string;
  };
  payload: {
    event_type: ISubscriptionEventType;
    resume_token: string;
    // true on the last event produced by a commit for this subscription:
    // the client's view is consistent with resume_token after applying it.
    commit_complete: boolean;

    // Populated on event_type === "SNAPSHOT"
    snapshot?: {
      is_synced: boolean;
      documents: {
        index: number;
        path: string;
        data: Record<string, any>;
        _v?: IDocumentVersion;
      }[];
    };

    // Populated on "ADDED" | "MODIFIED" | "REMOVED"
    delta?: {
      path: string;
      old_index: number | null; // For array sorting transitions
      new_index: number | null;
      document?: {
        path: string;
        data: Record<string, any>;
        _v?: IDocumentVersion;
      };
    };
  };
}

/**
 * Client ACKs receipt of an EVENT frame.
 */
export interface IClientEventAck {
  meta: {
    type: "ACK";
    subscription_id: string;
    timestamp: string;
  };
  payload: {
    resume_token: string;
  };
}

/**
 * Client tears down an active listener.
 */
export interface IUnsubscribeRequest {
  meta: { type: "UNSUBSCRIBE"; request_id: string };
  payload: {
    subscription_id: string;
  };
}

export interface IUnsubscribeResponse {
  meta: IAckMetaGeneric;
  payload: {
    subscription_id: string;
    status: "UNSUBSCRIBED";
  };
}

/* ==========================================================================
   Master Union Types
   ========================================================================== */

export type IRequestEvent =
  | IReadRequest
  | IQueryRequest
  | IWriteRequest
  | IDeleteRequest
  | IMutateManyRequest
  | ICreateIndexRequest
  | IDropIndexRequest
  | IListIndexesRequest
  | ISubscribeRequest
  | IUnsubscribeRequest
  | IClientEventAck;

export type IResponseEvent =
  | IReadResponse
  | IQueryResponse
  | IWriteResponse
  | IDeleteResponse
  | IMutateManyResponse
  | ICreateIndexResponse
  | IDropIndexResponse
  | IListIndexesResponse
  | ISubscribeResponse
  | IUnsubscribeResponse
  | ISubscriptionEventPush
  | IErrorResponse;
