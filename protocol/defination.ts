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
  | "RESET";

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
    code: string;
    message: string;
    details?: Record<string, any>;
  };
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
      _v?: IDocumentVersion;
    };
  };
}

export interface IQueryRequest {
  meta: { type: "QUERY"; request_id: string };
  payload: {
    path: string; // Odd path (Collection)
    project?: string[]; // Optional field projection
    where?: {
      field: string;
      op: IFilterOperations;
      value: any;
    }[];
    orderBy?: {
      field: string;
      direction: IOrderByDirections;
    }[];
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
    where?: {
      field: string;
      op: IFilterOperations;
      value: any;
    }[];
    delete?: string[]; // Field paths to strip across matched docs
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
   Subscription System
   ========================================================================== */

/**
 * Client initiates a real-time listener on a Document (even path)
 * or a Collection Query (odd path).
 */
export interface ISubscribeRequest {
  meta: { type: "SUBSCRIBE"; request_id: string };
  payload: {
    subscription_id: string;
    path: string;
    project?: string[];
    where?: {
      field: string;
      op: IFilterOperations;
      value: any;
    }[];
    orderBy?: {
      field: string;
      direction: IOrderByDirections;
    }[];
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
  | ISubscribeRequest
  | IUnsubscribeRequest
  | IClientEventAck;

export type IResponseEvent =
  | IReadResponse
  | IQueryResponse
  | IWriteResponse
  | IDeleteResponse
  | IMutateManyResponse
  | ISubscribeResponse
  | IUnsubscribeResponse
  | ISubscriptionEventPush
  | IErrorResponse;
