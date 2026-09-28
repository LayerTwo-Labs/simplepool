/* The fee rule, as Slipstream states it: a tx must pay the higher of the
 * minimum submission rate and the current mineable rate.
 *
 * The mineable rate is read off the template the proxy is mining now. While
 * the template has room, anything over the floor gets in, so the floor is the
 * mineable rate. Once it is full, the cheapest tx it carries is what a new tx
 * has to beat. Per tx, not per package: a cheap parent carried by its child
 * reads lower than it was really priced at, which only ever errs towards
 * accepting.
 */

/* Weight a template can carry: 4M less what the header and a coinbase take.
 * The coinbase is the one piece that varies, and a pplns-coinbase one paying
 * a window of miners is the heaviest, so reserve for that. */
export const TEMPLATE_WEIGHT_CAPACITY = 4_000_000 - 8_000;

/* A template this close to capacity is full: nothing typical fits after it. */
export const FULL_MARGIN_WEIGHT = 4_000;

export const vsizeOf = (weight) => Math.ceil(weight / 4);

/* Rate in sat/vB, rounded to 3 places so a stored rate reads back as it was
 * shown. */
export const feeRate = (feeSats, vsize) => Math.round((feeSats / vsize) * 1000) / 1000;

export function mineableRate(template, floor) {
    const txs = template?.transactions ?? [];
    const used = txs.reduce((w, tx) => w + (tx.weight ?? 0), 0);
    if (txs.length === 0 || used < TEMPLATE_WEIGHT_CAPACITY - FULL_MARGIN_WEIGHT) {
        return floor;
    }
    const cheapest = Math.min(...txs.map(tx => feeRate(tx.fee ?? 0, vsizeOf(tx.weight))));
    return Math.max(floor, cheapest);
}

/* What /api/fees reports, and what a submission is held to. */
export function feeSnapshot(template, floor) {
    const mineable = mineableRate(template, floor);
    return {
        min_submission_rate: floor,
        mineable_rate: mineable,
        required_rate: Math.max(floor, mineable),
        template_height: template?.height ?? null,
        template_weight: (template?.transactions ?? []).reduce((w, tx) => w + (tx.weight ?? 0), 0),
    };
}
