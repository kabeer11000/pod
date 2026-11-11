// Query specification and validation. Pure data: the Query class in
// references.ts wraps a QuerySpec and builds new specs with these helpers.
import { PodError } from "./errors";
import type { IFilter, IFilterOperations, IOrderBy, IQueryRequest } from "./network/ws/index";
import { validateFieldPath } from "./path";

export type WhereOperator = IFilterOperations;
export type OrderDirection = "asc" | "desc" | "ASCENDING" | "DESCENDING";

const OPERATORS: readonly WhereOperator[] = [
  "EQUAL",
  "GREATER",
  "LESSER",
  "GREATER_EQUAL",
  "LESSER_EQUAL",
  "ARRAY.IN",
  "ARRAY.NOT_IN",
  "ARRAY.EQUALS",
  "ARRAY.ELEMENT_AT",
];

export interface QuerySpec {
  readonly where: readonly IFilter[];
  readonly orderBy: readonly IOrderBy[];
  readonly limit?: number;
  readonly project?: readonly string[];
  readonly cursor?: string;
}

export const EMPTY_SPEC: QuerySpec = { where: [], orderBy: [] };

export function makeFilter(field: string, op: WhereOperator, value: unknown): IFilter {
  validateFieldPath(field);
  if (!OPERATORS.includes(op)) throw new PodError("INVALID_ARGUMENT", `Unknown filter operation '${String(op)}'.`, 400);
  if (value === undefined) throw new PodError("INVALID_ARGUMENT", "where() value must not be undefined.", 400);
  if (op === "ARRAY.EQUALS" && !Array.isArray(value))
    throw new PodError("INVALID_ARGUMENT", "ARRAY.EQUALS requires an array value.", 400);
  if (op === "ARRAY.ELEMENT_AT") {
    const v = value as { index?: unknown } | null;
    if (!v || typeof v !== "object" || !Number.isInteger(v.index) || !("value" in v))
      throw new PodError("INVALID_ARGUMENT", 'ARRAY.ELEMENT_AT requires {"index": <integer>, "value": <any>}.', 400);
  }
  return { field, op, value };
}

export function makeOrder(field: string, direction: OrderDirection = "asc"): IOrderBy {
  validateFieldPath(field);
  const dir =
    direction === "asc" || direction === "ASCENDING"
      ? "ASCENDING"
      : direction === "desc" || direction === "DESCENDING"
        ? "DESCENDING"
        : null;
  if (!dir) throw new PodError("INVALID_ARGUMENT", `Invalid orderBy direction '${String(direction)}'.`, 400);
  return { field, direction: dir };
}

export function checkLimit(n: number): number {
  if (!Number.isInteger(n) || n < 1 || n > 4294967295)
    throw new PodError("INVALID_ARGUMENT", "limit() must be a positive integer.", 400);
  return n;
}

export function checkProjection(fields: readonly string[]): string[] {
  fields.forEach(validateFieldPath);
  return [...fields];
}

export function checkCursor(cursor: string | { readonly id: string }): string {
  const id = typeof cursor === "string" ? cursor : cursor?.id;
  if (typeof id !== "string" || id === "" || id.includes("/"))
    throw new PodError("INVALID_CURSOR", "startAfter() needs a document id or snapshot.", 400);
  return id;
}

/** The QUERY / SUBSCRIBE payload fields for a collection path and spec. */
export function specToPayload(path: string, spec: QuerySpec): IQueryRequest["payload"] {
  const p: IQueryRequest["payload"] = { path };
  if (spec.where.length) p.where = spec.where.map((f) => ({ ...f }));
  if (spec.orderBy.length) p.orderBy = spec.orderBy.map((o) => ({ ...o }));
  if (spec.limit !== undefined) p.limit = spec.limit;
  if (spec.project) p.project = [...spec.project];
  if (spec.cursor !== undefined) p.cursor = spec.cursor;
  return p;
}
