// Client-side path validation, mirroring the server's rules (protocol/README):
// '/'-separated segments alternating collection/document; leading/trailing
// '/' ignored; no empty segments, ".", "..", NUL, or __reserved__ segments;
// at most 100 segments of at most 1500 bytes.
import { PodError } from "./errors";

const MAX_SEGMENTS = 100;
const MAX_SEGMENT_BYTES = 1500;
const encoder = new TextEncoder();

export function parsePath(raw: string): string[] {
  if (typeof raw !== "string") throw new PodError("INVALID_PATH", "Path must be a string.", 400);
  const trimmed = raw.replace(/^\/+/, "").replace(/\/+$/, "");
  if (trimmed === "") throw new PodError("INVALID_PATH", "Path must not be empty.", 400);
  const segments = trimmed.split("/");
  if (segments.length > MAX_SEGMENTS)
    throw new PodError("INVALID_PATH", `Path exceeds ${MAX_SEGMENTS} segments.`, 400);
  for (const s of segments) validateSegment(s);
  return segments;
}

export function validateSegment(s: string): void {
  if (s === "") throw new PodError("INVALID_PATH", "Path contains an empty segment.", 400);
  if (s === "." || s === "..") throw new PodError("INVALID_PATH", "Path segments may not be '.' or '..'.", 400);
  if (s.length >= 4 && s.startsWith("__") && s.endsWith("__"))
    throw new PodError("INVALID_PATH", "Path segments of the form __name__ are reserved.", 400);
  if (s.includes("\0")) throw new PodError("INVALID_PATH", "Path segments may not contain NUL.", 400);
  if (s.includes("/")) throw new PodError("INVALID_PATH", "Path segments may not contain '/'.", 400);
  if (encoder.encode(s).length > MAX_SEGMENT_BYTES)
    throw new PodError("INVALID_PATH", `Path segment exceeds ${MAX_SEGMENT_BYTES} bytes.`, 400);
}

export function documentSegments(raw: string): string[] {
  const s = parsePath(raw);
  if (s.length % 2 !== 0)
    throw new PodError("INVALID_PATH", `'${raw}' is a collection path; a document path needs an even number of segments.`, 400);
  return s;
}

export function collectionSegments(raw: string): string[] {
  const s = parsePath(raw);
  if (s.length % 2 !== 1)
    throw new PodError("INVALID_PATH", `'${raw}' is a document path; a collection path needs an odd number of segments.`, 400);
  return s;
}

export function validateFieldPath(field: string): void {
  if (typeof field !== "string" || field === "" || field.split(".").some((p) => p === ""))
    throw new PodError("INVALID_ARGUMENT", `Invalid field path '${String(field)}'.`, 400);
}

/** Reads a dot-notation field from document data; undefined when absent. */
export function getField(data: Record<string, any> | undefined, field: string): unknown {
  let cur: unknown = data;
  for (const part of field.split(".")) {
    if (cur === null || typeof cur !== "object" || Array.isArray(cur)) return undefined;
    if (!Object.prototype.hasOwnProperty.call(cur, part)) return undefined;
    cur = (cur as Record<string, unknown>)[part];
  }
  return cur;
}
