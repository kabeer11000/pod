import * as IDB from "idb-keyval";

export class ImplementableStorageAdapter {
    async set(key: string, value: any): Promise<void> {}
    async get(key: string): Promise<any> {}
    async remove(key: string): Promise<void> {}
}
export class IndexedDB implements ImplementableStorageAdapter {
    private store: any;

    constructor(collectionKey: string) {
        this.store = IDB.createStore("pod-cache", collectionKey);
    }

    async get(key: string): Promise<any> {
        try {
            return await IDB.get(key, this.store);
        } catch (error) {
            if (process.env.NODE_ENV === "development") {
                console.warn("IndexedDB get failed:", error);
            }
            return undefined;
        }
    }

    async remove(key: string): Promise<void> {
        try {
            await IDB.del(key, this.store);
        } catch (error) {
            if (process.env.NODE_ENV === "development") {
                console.warn("IndexedDB remove failed:", error);
            }
        }
    }

    async set(key: string, value: any): Promise<void> {
        try {
            await IDB.set(key, value, this.store);
        } catch (error) {
            if (process.env.NODE_ENV === "development") {
                console.warn("IndexedDB set failed:", error);
            }
        }
    }
}
