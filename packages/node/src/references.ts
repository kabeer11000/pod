import { PodError } from "./errors";
import type {
  IDeleteResponse,
  IMutateManyResponse,
  IQueryResponse,
  IReadResponse,
  IWriteResponse,
} from "./network/ws/index";
import { parsePath, validateFieldPath } from "./path";
import type { Pod } from "./pod";
import {
  checkCursor,
  checkLimit,
  checkProjection,
  EMPTY_SPEC,
  makeFilter,
  makeOrder,
  specToPayload,
  type OrderDirection,
  type QuerySpec,
  type WhereOperator,
} from "./query";
import { DocumentSnapshot, QuerySnapshot, snapshotFromWire, type DocumentData } from "./snapshot";
import { uuidv7 } from "./vendor/uuid7/index";

export interface SetOptions {
  /** Deep-merge into the existing document instead of replacing it. */
  merge?: boolean;
}

export interface WriteResult {
  /** The document's version (_v) after the write. */
  readonly version: number | undefined;
  /** True if the write created the document. */
  readonly created: boolean;
}

export interface MutateManyOptions {
  /** Field path -> new value, applied to every matching document. */
  set?: Record<string, unknown>;
  /** Field paths removed from every matching document (applied first). */
  remove?: string[];
}

export type Unsubscribe = () => void;
export type ErrorCallback = (error: PodError) => void;

/** A document location: an even number of path segments. */
export class DocumentReference {
  readonly type = "document" as const;
  readonly path: string;

  constructor(
    readonly pod: Pod,
    readonly segments: readonly string[],
  ) {
    this.path = segments.join("/");
  }

  get id(): string {
    return this.segments[this.segments.length - 1]!;
  }

  get parent(): CollectionReference {
    return new CollectionReference(this.pod, this.segments.slice(0, -1));
  }

  /** A sub-collection of this document. */
  collection(path: string): CollectionReference {
    return this.pod.collection(`${this.path}/${parsePath(path).join("/")}`);
  }

  isEqual(other: DocumentReference): boolean {
    return other.pod === this.pod && other.path === this.path;
  }

  async get(): Promise<DocumentSnapshot> {
    const res = await this.pod._request<IReadResponse>("READ", { path: this.path });
    const d = res.payload.document;
    return new DocumentSnapshot(this, d.data ?? null, d._v);
  }

  async set(data: DocumentData, options: SetOptions = {}): Promise<WriteResult> {
    if (data === null || typeof data !== "object" || Array.isArray(data))
      throw new PodError("INVALID_ARGUMENT", "Document data must be a plain object.", 400);
    const res = await this.pod._request<IWriteResponse>("WRITE", {
      path: this.path,
      merge: options.merge === true,
      data,
    });
    return { version: res.payload._v, created: res.meta.status === 201 };
  }

  /** Deletes the document and every sub-collection beneath it. */
  async delete(): Promise<void> {
    await this.pod._request<IDeleteResponse>("DELETE", { path: this.path });
  }

  onSnapshot(onNext: (snapshot: DocumentSnapshot) => void, onError?: ErrorCallback): Unsubscribe {
    return this.pod._listen({ kind: "document", ref: this, onNext, ...(onError ? { onError } : {}) });
  }
}

/** An immutable query over the direct documents of a collection. */
export class Query {
  constructor(
    readonly pod: Pod,
    /** Path of the queried collection. */
    readonly path: string,
    readonly spec: QuerySpec = EMPTY_SPEC,
  ) {}

  private with(changes: Partial<QuerySpec>): Query {
    return new Query(this.pod, this.path, { ...this.spec, ...changes });
  }

  where(field: string, op: WhereOperator, value: unknown): Query {
    return this.with({ where: [...this.spec.where, makeFilter(field, op, value)] });
  }

  orderBy(field: string, direction?: OrderDirection): Query {
    return this.with({ orderBy: [...this.spec.orderBy, makeOrder(field, direction)] });
  }

  limit(n: number): Query {
    return this.with({ limit: checkLimit(n) });
  }

  /** Field projection: only these (dot-notation) fields are returned. */
  select(...fields: string[]): Query {
    return this.with({ project: checkProjection(fields) });
  }

  /** Continue after a document (id or snapshot) in this query's order. */
  startAfter(cursor: string | { readonly id: string }): Query {
    return this.with({ cursor: checkCursor(cursor) });
  }

  async get(): Promise<QuerySnapshot> {
    const res = await this.pod._request<IQueryResponse>("QUERY", specToPayload(this.path, this.spec));
    const p = res.payload;
    const metadata = {
      fromCache: false,
      index: p.plan?.index ?? null,
      documentsScanned: p.plan?.documents_scanned ?? 0,
    };
    const docs = p.documents.map((d) => snapshotFromWire(this.pod, d, metadata));
    return new QuerySnapshot(this, docs, metadata, p.has_more, p.next_cursor);
  }

  /**
   * Applies `remove` then `set` to every document matching this query's
   * where() filters, atomically. Returns the number of documents changed.
   */
  async mutateMany(options: MutateManyOptions): Promise<number> {
    const { spec } = this;
    if (spec.orderBy.length || spec.limit !== undefined || spec.cursor !== undefined || spec.project)
      throw new PodError("INVALID_ARGUMENT", "mutateMany() only supports where() filters.", 400);
    options.remove?.forEach(validateFieldPath);
    Object.keys(options.set ?? {}).forEach(validateFieldPath);
    const payload: Record<string, unknown> = { path: this.path };
    if (spec.where.length) payload.where = spec.where;
    if (options.remove?.length) payload.remove = options.remove;
    if (options.set && Object.keys(options.set).length) payload.set = options.set;
    const res = await this.pod._request<IMutateManyResponse>("MUTATE_MANY", payload);
    return res.payload.affected_count;
  }

  onSnapshot(onNext: (snapshot: QuerySnapshot) => void, onError?: ErrorCallback): Unsubscribe {
    if (this.spec.cursor !== undefined)
      throw new PodError("INVALID_ARGUMENT", "Listeners do not support startAfter().", 400);
    return this.pod._listen({ kind: "query", query: this, onNext, ...(onError ? { onError } : {}) });
  }
}

/** A collection location: an odd number of path segments. */
export class CollectionReference extends Query {
  readonly segments: readonly string[];

  constructor(pod: Pod, segments: readonly string[]) {
    super(pod, segments.join("/"));
    this.segments = segments;
  }

  get id(): string {
    return this.segments[this.segments.length - 1]!;
  }

  /** The document that owns this sub-collection, or null at the root. */
  get parent(): DocumentReference | null {
    return this.segments.length > 1 ? new DocumentReference(this.pod, this.segments.slice(0, -1)) : null;
  }

  /** A document in this collection; a new uuidv7 id when omitted. */
  doc(id?: string): DocumentReference {
    return this.pod.doc(`${this.path}/${id === undefined ? uuidv7() : parsePath(id).join("/")}`);
  }

  /** Creates a document with a generated id. */
  async add(data: DocumentData): Promise<DocumentReference> {
    const ref = this.doc();
    await ref.set(data);
    return ref;
  }

  /** Deletes every document in (and beneath) this collection. Returns the count. */
  async delete(): Promise<number> {
    const res = await this.pod._request<IDeleteResponse>("DELETE", { path: this.path });
    return res.payload.affected_count;
  }

  isEqual(other: CollectionReference): boolean {
    return other.pod === this.pod && other.path === this.path;
  }
}
