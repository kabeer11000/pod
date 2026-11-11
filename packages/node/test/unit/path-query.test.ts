import { describe, expect, test } from "bun:test";
import { Pod, PodError } from "../../src/index";
import { specToPayload } from "../../src/query";
import { collectionSegments, documentSegments, getField, parsePath } from "../../src/path";

const pod = new Pod({ uri: "ws://127.0.0.1:1/webchannel" });

function code(fn: () => unknown): string | undefined {
  try {
    fn();
  } catch (e) {
    return (e as PodError).code;
  }
  return undefined;
}

describe("paths", () => {
  test("accepts and normalises valid paths", () => {
    expect(parsePath("/users/u1/")).toEqual(["users", "u1"]);
    expect(documentSegments("users/u1/posts/p1")).toEqual(["users", "u1", "posts", "p1"]);
    expect(collectionSegments("users/u1/posts")).toEqual(["users", "u1", "posts"]);
  });

  test.each([
    ["", "INVALID_PATH"],
    ["///", "INVALID_PATH"],
    ["a//b", "INVALID_PATH"],
    ["a/./b", "INVALID_PATH"],
    ["a/..", "INVALID_PATH"],
    ["__x__/d", "INVALID_PATH"],
    ["a\0b/c", "INVALID_PATH"],
    ["x".repeat(1501) + "/d", "INVALID_PATH"],
    [Array(101).fill("a").join("/"), "INVALID_PATH"],
  ])("rejects %p", (p, c) => {
    expect(code(() => parsePath(p))).toBe(c);
  });

  test("segment byte limit counts UTF-8 bytes", () => {
    expect(code(() => parsePath("é".repeat(750)))).toBeUndefined();
    expect(code(() => parsePath("é".repeat(751)))).toBe("INVALID_PATH");
  });

  test("document vs collection parity", () => {
    expect(code(() => pod.doc("users"))).toBe("INVALID_PATH");
    expect(code(() => pod.collection("users/u1"))).toBe("INVALID_PATH");
    expect(pod.collection("users").doc("u1").path).toBe("users/u1");
    expect(pod.doc("users/u1").collection("posts").path).toBe("users/u1/posts");
    expect(pod.doc("users/u1").parent.path).toBe("users");
    expect(pod.collection("users/u1/posts").parent?.path).toBe("users/u1");
    expect(pod.collection("users").parent).toBeNull();
    expect(code(() => pod.collection("users").doc("a/b"))).toBe("INVALID_PATH");
  });

  test("generated ids are unique and time-ordered", () => {
    const a = pod.collection("c").doc().id;
    const b = pod.collection("c").doc().id;
    expect(a).not.toBe(b);
    expect(a < b).toBe(true);
  });

  test("getField", () => {
    const data = { a: { b: [1, 2] }, n: null };
    expect(getField(data, "a.b")).toEqual([1, 2]);
    expect(getField(data, "a.b.0")).toBeUndefined();
    expect(getField(data, "n")).toBeNull();
    expect(getField(data, "missing.x")).toBeUndefined();
  });
});

describe("queries", () => {
  const users = pod.collection("users");

  test("builds the wire payload", () => {
    const q = users
      .where("age", "GREATER_EQUAL", 18)
      .where("tags", "ARRAY.IN", "x")
      .orderBy("age", "desc")
      .orderBy("name")
      .limit(10)
      .select("name", "age")
      .startAfter("u5");
    expect(specToPayload(q.path, q.spec)).toEqual({
      path: "users",
      where: [
        { field: "age", op: "GREATER_EQUAL", value: 18 },
        { field: "tags", op: "ARRAY.IN", value: "x" },
      ],
      orderBy: [
        { field: "age", direction: "DESCENDING" },
        { field: "name", direction: "ASCENDING" },
      ],
      limit: 10,
      project: ["name", "age"],
      cursor: "u5",
    });
  });

  test("queries are immutable", () => {
    const a = users.where("x", "EQUAL", 1);
    const b = a.limit(5);
    expect(a.spec.limit).toBeUndefined();
    expect(b.spec.limit).toBe(5);
    expect(users.spec.where).toHaveLength(0);
  });

  test("startAfter accepts a snapshot-like object", () => {
    expect(users.startAfter({ id: "abc" }).spec.cursor).toBe("abc");
    expect(code(() => users.startAfter(""))).toBe("INVALID_CURSOR");
    expect(code(() => users.startAfter("a/b"))).toBe("INVALID_CURSOR");
  });

  test.each([
    [() => users.where("", "EQUAL", 1), "INVALID_ARGUMENT"],
    [() => users.where("a..b", "EQUAL", 1), "INVALID_ARGUMENT"],
    [() => users.where("a", "LIKE" as never, 1), "INVALID_ARGUMENT"],
    [() => users.where("a", "EQUAL", undefined), "INVALID_ARGUMENT"],
    [() => users.where("a", "ARRAY.EQUALS", 1), "INVALID_ARGUMENT"],
    [() => users.where("a", "ARRAY.ELEMENT_AT", { index: 1.5, value: 1 }), "INVALID_ARGUMENT"],
    [() => users.where("a", "ARRAY.ELEMENT_AT", { index: 1 }), "INVALID_ARGUMENT"],
    [() => users.orderBy("a", "up" as never), "INVALID_ARGUMENT"],
    [() => users.limit(0), "INVALID_ARGUMENT"],
    [() => users.limit(1.5), "INVALID_ARGUMENT"],
    [() => users.select("ok", ""), "INVALID_ARGUMENT"],
  ])("rejects invalid constraint %#", (fn, c) => {
    expect(code(fn)).toBe(c);
  });

  test("ARRAY.ELEMENT_AT accepts negative indexes", () => {
    expect(users.where("a", "ARRAY.ELEMENT_AT", { index: -1, value: null }).spec.where[0]?.value).toEqual({
      index: -1,
      value: null,
    });
  });

  test("mutateMany only accepts where()", async () => {
    await expect(users.orderBy("a").mutateMany({ set: { x: 1 } })).rejects.toMatchObject({ code: "INVALID_ARGUMENT" });
    await expect(users.mutateMany({ remove: [""] })).rejects.toMatchObject({ code: "INVALID_ARGUMENT" });
  });

  test("listeners reject startAfter", () => {
    expect(code(() => users.startAfter("x").onSnapshot(() => {}))).toBe("INVALID_ARGUMENT");
  });
});

describe("options", () => {
  test("validates uri and namespace", () => {
    expect(code(() => new Pod({ uri: "http://x" }))).toBe("INVALID_ARGUMENT");
    expect(code(() => new Pod({ uri: "not a url" }))).toBe("INVALID_ARGUMENT");
    expect(code(() => new Pod({ uri: "ws://x", namespace: "bad ns" }))).toBe("INVALID_ARGUMENT");
    expect(code(() => new Pod({ uri: "ws://x", namespace: "x".repeat(65) }))).toBe("INVALID_ARGUMENT");
    expect(code(() => new Pod({ uri: "ws://x", namespace: "team-A_1" }))).toBeUndefined();
  });
});
