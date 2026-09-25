import { test } from 'node:test';
import assert from 'node:assert/strict';

import { buildInfo, PAYOUT_TEXT } from '../lib/info.js';

const META = {
    pool_mode: 'pplns-coinbase',
    fee_bps: 100,
    network: 'main',
    coinbase_tag: '/bip300xyz/',
    operator_address: 'bc1qljvzxk0tp6qtrunt590z5rtdhs8jhkkn4ny4rm',
    pool_btc_address: null,
};

const PRESENTATION = {
    name: 'bip300.xyz',
    operator: 'bip300.xyz',
    chain: 'betanet',
    logo: 'mining-pool-logos/bip300xyz.svg',
    contact: null,
    payoutText: null,
    stratumUrl: 'stratum+tcp://stratum.beta.bip300.xyz:3334',
    dashboardUrl: 'https://pool.beta.bip300.xyz/',
    slipstreamUrl: 'https://slipstream.beta.bip300.xyz',
};

test('matches the shape pool directories expect', () => {
    const info = buildInfo(META, PRESENTATION);
    assert.deepEqual(info, {
        name: 'bip300.xyz',
        operator: 'bip300.xyz',
        chain: 'betanet',
        mode: 'pplns-coinbase',
        fee_bps: 100,
        coinbase_tag: '/bip300xyz/',
        stratum_url: 'stratum+tcp://stratum.beta.bip300.xyz:3334',
        dashboard_url: 'https://pool.beta.bip300.xyz/',
        status_url: 'https://pool.beta.bip300.xyz/api/status',
        slipstream_url: 'https://slipstream.beta.bip300.xyz',
        operator_address: 'bc1qljvzxk0tp6qtrunt590z5rtdhs8jhkkn4ny4rm',
        pool_btc_address: null,
        payout: PAYOUT_TEXT['pplns-coinbase'],
        software: 'simplepool',
        version: null,
        logo: 'mining-pool-logos/bip300xyz.svg',
        contact: null,
    });
});

test('facts come from pool_meta, the exact mode included', () => {
    const info = buildInfo({ ...META, pool_mode: 'pplns-btc', fee_bps: 250 }, PRESENTATION);
    assert.equal(info.mode, 'pplns-btc');
    assert.equal(info.fee_bps, 250);
    assert.equal(info.payout, PAYOUT_TEXT['pplns-btc']);
});

test('every mode has a payout sentence, and it can be overridden', () => {
    for (const mode of ['solo', 'pps-classic', 'pplns-thunder', 'pplns-btc', 'pplns-coinbase']) {
        assert.ok(buildInfo({ ...META, pool_mode: mode }, PRESENTATION).payout, mode);
    }
    const info = buildInfo(META, { ...PRESENTATION, payoutText: 'custom' });
    assert.equal(info.payout, 'custom');
});

test('chain falls back to the network, and missing pool_meta reads as unknown', () => {
    assert.equal(buildInfo(META, { ...PRESENTATION, chain: null }).chain, 'main');
    const info = buildInfo(null, { ...PRESENTATION, chain: null, dashboardUrl: null });
    assert.equal(info.mode, null);
    assert.equal(info.fee_bps, null);
    assert.equal(info.chain, null);
    assert.equal(info.status_url, null);
    assert.equal(info.payout, null);
});
