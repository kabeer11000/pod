import type { IErrorCode } from "./network/ws/index";

/**
 * Error codes surfaced by the SDK: every server code from the protocol plus
 * client-side conditions.
 *   UNAVAILABLE        the connection dropped while the request was in flight
 *                      (the server may or may not have applied it)
 *   DEADLINE_EXCEEDED  no response within `requestTimeoutMs`
 *   CANCELLED          the client was closed
 */
export type PodErrorCode = IErrorCode | "DEADLINE_EXCEEDED" | "CANCELLED";

export class PodError extends Error {
  readonly code: PodErrorCode;
  /** HTTP-style status from the server, or 0 for client-side errors. */
  readonly status: number;
  readonly details?: Record<string, unknown>;

  constructor(code: PodErrorCode, message: string, status = 0, details?: Record<string, unknown>) {
    super(`${code}: ${message}`);
    this.name = "PodError";
    this.code = code;
    this.status = status;
    if (details !== undefined) this.details = details;
  }
}
