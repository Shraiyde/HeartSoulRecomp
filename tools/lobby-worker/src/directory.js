// Heart & Soul phone-host list. Game traffic does not pass through here.
export const TTL_MS = 90000;
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
        if (path.split("?")[0] !== "/v1/lobbies") {
            return { status: 404, body: { error: "not found" }, removed: this.prune() };
        }
        const removed = this.prune();
        if (method === "GET") {
            const lobbies = [...this.hosts.values()]
                .map(({ seen, ...shown }) => shown)
                .sort((a, b) => String(a.name).localeCompare(String(b.name)));
            return { status: 200, body: { lobbies }, removed };
        }
        if (method !== "POST" && method !== "DELETE") {
            return { status: 405, body: { error: "method" }, removed };
        }
        if (bodyText.length > 2048) {
            return { status: 413, body: { error: "too large" }, removed };
        }
        let data;
        try {
            data = JSON.parse(bodyText || "{}");
        } catch {
            return { status: 400, body: { error: "json" }, removed };
        }
        if (!hexKey(data.key)) {
            return { status: 400, body: { error: "key" }, removed };
        }
        const id = await publicId(data.key);
        if (method === "DELETE" || data.remove === true) {
            const existed = this.hosts.delete(id);
            return { status: 200, body: { ok: true }, removed: existed ? removed.concat(id) : removed };
        }
        if (!addressOk(data.address)) {
            return { status: 400, body: { error: "address" }, removed };
        }
        if (!this.hosts.has(id) && this.hosts.size >= MAX_HOSTS) {
            return { status: 503, body: { error: "full" }, removed };
        }
        const record = {
            address: String(data.address),
            name: text(data.name, 24) || "HEART & SOUL",
            host: text(data.host, 24) || "PLAYER",
            players: whole(data.players, 1, 2),
            max: whole(data.max, 1, 2),
            kind: "game",
            port: whole(data.port, 1, 65535),
            protocol: "heartsoul-direct-v1",
            seen: this.now(),
        };
        this.hosts.set(id, record);
        return { status: 200, body: { ok: true, ttl: TTL_MS / 1000 }, saved: { id, record }, removed };
    }
}

function text(value, limit) {
    return String(value ?? "").replace(/[\u0000-\u001f]/g, "").trim().slice(0, limit);
}

function whole(value, low, high) {
    const number = Math.trunc(Number(value));
    if (!Number.isFinite(number)) return low;
    return Math.min(high, Math.max(low, number));
}
