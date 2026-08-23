import type { DocumentReference } from "references/doc";
import type { IErrorResponse, IReadResponse } from "network/ws";

export interface IDocumentSnapshot {
    path: string;
    exists: boolean;
    data: Record<string, any> | null;
}

export async function getDoc(doc: DocumentReference): Promise<IDocumentSnapshot> {
    const response = await doc._db._request({
        meta: {
            type: "READ",
        },
        payload: {
            path: doc._path.join("/"),
        },
    });

    if (response.meta.type === "ERROR") {
        const error = response as IErrorResponse;
        throw new Error(`Pod: ${error.payload.code} - ${error.payload.message}`);
    }

    const { path, data } = (response as IReadResponse).payload.document;
    return {
        path,
        exists: data !== null && data !== undefined,
        data: data ?? null,
    };
}
