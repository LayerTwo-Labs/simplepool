/* Reads the dashboard's public JSON API. /api/status is always 200 when the
 * dashboard is up (health is reported inside it), so any other status here
 * is the dashboard itself failing. */

const TIMEOUT_MS = 10000;

export class DashboardClient {
    constructor({ url, fetchImpl = fetch }) {
        this.url = url;
        this.fetch = fetchImpl;
    }

    async status() {
        return this.#get('/api/status');
    }

    /* Newest first. */
    async blocks(limit = 50) {
        const r = await this.#get(`/api/blocks?limit=${limit}`);
        return r.rows ?? [];
    }

    async #get(path) {
        const res = await this.fetch(this.url + path, { signal: AbortSignal.timeout(TIMEOUT_MS) });
        if (!res.ok) throw new Error(`dashboard ${path}: HTTP ${res.status}`);
        return res.json();
    }
}
