import { describe, expect, test } from "bun:test";
import { applyDelta, diffViews, ViewDesyncError, type ViewChange, type WireDocument } from "../../src/realtime/view";

const doc = (id: string, v = 1, extra: Record<string, unknown> = {}): WireDocument => ({
  path: `c/${id}`,
  data: { id, v, ...extra },
  _v: v,
});

/** The wire delta the server would send for a change. */
function toWire(c: ViewChange) {
  const type = c.type === "added" ? "ADDED" : c.type === "modified" ? "MODIFIED" : "REMOVED";
  return {
    type,
    delta: {
      path: c.doc.path,
      old_index: c.oldIndex === -1 ? null : c.oldIndex,
      new_index: c.newIndex === -1 ? null : c.newIndex,
      ...(c.type === "removed" ? {} : { document: c.doc }),
    },
  } as const;
}

// Deterministic PRNG so failures are reproducible.
function rng(seed: number) {
  return () => {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return seed / 0x7fffffff;
  };
}

describe("applyDelta (protocol/README delta rules)", () => {
  test("REMOVED removes at old_index", () => {
    const v = [doc("a"), doc("b"), doc("c")];
    applyDelta(v, "REMOVED", { path: "c/b", old_index: 1, new_index: null });
    expect(v.map((d) => d.path)).toEqual(["c/a", "c/c"]);
  });

  test("ADDED inserts at new_index", () => {
    const v = [doc("a"), doc("c")];
    applyDelta(v, "ADDED", { path: "c/b", old_index: null, new_index: 1, document: doc("b") });
    expect(v.map((d) => d.path)).toEqual(["c/a", "c/b", "c/c"]);
  });

  test("MODIFIED removes at old_index, then inserts at new_index", () => {
    const v = [doc("a"), doc("b"), doc("c")];
    // a moves to the end: new_index is relative to the list after removal.
    applyDelta(v, "MODIFIED", { path: "c/a", old_index: 0, new_index: 2, document: doc("a", 2) });
    expect(v.map((d) => d.path)).toEqual(["c/b", "c/c", "c/a"]);
    expect(v[2]!._v).toBe(2);
  });

  test("detects desync", () => {
    const v = [doc("a")];
    expect(() => applyDelta(v, "REMOVED", { path: "c/x", old_index: 0, new_index: null })).toThrow(ViewDesyncError);
    expect(() => applyDelta(v, "REMOVED", { path: "c/a", old_index: 1, new_index: null })).toThrow(ViewDesyncError);
    expect(() => applyDelta(v, "ADDED", { path: "c/a", old_index: null, new_index: 0, document: doc("a") })).toThrow(
      ViewDesyncError,
    );
    expect(() => applyDelta(v, "ADDED", { path: "c/b", old_index: null, new_index: 5, document: doc("b") })).toThrow(
      ViewDesyncError,
    );
    expect(() => applyDelta(v, "MODIFIED", { path: "c/a", old_index: 0, new_index: 0 })).toThrow(ViewDesyncError);
    expect(v).toEqual([doc("a")]);
  });
});

describe("diffViews", () => {
  test("unchanged documents produce no changes", () => {
    const v = [doc("a"), doc("b")];
    expect(diffViews(v, v.map((d) => ({ ...d, data: { ...d.data } })))).toEqual([]);
  });

  test("randomized: diff then apply reproduces the target", () => {
    const random = rng(42);
    const ids = Array.from({ length: 12 }, (_, i) => `d${i}`);
    let checks = 0;
    for (let round = 0; round < 3000; round++) {
      const pick = () => {
        const shuffled = [...ids].sort(() => random() - 0.5);
        return shuffled
          .slice(0, Math.floor(random() * ids.length))
          .map((id) => doc(id, 1 + Math.floor(random() * 3), random() < 0.3 ? { x: random() } : {}));
      };
      const from = pick();
      const to = pick();
      const changes = diffViews(from, to);
      const view = from.map((d) => ({ ...d }));
      for (const c of changes) {
        const w = toWire(c);
        applyDelta(view, w.type, w.delta);
      }
      expect(view).toEqual(to);
      // Each document changes at most once.
      expect(new Set(changes.map((c) => c.doc.path)).size).toBe(changes.length);
      checks++;
    }
    expect(checks).toBe(3000);
  });
});
