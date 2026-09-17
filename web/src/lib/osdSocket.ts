// /ws/osd client for font, plane and banner requests; replies match by id.

interface PendingRequest {
    resolve: (value: any) => void;
    reject: (error: Error) => void;
    timer: ReturnType<typeof setTimeout>;
    bannerMeta?: any;
}

// Backstop for a dead socket; RT4K errors arrive as {ok:false} replies.
const REQUEST_TIMEOUT_MS = 60000;

let socket: WebSocket | null = null;
let connectPromise: Promise<WebSocket> | null = null;
let nextId = 1;
const pending = new Map<number, PendingRequest>();

// Banner metadata waiting for its binary frame (one banner request at a time).
let pendingBannerId: number | null = null;

// Handlers for messages the bridge pushes without a request id.
const listeners = new Map<string, Set<(message: any) => void>>();

export function onOsdEvent(type: string, handler: (message: any) => void): () => void {
    const handlers = listeners.get(type) ?? new Set();
    listeners.set(type, handlers);
    handlers.add(handler);
    return () => {
        handlers.delete(handler);
    };
}

function failAll(error: Error): void {
    for (const request of pending.values()) {
        clearTimeout(request.timer);
        request.reject(error);
    }
    pending.clear();
    pendingBannerId = null;
}

function handleText(text: string): void {
    let message: any;
    try {
        message = JSON.parse(text);
    } catch {
        return;
    }

    if (message.id === undefined) {
        listeners.get(message.type)?.forEach((handler) => handler(message));
        return;
    }

    const request = pending.get(message.id);
    if (!request)
        return;

    if (message.type === "banner" && message.hasData) {
        pendingBannerId = message.id;
        request.bannerMeta = message;
        return;
    }

    pending.delete(message.id);
    clearTimeout(request.timer);
    request.resolve(message);
}

function handleBinary(data: ArrayBuffer): void {
    if (pendingBannerId === null)
        return;

    const id = pendingBannerId;
    pendingBannerId = null;

    const request = pending.get(id);
    if (!request)
        return;

    pending.delete(id);
    clearTimeout(request.timer);
    request.resolve({ ...request.bannerMeta, bytes: data });
}

function connect(): Promise<WebSocket> {
    if (socket && socket.readyState === WebSocket.OPEN)
        return Promise.resolve(socket);

    if (connectPromise)
        return connectPromise;

    connectPromise = new Promise((resolve, reject) => {
        const protocol = location.protocol === "https:" ? "wss://" : "ws://";
        const ws = new WebSocket(protocol + location.host + "/ws/osd");
        ws.binaryType = "arraybuffer";

        ws.onopen = () => {
            socket = ws;
            connectPromise = null;
            resolve(ws);
        };

        ws.onmessage = (event: MessageEvent) => {
            if (event.data instanceof ArrayBuffer)
                handleBinary(event.data);
            else
                handleText(event.data);
        };

        ws.onclose = () => {
            const wasOpen = socket === ws;
            socket = null;
            connectPromise = null;

            if (wasOpen)
                failAll(new Error("OSD socket closed"));
            else
                reject(new Error("Failed to connect to OSD socket"));
        };

        ws.onerror = () => ws.close();
    });

    return connectPromise;
}

export async function osdRequest<T = any>(type: string, params: Record<string, unknown> = {}): Promise<T> {
    const ws = await connect();
    const id = nextId++;

    return new Promise<T>((resolve, reject) => {
        const timer = setTimeout(() => {
            pending.delete(id);
            reject(new Error("OSD request timed out"));
        }, REQUEST_TIMEOUT_MS);

        pending.set(id, { resolve, reject, timer });
        ws.send(JSON.stringify({ id, type, ...params }));
    });
}
