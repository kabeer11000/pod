import type { PodReference } from "references/pod";

export class CollectionReference {
    _db: PodReference;
    _path: string[];
    readonly type = "collection";
    constructor(db: PodReference, path: string) {
        this._db = db;
        this._path = path.split("/").filter(Boolean);
    }
}

export function collection(db: PodReference, path: string): CollectionReference {
    return new CollectionReference(db, path);
}
