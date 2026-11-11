// Persistent cache used by real-time listeners to store their last
// consistent view and resume token, so a new listener (or a reloaded page)
// can show cached data immediately and resume with only the missed deltas.

export interface StorageAdapter {
  get(key: string): Promise<unknown>;
  set(key: string, value: unknown): Promise<void>;
  remove(key: string): Promise<void>;
}

/** Process-local cache (the default). Values are deep-copied. */
export class MemoryStorageAdapter implements StorageAdapter {
  private readonly store = new Map<string, unknown>();

  async get(key: string): Promise<unknown> {
    const v = this.store.get(key);
    return v === undefined ? undefined : structuredClone(v);
  }

  async set(key: string, value: unknown): Promise<void> {
    this.store.set(key, structuredClone(value));
  }

  async remove(key: string): Promise<void> {
    this.store.delete(key);
  }
}

type IdbModule = typeof import("idb-keyval");
type IdbStore = ReturnType<IdbModule["createStore"]>;

/**
 * Browser IndexedDB cache (via idb-keyval, loaded lazily so Node/Bun never
 * import it unless this adapter is used). Failures degrade to cache misses.
 */
export class IndexedDBStorageAdapter implements StorageAdapter {
  private handle: Promise<{ idb: IdbModule; store: IdbStore }> | null = null;

  constructor(
    private readonly databaseName = "pod-cache",
    private readonly storeName = "listeners",
  ) {}

  private open() {
    this.handle ??= import("idb-keyval").then((idb) => ({
      idb,
      store: idb.createStore(this.databaseName, this.storeName),
    }));
    return this.handle;
  }

  async get(key: string): Promise<unknown> {
    try {
      const { idb, store } = await this.open();
      return await idb.get(key, store);
    } catch {
      return undefined;
    }
  }

  async set(key: string, value: unknown): Promise<void> {
    try {
      const { idb, store } = await this.open();
      await idb.set(key, value, store);
    } catch {
      // Cache writes are best effort.
    }
  }

  async remove(key: string): Promise<void> {
    try {
      const { idb, store } = await this.open();
      await idb.del(key, store);
    } catch {
      // Cache writes are best effort.
    }
  }
}

