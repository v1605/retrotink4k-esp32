// HTTP API helpers. JSON endpoints answer {"ok":true,...} or {"ok":false,"error":...}.

export interface OkResult {
    ok: boolean;
    error?: string;
}

type Fields = Record<string, string>;

const FORM_HEADERS = { "Content-Type": "application/x-www-form-urlencoded" };

export function withQuery(path: string, params?: Fields): string {
    return params ? path + "?" + new URLSearchParams(params) : path;
}

// The device answers errors as JSON too, so a non-JSON reply is unexpected.
export async function readJson<T>(res: Response): Promise<T> {
    try {
        return await res.json();
    } catch {
        throw new Error("Unexpected reply from the device (" + res.status + ")");
    }
}

export async function getJson<T>(path: string, params?: Fields): Promise<T> {
    return readJson<T>(await fetch(withQuery(path, params)));
}

// For endpoints that restart the device, whose reply may never arrive.
export function postForm(path: string, fields: Fields = {}): Promise<Response> {
    return fetch(path, {
        method: "POST",
        headers: FORM_HEADERS,
        body: new URLSearchParams(fields),
    });
}

export async function postFormJson<T>(path: string, fields: Fields = {}): Promise<T> {
    return readJson<T>(await postForm(path, fields));
}

// The device's error message from a failed response, or the status code.
export async function errorMessage(res: Response, fallback: string): Promise<string> {
    try {
        const info = await res.json();
        if (info.error)
            return info.error;
    } catch {}

    return fallback + " (" + res.status + ")";
}
