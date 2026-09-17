import { postForm } from "./client";

// The device queues presses, so callers don't need to wait between them.
export function postRemoteButton(button: string): Promise<Response> {
    return postForm("/api/remote", { button });
}
