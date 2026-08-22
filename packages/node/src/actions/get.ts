import type { DocumentReference } from "references/doc";

async function getDoc(doc: DocumentReference) {
    doc._db._request({
        meta: {
            type: 'READ'
        },
        payload: {
            path: doc._path.join("/")
        }
    })
}
