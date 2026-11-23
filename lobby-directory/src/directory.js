// Phone-host list for Heart & Soul. Hosts announce their ZeroTier address.
// Game traffic does not pass through here.
export const TTL_MS = 75000;
export const MAX_HOSTS = 200;

const hexKey = (value) => /^[0-9a-f]{32,64}$/.test(String(value ?? ""));
const addressOk = (value) => /^[0-9a-fA-F:.]+$/.test(String(value ?? "")) && String(value).length <= 64;

async function publicId(key) {
    const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(key));
    return [...new Uint8Array(digest)].slice(0, 8).map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

export class Directory {
    constructor(now = () => Date.now()) {
        this.now = now;
        this.hosts = new Map();
    }

    prune() {
        const cutoff = this.now() - TTL_MS;
        const removed = [];
        for (const [id, host] of this.hosts) {
            if (host.seen < cutoff) {
                this.hosts.delete(id);
                removed.push(id);
            }
        }
        return removed;
    }

    async handle(method, path, bodyText) {
        if (path.split("?")[0] !== "/v1/hosts") {
            return { status: 404, body: { error: "not found" }, removed: this.prune() };
        }
        const removed = this.prune();
        if (method === "GET") {
            const hosts = [...this.hosts.values()]
                .map(({ address, seen }) => ({ address, seen }))
                .sort((a, b) => b.seen - a.seen);
            return { status: 200, body: { hosts }, removed };
        }
        if (method !== "POST") {
            return { status: 405, body: { error: "method" }, removed };
        }
        if (bodyText.length > 1024) {
            return { status: 413, body: { error: "too large" }, removed };
        }
        let data;
        try {
            data = JSON.parse(bodyText);
        } catch {
            return { status: 400, body: { error: "json" }, removed };
        }
        if (!hexKey(data.key) || !addressOk(data.address)) {
            return { status: 400, body: { error: "host" }, removed };
        }
        const id = await publicId(data.key);
        if (!this.hosts.has(id) && this.hosts.size >= MAX_HOSTS) {
            return { status: 503, body: { error: "full" }, removed };
        }
        const record = { address: String(data.address), seen: this.now() };
        this.hosts.set(id, record);
        return { status: 200, body: { ok: true, ttl: TTL_MS / 1000 }, saved: { id, ...record }, removed };
    }
}
