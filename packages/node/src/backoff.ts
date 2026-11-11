export interface ReconnectOptions {
  /** First retry delay. Default 250 ms. */
  initialDelayMs?: number;
  /** Upper bound for the delay. Default 10 s. */
  maxDelayMs?: number;
}

/**
 * Exponential backoff with "equal jitter": the delay for attempt n (0-based)
 * is uniformly distributed in [d/2, d] where d = min(max, initial * 2^n).
 */
export function backoffDelay(attempt: number, options: ReconnectOptions = {}, random: () => number = Math.random): number {
  const initial = Math.max(1, options.initialDelayMs ?? 250);
  const max = Math.max(initial, options.maxDelayMs ?? 10_000);
  const exp = Math.min(max, initial * 2 ** Math.min(Math.max(0, attempt), 30));
  const half = exp / 2;
  return Math.round(half + random() * half);
}
