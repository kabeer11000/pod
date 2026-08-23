import { uuidv7 } from "vendor/uuid7";
import { CollectionReference } from "references/collection";
import type { PodReference } from "references/pod";

export class DocumentReference {
    readonly type = "pod/document";
    _db: PodReference;
    _path: string[];
    constructor(db: PodReference, path: string[]) {
        this._db = db;
        this._path = path;
    }
}

export function doc(db: PodReference, path: string): DocumentReference;
export function doc(collection: CollectionReference, id?: string): DocumentReference;
export function doc(
    dbOrCollection: PodReference | CollectionReference,
    pathOrId?: string,
): DocumentReference {
    if (dbOrCollection instanceof CollectionReference) {
        const collection = dbOrCollection;
        const id: string = pathOrId ?? uuidv7();
        return new DocumentReference(collection._db, [...collection._path, id]);
    }
    const _path: string[] = (pathOrId ?? "").split("/").filter(Boolean);
    return new DocumentReference(dbOrCollection, _path);
}
