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

function doc(db: PodReference, path: string): DocumentReference;
function doc(collection: CollectionReference, path: string): DocumentReference;
function doc(
    PodOrCollectionReference: PodReference | CollectionReference,
    path: string,
): DocumentReference {
    const _path: string[] = path.split("/");
    if (PodOrCollectionReference instanceof CollectionReference) {
        // Generate DocumentId
        const collection = PodOrCollectionReference as CollectionReference;
        const id: string = _path.at(-1) ?? uuidv7();
        _path.push(id);
        return new DocumentReference(collection._db, _path);
    }
    return new DocumentReference(PodOrCollectionReference, _path);
}

// if (PodOrCollectionReference instanceof CollectionReference) {
//     // Generate DocumentId
//     const collection = PodOrCollectionReference;
//     const id: string = _path.at(-1) ?? uuidv7();
//     _path.push(id);
// }
