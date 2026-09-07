/* Minimal ConnectRPC client for the bip300301_enforcer. Unary RPCs only. */

export async function enforcerRpc(enforcerAddr, rpcPath, body, timeoutMs = 30_000) {
    const base = /^https?:\/\//.test(enforcerAddr) ? enforcerAddr : `http://${enforcerAddr}`;
    const ctl  = new AbortController();
    const t    = setTimeout(() => ctl.abort(), timeoutMs);
    try {
        const r = await fetch(`${base}/${rpcPath}`, {
            method: 'POST',
            headers: {
                'content-type':             'application/json',
                'connect-protocol-version': '1',
            },
            body: JSON.stringify(body ?? {}),
            signal: ctl.signal,
        });
        const text = await r.text();
        let j;
        try { j = JSON.parse(text); }
        catch {
            throw new Error(`enforcer ${rpcPath}: non-json response (http ${r.status}): ${text.slice(0, 200)}`);
        }
        if (!r.ok) {
            const code = j.code || `http ${r.status}`;
            const err = new Error(
                `enforcer ${rpcPath}: ${code}${j.message ? `: ${j.message}` : ''}`);
            /* Keep the machine-readable parts on the error. Callers that need
             * to tell one failure from another should not have to match on
             * prose that upstream is free to reword. */
            err.code = j.code ?? null;
            err.status = r.status;
            throw err;
        }
        return j;
    } finally {
        clearTimeout(t);
    }
}
