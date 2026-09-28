/* info.json: what a pool directory needs to list this pool.
 *
 * Facts about the pool are read from pool_meta, which the proxy writes from
 * its own config, and never configured here: a second copy of the fee or the
 * mode is a copy that can silently disagree with what the coinbase actually
 * does. Only presentation (name, logo, URLs, contact) comes from env.
 *
 * `mode` is the exact pool_mode. Three modes are PPLNS and they differ in
 * what a miner has to trust the pool with, so "pplns" alone would hide the
 * one thing a miner choosing a pool most needs to know.
 */

/* One sentence per mode on how a miner gets paid. */
export const PAYOUT_TEXT = {
    'solo':
        'The stratum username is your BTC address; each block\'s coinbase pays the miner who found it.',
    'pps-classic':
        'The stratum username is your Thunder address; every accepted share is credited at a rate '
        + 'derived from the current block template, and balances are paid over Thunder in a daily batch.',
    'pplns-thunder':
        'The stratum username is your Thunder address; each block is split across the last window of '
        + 'shares once it is 100 blocks deep, and balances are paid over Thunder.',
    'pplns-btc':
        'The stratum username is your BTC address; each block is split across the last window of '
        + 'shares once it is 100 blocks deep, and balances are paid on Bitcoin.',
    'pplns-coinbase':
        'The stratum username is your BTC address; the coinbase pays every miner in the PPLNS window '
        + 'directly, so the pool never holds funds.',
};

const trimSlash = (url) => url.replace(/\/+$/, '');

export function buildInfo(meta, p, { version = null } = {}) {
    const mode = meta?.pool_mode ?? null;
    return {
        name:             p.name,
        operator:         p.operator,
        chain:            p.chain ?? meta?.network ?? null,
        mode,
        fee_bps:          meta?.fee_bps ?? null,
        coinbase_tag:     meta?.coinbase_tag ?? null,
        stratum_url:      p.stratumUrl,
        dashboard_url:    p.dashboardUrl,
        status_url:       p.dashboardUrl ? `${trimSlash(p.dashboardUrl)}/api/status` : null,
        slipstream_url:   p.slipstreamUrl,
        operator_address: meta?.operator_address ?? null,
        pool_btc_address: meta?.pool_btc_address ?? null,
        payout:           p.payoutText ?? PAYOUT_TEXT[mode] ?? null,
        software:         'simplepool',
        version,
        logo:             p.logo,
        contact:          p.contact,
    };
}
