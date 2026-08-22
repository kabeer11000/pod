import type { PodReference } from "references/pod";

export class CollectionReference {
    _db: PodReference;
    readonly type = "collection";
    constructor(db: PodReference, path: string) {
        this._db = db;
    }
}
