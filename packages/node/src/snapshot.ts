import { getField } from "./path";
import type { Pod } from "./pod";
import type { ChangeType, WireDocument } from "./realtime/view";
import type { DocumentReference, Query } from "./references";

// Document contents are arbitrary JSON objects chosen by the application;
// `any` keeps field access ergonomic (data().title) as in other document SDKs.
export type DocumentData = Record<string, any>;

export interface SnapshotMetadata {
  /** True when the data came from the local cache, not the server. */
  readonly fromCache: boolean;
  /** getDocs: id of the secondary index the server used (null = scan). */
  readonly index?: string | null;
  /** getDocs: number of documents the server examined. */
  readonly documentsScanned?: number;
}

const SERVER: SnapshotMetadata = { fromCache: false };

export class DocumentSnapshot {
  readonly id: string;
  /** Server document version (_v); undefined when the document is missing. */
  readonly version: number | undefined;

  constructor(
    readonly ref: DocumentReference,
    private readonly _data: DocumentData | null,
    version: number | null | undefined,
    readonly metadata: SnapshotMetadata = SERVER,
  ) {
    this.id = ref.id;
    this.version = _data === null || version === null ? undefined : version;
  }

  exists(): boolean {
    return this._data !== null;
  }

  /** A copy of the document data, or undefined if it does not exist. */
  data(): DocumentData | undefined {
    return this._data === null ? undefined : structuredClone(this._data);
  }

  /** A field by dot-notation path, or undefined. */
  get(fieldPath: string): unknown {
    const v = getField(this._data ?? undefined, fieldPath);
    return v === undefined ? undefined : structuredClone(v);
  }
}

export interface DocumentChange {
  readonly type: ChangeType;
  readonly doc: DocumentSnapshot;
  /** Index before the change; -1 for "added". */
  readonly oldIndex: number;
  /** Index after the change; -1 for "removed". */
  readonly newIndex: number;
}

export class QuerySnapshot {
  constructor(
    readonly query: Query,
    readonly docs: readonly DocumentSnapshot[],
    readonly metadata: SnapshotMetadata,
    /** get(): more results exist after this page. */
    readonly hasMore = false,
    /** get(): pass to startAfter() for the next page. */
    readonly nextCursor: string | null = null,
    private readonly changes: readonly DocumentChange[] = [],
  ) {}

  get size(): number {
    return this.docs.length;
  }

  get empty(): boolean {
    return this.docs.length === 0;
  }

  forEach(cb: (doc: DocumentSnapshot, index: number) => void): void {
    this.docs.forEach(cb);
  }

  /** Changes since the previous snapshot delivered to the same listener. */
  docChanges(): DocumentChange[] {
    return [...this.changes];
  }
}

export function snapshotFromWire(pod: Pod, wire: WireDocument, metadata: SnapshotMetadata = SERVER): DocumentSnapshot {
  return new DocumentSnapshot(pod.doc(wire.path), wire.data, wire._v, metadata);
}
