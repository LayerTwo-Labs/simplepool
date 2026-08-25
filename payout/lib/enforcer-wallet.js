/* L1 Bitcoin payout rail, for pool_mode = pplns-btc.
 *
 * The pool holds no keys and builds no transactions. The bip300301_enforcer
 * runs with --enable-wallet, the coinbase pays an address from that wallet
 * (WalletService/CreateNewAddress), and paying miners is one RPC:
 * WalletService/SendTransaction takes a destinations map and a fee rate and
 * does the selecting, signing and broadcasting itself.
 *
 * That is the whole reason this file is short. An earlier design had the pool
 * track its own coinbase outpoints, build a BIP174 PSBT, and hand it to the
 * operator to sign offline — several hundred lines of transaction
 * construction whose bugs would be silent and expensive. The enforcer already
 * owns a wallet, so none of that has to exist here.
 *
 * The interface deliberately mirrors ThunderClient's, so payout.js does not
 * care which rail it is driving: same at-most-once protocol, same
 * payouts_in_flight rows, same settle-on-confirm. Only the drain differs.
 */

import { enforcerRpc } from './enforcer-rpc.js';

const SVC = 'cusf.mainchain.v1.WalletService';

/* Sat amounts stay well inside 2^53 (all of Bitcoin is ~2.1e15), but the JSON
 * layer is numbers, so refuse rather than round silently. */
function safeNumber(v, what) {
    const n = BigInt(v);
    if (n > BigInt(Number.MAX_SAFE_INTEGER)) {
        throw new Error(`${what}: ${n} exceeds safe integer range`);
    }
    return Number(n);
}

export class EnforcerWalletClient {
    /* feeRateSatPerVb is passed straight through to the enforcer, which does
     * the fee arithmetic. There is no local estimator to drift out of date. */
    constructor({ addr, feeRateSatPerVb = 5, passphrase = null, timeoutMs = 30_000 }) {
        this.addr = addr;
        this.feeRate = feeRateSatPerVb;
        this.passphrase = passphrase;
        this.timeoutMs = timeoutMs;
        this._unlocked = false;
    }

    async _call(method, body, timeoutMs = this.timeoutMs) {
        return enforcerRpc(this.addr, `${SVC}/${method}`, body, timeoutMs);
    }

    /* An encrypted wallet answers every spend with an error until it is
     * unlocked, so do it once up front rather than discovering it mid-batch.
     * Unlocking is idempotent and cheap; a wallet with no passphrase
     * configured is assumed unencrypted and left alone. */
    async ensureUnlocked() {
        if (this._unlocked || !this.passphrase) return;
        await this._call('UnlockWallet', { password: this.passphrase });
        this._unlocked = true;
    }

    /* Spendable balance in sats. The enforcer reports several buckets; only
     * confirmed money can fund a payout — a coinbase output is not spendable
     * until it is 100 deep, and counting it before then is how a pool
     * promises what it cannot send. */
    async balance() {
        const j = await this._call('GetBalance', {});
        const sats = j.confirmedSats ?? j.confirmed_sats ?? j.confirmed ?? 0;
        return BigInt(Math.floor(Number(sats)));
    }

    /* One transaction for the whole batch. Name and shape match
     * ThunderClient.transferBatchDetailed so payout.js can hold either.
     *
     * The second argument is ignored: Thunder is quoted an absolute fee,
     * whereas the enforcer takes a rate and computes the fee from the
     * transaction it actually builds — which it can do and we cannot, since
     * it is the one selecting the inputs. */
    async transferBatchDetailed(recipients, _feeSatsIgnored) {
        if (!Array.isArray(recipients) || recipients.length === 0) {
            throw new Error('transferBatch: no recipients');
        }
        await this.ensureUnlocked();

        /* Two miners can authorize with the same payout address from
         * different rigs. destinations is keyed by address, so sending the
         * list unmerged would let one entry overwrite the other and pay that
         * miner once for two debts — while the ledger marked both settled. */
        const merged = new Map();
        for (const r of recipients) {
            const sats = BigInt(r.sats);
            if (sats <= 0n) throw new Error('transferBatch: non-positive amount');
            if (!r.address) throw new Error('transferBatch: missing address');
            merged.set(r.address, (merged.get(r.address) || 0n) + sats);
        }

        const destinations = {};
        for (const [addr, sats] of merged) {
            destinations[addr] = safeNumber(sats, `destination ${addr}`);
        }

        const fail = (stage, err) => { err.stage = stage; return err; };
        let j;
        try {
            j = await this._call('SendTransaction', {
                destinations,
                fee_rate: { sat_per_vbyte: this.feeRate },
            });
        } catch (e) { throw fail('create', e); }

        const txid = j.txid?.hex ?? j.txid ?? j.txId ?? null;
        if (!txid) {
            throw fail('create',
                new Error(`SendTransaction returned no txid: ${JSON.stringify(j).slice(0, 200)}`));
        }
        return { txid, recipients: [...merged].map(([address, sats]) => ({ address, sats })) };
    }

    /* Settlement. Returns the shape settlementState() in payout.js expects:
     * confirmed / known / error, where "unknown" must never be read as
     * either confirmation or eviction. */
    async getTransaction(txid) {
        let j;
        try {
            j = await this._call('ListTransactions', {});
        } catch (e) {
            return { confirmed: false, known: false, error: e.message };
        }
        const rows = j.transactions || j.txs || [];
        const hit = rows.find(t => (t.txid?.hex ?? t.txid) === txid);
        if (!hit) return { confirmed: false, known: false, error: null };
        const confs = Number(hit.confirmations ?? hit.confirmationHeight ?? 0);
        return { confirmed: confs > 0, known: true, error: null };
    }

    /* payout.js cross-checks settlement against wallet outputs, because a
     * node that has forgotten a transaction and one that confirmed it long
     * ago look identical from getTransaction alone. */
    async walletUtxos() {
        try {
            const j = await this._call('ListUnspentOutputs', {});
            const rows = j.outputs || j.utxos || [];
            return { ok: true, utxos: rows.map(u => ({ txid: u.txid?.hex ?? u.txid })) };
        } catch (e) {
            return { ok: false, utxos: [], error: e.message };
        }
    }

    /* Thunder only advances when a mainchain block commits to it, so its rail
     * has to nudge mining along. Bitcoin blocks arrive without being asked.
     * Present so payout.js can drive either rail without branching. */
    async mempool()  { return { ok: true, txids: [] }; }
    async mine()     { return { ok: true, skipped: 'l1 needs no nudging' }; }
}
