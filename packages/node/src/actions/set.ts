import type { DocumentReference } from "references/doc";
import type { IErrorResponse, IWriteResponse } from "network/ws";

export interface IWriteResult {
    path: string;
}

export async function setDoc(
    doc: DocumentReference,
    data: Record<string, any>,
    options: { merge?: boolean } = {},
): Promise<IWriteResult> {
    const response = await doc._db._request({
        meta: {
            type: "WRITE",
        },
        payload: {
            path: doc._path.join("/"),
            merge: options.merge ?? false,
            data,
        },
    });

    if (response.meta.type === "ERROR") {
        const error = response as IErrorResponse;
        throw new Error(`Pod: ${error.payload.code} - ${error.payload.message}`);
    }

    return { path: (response as IWriteResponse).payload.path };
}
