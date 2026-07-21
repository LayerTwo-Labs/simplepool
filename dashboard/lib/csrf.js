/* Tiny in-memory CSRF token store.
 *
 * HTTP Basic auth alone doesn't defend against CSRF: a malicious page can
 * make the browser POST cross-site with the cached basic-auth header. We
 * mint a random token when the admin page is rendered, stash it, and
 * require it back on every POST /admin/action/*. The attacker can't read
 * the admin page (basic auth blocks the GET), so they can't obtain a
 * valid token.
 *
 * Tokens are single-use and expire after CSRF_TTL_MS. Kept in memory —
 * fine for a single dashboard process; a multi-worker deployment would
 * need shared storage. */

import { randomBytes } from 'node:crypto';

const CSRF_TTL_MS = 60 * 60 * 1000;   // 1 hour

const tokens = new Map();   // token -> expiresAtMs

function sweep() {
    const now = Date.now();
    for (const [t, exp] of tokens) if (exp < now) tokens.delete(t);
}

export function issueToken() {
    sweep();
    const t = randomBytes(24).toString('base64url');
    tokens.set(t, Date.now() + CSRF_TTL_MS);
    return t;
}

/* Consumes the token — a second call with the same token returns false. */
export function consumeToken(t) {
    if (!t || typeof t !== 'string') return false;
    sweep();
    const exp = tokens.get(t);
    if (!exp) return false;
    tokens.delete(t);
    return exp >= Date.now();
}
