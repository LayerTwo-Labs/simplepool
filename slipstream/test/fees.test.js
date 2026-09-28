import { test } from 'node:test';
import assert from 'node:assert/strict';

import { feeSnapshot, mineableRate } from '../lib/fees.js';
import { fullTemplate } from './helpers.js';

test('a template with room is mineable at the floor', () => {
    const template = fullTemplate({ feeRate: 50, count: 2 });
    assert.equal(mineableRate(template, 1), 1);
    assert.equal(mineableRate({ transactions: [] }, 1), 1);
    assert.equal(mineableRate(null, 1), 1);
});

test('a full template is mineable at its cheapest tx', () => {
    const template = fullTemplate({ feeRate: 3 });
    template.transactions[4].fee = 5 * 100_000;   // 5 sat/vB, dearer than the rest
    assert.equal(mineableRate(template, 1), 3);
});

test('the required rate is the higher of floor and mineable', () => {
    assert.equal(feeSnapshot(fullTemplate({ feeRate: 3 }), 1).required_rate, 3);
    assert.equal(feeSnapshot(fullTemplate({ feeRate: 3 }), 10).required_rate, 10);
    const snap = feeSnapshot(fullTemplate({ feeRate: 50, count: 1 }), 1);
    assert.deepEqual(
        [snap.min_submission_rate, snap.mineable_rate, snap.required_rate],
        [1, 1, 1],
    );
});
