// Pure client-side view maintenance for subscriptions (no I/O).
//
// Delta indices are sequential steps (protocol/README):
//   REMOVED   remove at old_index
//   ADDED     insert at new_index
//   MODIFIED  remove at old_index, then insert the new document at new_index
import type { IDocumentVersion, ISubscriptionEventPush } from "../network/ws/index";

export interface WireDocument {
  path: string;
  data: Record<string, any>;
  _v?: IDocumentVersion;
}

export type ChangeType = "added" | "modified" | "removed";

export interface ViewChange {
  type: ChangeType;
  doc: WireDocument;
  /** Position before the change, -1 for "added". */
  oldIndex: number;
  /** Position after the change, -1 for "removed". */
  newIndex: number;
}

/** The server's deltas no longer match the local view. */
export class ViewDesyncError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ViewDesyncError";
  }
}

type Delta = NonNullable<ISubscriptionEventPush["payload"]["delta"]>;

/** Applies one ADDED / MODIFIED / REMOVED delta to `view` in place. */
export function applyDelta(view: WireDocument[], type: "ADDED" | "MODIFIED" | "REMOVED", delta: Delta): ViewChange {
  const isIndex = (n: unknown): n is number => typeof n === "number" && Number.isInteger(n) && n >= 0;
  if (type === "REMOVED") {
    const i = delta.old_index;
    if (!isIndex(i) || i >= view.length || view[i]!.path !== delta.path)
      throw new ViewDesyncError(`REMOVED ${delta.path} at ${String(i)} does not match the local view`);
    const [doc] = view.splice(i, 1);
    return { type: "removed", doc: doc!, oldIndex: i, newIndex: -1 };
  }
  const doc = delta.document;
  if (!doc) throw new ViewDesyncError(`${type} ${delta.path} has no document`);
  const fresh: WireDocument = { path: doc.path, data: doc.data, ...(doc._v !== undefined ? { _v: doc._v } : {}) };
  if (type === "ADDED") {
    const j = delta.new_index;
    if (!isIndex(j) || j > view.length) throw new ViewDesyncError(`ADDED ${delta.path} at ${String(j)} is out of range`);
    if (view.some((d) => d.path === delta.path)) throw new ViewDesyncError(`ADDED ${delta.path} is already in the view`);
    view.splice(j, 0, fresh);
    return { type: "added", doc: fresh, oldIndex: -1, newIndex: j };
  }
  const i = delta.old_index;
  const j = delta.new_index;
  if (!isIndex(i) || i >= view.length || view[i]!.path !== delta.path)
    throw new ViewDesyncError(`MODIFIED ${delta.path} at ${String(i)} does not match the local view`);
  view.splice(i, 1);
  if (!isIndex(j) || j > view.length) throw new ViewDesyncError(`MODIFIED ${delta.path} to ${String(j)} is out of range`);
  view.splice(j, 0, fresh);
  return { type: "modified", doc: fresh, oldIndex: i, newIndex: j };
}

/**
 * Sequential changes that transform `from` into `to` (used when a full
 * SNAPSHOT replaces an existing view). Applying the changes in order to a
 * copy of `from` yields exactly `to`.
 */
export function diffViews(from: readonly WireDocument[], to: readonly WireDocument[]): ViewChange[] {
  const target = new Set(to.map((d) => d.path));
  const changes: ViewChange[] = [];
  const cur: WireDocument[] = [];
  let removed = 0;
  from.forEach((d, i) => {
    if (target.has(d.path)) cur.push(d);
    else changes.push({ type: "removed", doc: d, oldIndex: i - removed++, newIndex: -1 });
  });
  to.forEach((d, j) => {
    const i = cur.findIndex((c, k) => k >= j && c.path === d.path);
    if (i === -1) {
      cur.splice(j, 0, d);
      changes.push({ type: "added", doc: d, oldIndex: -1, newIndex: j });
      return;
    }
    const held = cur[i]!;
    if (i === j && held._v === d._v && sameData(held.data, d.data)) {
      cur[j] = d;
      return;
    }
    cur.splice(i, 1);
    cur.splice(j, 0, d);
    changes.push({ type: "modified", doc: d, oldIndex: i, newIndex: j });
  });
  return changes;
}

function sameData(a: unknown, b: unknown): boolean {
  if (a === b) return true;
  if (typeof a !== "object" || typeof b !== "object" || a === null || b === null) return false;
  if (Array.isArray(a) !== Array.isArray(b)) return false;
  if (Array.isArray(a)) {
    const bb = b as unknown[];
    return a.length === bb.length && a.every((v, i) => sameData(v, bb[i]));
  }
  const ka = Object.keys(a);
  const kb = Object.keys(b);
  return (
    ka.length === kb.length &&
    ka.every((k) => Object.prototype.hasOwnProperty.call(b, k) && sameData((a as any)[k], (b as any)[k]))
  );
}

/** Stable JSON: object keys sorted recursively; used for cache keys. */
export function canonicalJson(value: unknown): string {
  if (value === null || typeof value !== "object") return JSON.stringify(value) ?? "null";
  if (Array.isArray(value)) return `[${value.map((v) => canonicalJson(v === undefined ? null : v)).join(",")}]`;
  const entries = Object.keys(value)
    .filter((k) => (value as Record<string, unknown>)[k] !== undefined)
    .sort()
    .map((k) => `${JSON.stringify(k)}:${canonicalJson((value as Record<string, unknown>)[k])}`);
  return `{${entries.join(",")}}`;
}
