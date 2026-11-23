// One Durable Object holds the host list.
// Set LOBBY_KEY with `npx wrangler secret put LOBBY_KEY`. Do not print it.
import { Directory } from "./directory.js";

const reply = (status, body) => new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
});

const sameText = (a, b) => {
    let different = a.length ^ b.length;
    const limit = Math.max(a.length, b.length);
    for (let i = 0; i < limit; i++) {
        different |= (a.charCodeAt(i) || 0) ^ (b.charCodeAt(i) || 0);
    }
    return different === 0;
};

export class HostDirectory {
    constructor(state) {
        this.storage = state.storage;
        this.directory = new Directory();
        state.blockConcurrencyWhile(async () => {
            for (const [id, host] of await this.storage.list()) {
                this.directory.hosts.set(id, host);
            }
        });
    }

    async fetch(request) {
        const url = new URL(request.url);
        const body = request.method === "GET" ? "" : await request.text();
        const result = await this.directory.handle(request.method, url.pathname, body);
        if (result.saved) {
            await this.storage.put(result.saved.id, result.saved.record);
        }
        if (result.removed && result.removed.length) {
            await this.storage.delete(result.removed);
        }
        return reply(result.status, result.body);
    }
}

export default {
    async fetch(request, env) {
        const wanted = String(env.LOBBY_KEY ?? "");
        if (wanted && !sameText(request.headers.get("x-heartsoul-key") ?? "", wanted)) {
            return reply(401, { error: "key" });
        }
        const stub = env.DIRECTORY.get(env.DIRECTORY.idFromName("hosts"));
        return stub.fetch(request);
    },
};
