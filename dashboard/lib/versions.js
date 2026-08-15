/* Build provenance for every process the pool depends on.
 *
 * "Which commit is the pool running?" has, until now, only been answerable by
 * SSHing to the box and running `git log` in four directories — and the answer
 * that produces is wrong often enough to matter, because a checkout's HEAD is
 * not what the running binary was built from. A tree gets rebased, patched by
 * hand, or simply moves on since the last build.
 *
 * So the commit is looked for in the artifact first, and in a source tree only
 * as a last resort. Three sources, in descending order of what they prove:
 *
 *   binary    the process states its own build commit. simplepool and the
 *             enforcer embed it; `--version` prints it. Nothing to keep in
 *             sync, nothing to fall out of date — the answer travels inside
 *             the thing it describes. Asked through /proc/<pid>/exe rather
 *             than the configured path, so a component rebuilt but not yet
 *             restarted reports what it is running, not what it is about to
 *             run; the pending build appears separately as `on_disk`.
 *   manifest  a <binary>.build.json written by scripts/record-build.sh at the
 *             moment of the build, pinned to the binary by sha256. This is how
 *             thunder and bitcoind — which print a version number and no
 *             commit — get a trustworthy commit without their source having to
 *             stay on the machine afterwards.
 *   checkout  the git tree beside the binary. Weakest by far: it describes
 *             what is on disk now, not what was compiled. Reported when it is
 *             there, never required, and never silently upgraded to fact.
 *
 * `provenance` on each component says which of the three the answer came from,
 * so a consumer can tell a proof from an inference. Where two sources both
 * know a commit, `commit_matches` cross-checks them; where only one does it is
 * null, never an optimistic true.
 *
 * A component is not required to have a source tree, a manifest, or even to
 * support --version. Each is independently guarded, and a component that can
 * answer nothing reports `error` without taking the other three (or the
 * endpoint) down.
 *
 * Subprocesses and sha256 over a large binary are expensive relative to a 15s
 * dashboard refresh, and the answer only changes on restart or redeploy, so
 * results are cached for VERSIONS_TTL_MS (default 5 min) with in-flight
 * de-duplication.
 */

import { execFile } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const execFileP = promisify(execFile);

const EXEC_TIMEOUT_MS = parseInt(process.env.VERSIONS_EXEC_TIMEOUT_MS || '5000', 10);
const TTL_MS          = parseInt(process.env.VERSIONS_TTL_MS || '300000', 10);

/* The manifest is the supported way to answer for a binary that doesn't embed
 * its commit. Reading a git tree is the fallback, and an operator who has
 * deliberately deployed binaries without source shouldn't have to see it
 * attempted at all: set VERSIONS_USE_CHECKOUT=0 to turn it off. */
const USE_CHECKOUT = process.env.VERSIONS_USE_CHECKOUT !== '0';

/* On this deployment all four checkouts happen to be siblings —
 * .../forknet-software/{simplepool,bip300301_enforcer,thunder-rust,bitcoin} —
 * so the defaults find them with no configuration. They are a convenience,
 * not a requirement: a path that isn't there is simply not a source of
 * evidence, not an error. */
const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const SOFTWARE  = path.resolve(REPO_ROOT, '..');

function env(name, fallback) {
    const v = process.env[name];
    return v === undefined ? fallback : v;
}

export function componentSpecs() {
    const specs = [
        {
            id:    'simplepool',
            label: 'simplepool (stratum proxy)',
            repo:  env('SIMPLEPOOL_REPO_DIR', REPO_ROOT),
            bin:   env('SIMPLEPOOL_BIN',      path.join(REPO_ROOT, 'build', 'simplepool')),
        },
        {
            id:    'enforcer',
            label: 'bip300301_enforcer',
            repo:  env('ENFORCER_REPO_DIR', path.join(SOFTWARE, 'bip300301_enforcer')),
            bin:   env('ENFORCER_BIN',      path.join(SOFTWARE, 'bip300301_enforcer', 'target', 'release', 'bip300301_enforcer')),
        },
        {
            id:    'thunder',
            label: 'thunder',
            repo:  env('THUNDER_REPO_DIR', path.join(SOFTWARE, 'thunder-rust')),
            bin:   env('THUNDER_BIN',      path.join(SOFTWARE, 'thunder-rust', 'target', 'release', 'thunder_app')),
        },
        {
            id:    'bitcoind',
            label: 'bitcoind (mainchain node)',
            repo:  env('BITCOIN_REPO_DIR', path.join(SOFTWARE, 'bitcoin')),
            bin:   env('BITCOIND_BIN',     path.join(SOFTWARE, 'bitcoin', 'build', 'bin', 'bitcoind')),
        },
    ];
    return specs
        .map(s => ({
            ...s,
            /* Defaults to sitting beside the binary so it survives the source
             * being deleted, and travels with the binary if it is copied. */
            manifest: env(`${s.id.toUpperCase()}_BUILD_MANIFEST`,
                          s.bin ? `${s.bin}.build.json` : ''),
        }))
        .filter(c => c.repo || c.bin);
}

/* --- git ---------------------------------------------------------------- */

async function git(dir, args) {
    const { stdout } = await execFileP('git', ['-C', dir, ...args], {
        timeout: EXEC_TIMEOUT_MS,
        maxBuffer: 1 << 20,
        /* Never take a lock or prompt for credentials: this runs on a web
         * request against a tree another process may be building from. */
        env: { ...process.env, GIT_OPTIONAL_LOCKS: '0', GIT_TERMINAL_PROMPT: '0' },
    });
    return stdout.trim();
}

/* github.com:owner/repo.git and https://github.com/owner/repo.git both become
 * https://github.com/owner/repo, so the commit can be linked. Credentials in a
 * remote URL are stripped — this JSON is public. */
export function normalizeRemote(url) {
    if (!url) return null;
    let u = url.trim().replace(/\.git$/, '');
    const scp = u.match(/^[^@/]+@([^:]+):(.+)$/);
    if (scp) u = `https://${scp[1]}/${scp[2]}`;
    u = u.replace(/^(https?:\/\/)[^@/]+@/, '$1');
    return u;
}

export function commitUrl(remote, commit) {
    if (!remote || !commit) return null;
    if (!/^https:\/\/(github\.com|gitlab\.com)\//.test(remote)) return null;
    return `${remote}/commit/${commit}`;
}

async function checkoutInfo(dir) {
    /* %x1f (unit separator) can't appear in a subject line, so one call gets
     * commit, commit date and subject without re-running git per field. */
    const [branch, show, remote, dirty] = await Promise.all([
        git(dir, ['rev-parse', '--abbrev-ref', 'HEAD']),
        git(dir, ['show', '-s', '--format=%H%x1f%cI%x1f%s', 'HEAD']),
        git(dir, ['config', '--get', 'remote.origin.url']).catch(() => ''),
        git(dir, ['status', '--porcelain', '--untracked-files=no']).catch(() => ''),
    ]);
    const [commit, committedAt, subject] = show.split('\x1f');
    const dirtyFiles = dirty ? dirty.split('\n').filter(Boolean) : [];
    const url = normalizeRemote(remote);
    return {
        repo:         url,
        branch:       branch === 'HEAD' ? null : branch,   /* detached */
        detached:     branch === 'HEAD',
        commit,
        commit_short: commit ? commit.slice(0, 7) : null,
        commit_time:  committedAt || null,
        subject:      subject || null,
        /* Tracked modifications only. Untracked files are excluded on purpose:
         * build output and scratch files litter a working server and would pin
         * this to "dirty" forever, drowning the signal that matters — that
         * tracked source differs from the commit named above. */
        dirty:        dirtyFiles.length > 0,
        dirty_files:  dirtyFiles.length,
        commit_url:   commitUrl(url, commit),
    };
}

/* --- the build manifest ------------------------------------------------- */

async function sha256(file) {
    const h = crypto.createHash('sha256');
    for await (const chunk of fs.createReadStream(file)) h.update(chunk);
    return h.digest('hex');
}

/* A manifest is only evidence about the binary it was recorded for. Rebuild
 * without re-recording and it silently describes the previous artifact — so
 * it is pinned by sha256 and reported as unverified when the hash moved. An
 * unverified manifest is still shown, because "the manifest is stale" is
 * itself the finding. */
async function manifestInfo(file, bin) {
    const raw = JSON.parse(await fsp.readFile(file, 'utf8'));
    let verified = null;
    if (raw.binary_sha256 && bin) {
        verified = await sha256(bin).then(h => h === raw.binary_sha256)
                                    .catch(() => null);
    }
    const url = normalizeRemote(raw.repo);
    return {
        repo:         url,
        branch:       raw.branch || null,
        commit:       raw.commit || null,
        commit_short: raw.commit ? raw.commit.slice(0, 7) : null,
        commit_time:  raw.commit_time || null,
        subject:      raw.subject || null,
        dirty:        raw.dirty ?? null,
        recorded_at:  raw.recorded_at || null,
        /* false: this manifest describes a different binary than the one on
         * disk — the component was rebuilt without re-recording. */
        verified,
        commit_url:   commitUrl(url, raw.commit),
    };
}

/* --- the running process ------------------------------------------------ */

/* Boot time in unix seconds, for turning /proc/<pid>/stat's starttime (which
 * counts clock ticks since boot) into a wall-clock timestamp. */
async function bootTimeSec() {
    const stat = await fsp.readFile('/proc/stat', 'utf8');
    const m = stat.match(/^btime\s+(\d+)/m);
    return m ? parseInt(m[1], 10) : null;
}

async function processStartSec(pid, btime) {
    if (btime == null) return null;
    const raw = await fsp.readFile(`/proc/${pid}/stat`, 'utf8');
    /* Field 2 (comm) is parenthesised and may itself contain spaces, so split
     * after the last ')' rather than on whitespace from the start. */
    const rest = raw.slice(raw.lastIndexOf(')') + 2).split(' ');
    const ticks = parseInt(rest[19], 10);            /* field 22, 1-indexed */
    if (!Number.isFinite(ticks)) return null;
    return btime + Math.floor(ticks / 100);          /* USER_HZ is 100 on Linux */
}

/* Find the live process running `bin`. /proc/<pid>/exe follows the inode, so a
 * binary replaced by a rebuild since the process started reads back as
 * "<path> (deleted)" — which is exactly the drift worth reporting. Linux only;
 * elsewhere (a macOS dev box) this is simply unavailable. */
async function runningProcess(bin) {
    if (!bin || !fs.existsSync('/proc')) return null;
    let pids;
    try {
        pids = (await fsp.readdir('/proc')).filter(n => /^\d+$/.test(n));
    } catch { return null; }

    const btime = await bootTimeSec().catch(() => null);
    for (const pid of pids) {
        let exe;
        /* EACCES for another user's process, ENOENT for one that exited
         * between readdir and readlink. Both mean "not ours", not an error. */
        try { exe = await fsp.readlink(`/proc/${pid}/exe`); } catch { continue; }
        const replaced = exe.endsWith(' (deleted)');
        const target   = replaced ? exe.slice(0, -' (deleted)'.length) : exe;
        if (target !== bin) continue;
        return {
            pid:             parseInt(pid, 10),
            started_at:      await processStartSec(pid, btime).catch(() => null),
            /* true: the file at this path is no longer the one running — it
             * was rebuilt or replaced and the service was never restarted. */
            binary_replaced: replaced,
        };
    }
    return null;
}

/* --- the binary's own version ------------------------------------------- */

/* A binary predating --version support does not fail cleanly: simplepool
 * builds before this endpoint existed treat the flag as a config path and
 * print "config error: cannot open config '--version'". Reporting that first
 * line as the version would be worse than reporting nothing, so the output has
 * to earn the label — a non-zero exit, or a first line with no version number
 * in it, is a component that cannot state its own version. */
export async function selfReportedVersion(bin, label = null) {
    let out, failed = false;
    try {
        const r = await execFileP(bin, ['--version'], {
            timeout: EXEC_TIMEOUT_MS, maxBuffer: 1 << 20,
        });
        out = `${r.stdout}${r.stderr}`;
    } catch (e) {
        /* ENOENT/EACCES carry no output and mean the path is wrong — a
         * different failure from "ran, but doesn't understand the flag". */
        out = `${e.stdout || ''}${e.stderr || ''}`;
        if (!out.trim()) throw e;
        failed = true;
    }
    const lines = out.split('\n').map(s => s.trim()).filter(Boolean);
    const first = lines[0] || '';
    /* Every daemon in this stack prints its identity first and details after:
     *   "Bitcoin Core (eCash drynet4) daemon version v31.1.0 bitcoind"
     *   "bip300301_enforcer_lib v0.3.4" / " commit: 7b005cc"
     *   "thunder_app 0.17.2"
     *   "simplepool 0.1.0" / " commit: <sha>" / " branch: main"     */
    if (failed || !/\d+\.\d+/.test(first)) {
        /* Tagged so the caller can tell "this binary has no --version" — where
         * retrying against a different path is pointless — from a spawn error
         * like EACCES, where the fallback path is worth trying. */
        const err = new Error(`${label || path.basename(bin)} does not report --version` +
                              (first ? ` (said: ${first.slice(0, 80)})` : ''));
        err.code = 'NO_VERSION_FLAG';
        throw err;
    }
    const commit = out.match(/\bcommit:?\s*([0-9a-f]{7,40})\b/i);
    const branch = out.match(/\bbranch:?\s*(\S+)/i);
    return {
        version:      first,
        build_commit: commit ? commit[1] : null,
        build_branch: branch && branch[1] !== 'unknown' ? branch[1] : null,
        dirty:        /\bdirty\b/i.test(out) || null,
    };
}

/* Ask the *running* process for its version, not the file at the configured
 * path — during a deploy those are two different programs.
 *
 * /proc/<pid>/exe resolves to the inode the process is executing, which stays
 * readable and executable after the file has been unlinked or replaced by a
 * rebuild. Without this, a component rebuilt but not yet restarted would be
 * reported at the version it is *about* to run, which is exactly backwards
 * during the window when someone is most likely to be reading this.
 *
 * Falls back to the configured path when there is no live process, when /proc
 * isn't there (a macOS dev box), or when exec'ing through /proc is refused. */
async function runningVersion(spec, proc) {
    if (proc) {
        try {
            const v = await selfReportedVersion(`/proc/${proc.pid}/exe`,
                                                path.basename(spec.bin));
            return { ...v, source: 'process' };
        } catch (e) {
            /* No --version support is a property of the program, not of how we
             * reached it, so re-running against the path would fail the same
             * way for the same reason. Anything else is worth one retry. */
            if (e.code === 'NO_VERSION_FLAG') throw e;
        }
    }
    const v = await selfReportedVersion(spec.bin);
    return { ...v, source: proc ? 'path_fallback' : 'path' };
}

/* --- one component ------------------------------------------------------ */

/* Two commits agree if either is a prefix of the other: binaries print 7
 * characters, git stores 40. */
function sameCommit(a, b) {
    if (!a || !b) return null;
    return a.startsWith(b) || b.startsWith(a);
}

export async function describeComponent(spec) {
    /* The process has to be located before anything is exec'd, because which
     * program to ask depends on it — see runningVersion(). */
    const proc = spec.bin ? await runningProcess(spec.bin).catch(() => null) : null;

    const [binStat, self, onDisk, manifest, checkout] = await Promise.all([
        spec.bin ? fsp.stat(spec.bin).catch(() => null) : null,
        spec.bin ? runningVersion(spec, proc).then(v => ({ v }), e => ({ e })) : null,
        /* Only worth a second exec when the two are known to differ: the file
         * has been replaced since the process started, so this is the version
         * a restart would pick up. */
        spec.bin && proc?.binary_replaced
            ? selfReportedVersion(spec.bin).then(v => ({ v }), e => ({ e })) : null,
        spec.manifest ? manifestInfo(spec.manifest, spec.bin).then(v => ({ v }), e => ({ e })) : null,
        USE_CHECKOUT && spec.repo ? checkoutInfo(spec.repo).then(v => ({ v }), e => ({ e })) : null,
    ]);

    const out = {
        id: spec.id, label: spec.label,
        version: null, repo: null, branch: null,
        commit: null, commit_short: null, commit_url: null,
        /* Which of the three sources the resolved commit above came from —
         * 'binary' and 'manifest' are claims about the artifact, 'checkout' is
         * a claim about a directory. Null when no commit could be found. */
        provenance: null,
        running: null, on_disk: null, manifest: null, checkout: null,
        commit_matches: null, notes: [], error: null,
    };

    /* Process facts are reported whenever the binary exists, even if it can't
     * state a version: "this pid, started then, still running the file on
     * disk" stands on its own, and a component that half-answers should lose
     * only the half it failed. */
    if (spec.bin) {
        const v = self?.v;
        out.version = v ? v.version : null;
        out.running = {
            version:               v ? v.version : null,
            build_commit:          v ? v.build_commit : null,
            build_commit_short:    v?.build_commit ? v.build_commit.slice(0, 7) : null,
            built_from_dirty_tree: v ? v.dirty : null,
            /* 'process' — read from /proc/<pid>/exe, so it describes the live
             * process. 'path' — no process found, so this is the file that
             * would run. 'path_fallback' — a process exists but could not be
             * asked, so this describes the file and may not be what is
             * running. Only 'process' is a statement about the running code. */
            version_source:        v ? v.source : null,
            /* Of the file at the configured path, which is not necessarily the
             * program described above — see binary_replaced. */
            binary_mtime:          binStat ? Math.floor(binStat.mtimeMs / 1000) : null,
            binary_size:           binStat ? binStat.size : null,
            pid:                   proc ? proc.pid : null,
            started_at:            proc ? proc.started_at : null,
            /* false: no live process is running this binary. Either the
             * service is down or it is running a copy from elsewhere. */
            process_found:         Boolean(proc),
            binary_replaced:       proc ? proc.binary_replaced : null,
        };
        /* Not an error on its own — thunder and bitcoind simply don't embed a
         * commit, which is what the manifest exists to cover. */
        if (self?.e) out.notes.push(self.e.message);

        /* The one combination where the version above is not a claim about the
         * running process: the file was replaced, and /proc could not be used
         * to reach what is actually executing. */
        if (v?.source === 'path_fallback' && proc?.binary_replaced) {
            out.notes.push('the running process could not be queried directly; ' +
                           'this version describes the file on disk, which has ' +
                           'been replaced since that process started');
        }

        /* Present only mid-deploy: what a restart would start running. Makes
         * "rebuilt but not restarted" legible as two concrete versions rather
         * than a single flag. */
        if (onDisk?.v) {
            out.on_disk = {
                version:            onDisk.v.version,
                build_commit:       onDisk.v.build_commit,
                build_commit_short: onDisk.v.build_commit
                                        ? onDisk.v.build_commit.slice(0, 7) : null,
                mtime:              binStat ? Math.floor(binStat.mtimeMs / 1000) : null,
            };
            out.notes.push('rebuilt but not restarted — the running process is ' +
                           'still the previous build; restart to pick up ' +
                           `${out.on_disk.build_commit_short || out.on_disk.version}`);
        }
    }

    if (manifest?.v) out.manifest = manifest.v;
    else if (manifest?.e && manifest.e.code !== 'ENOENT') {
        out.notes.push(`manifest: ${manifest.e.message}`);
    }

    if (checkout?.v) out.checkout = checkout.v;
    else if (checkout?.e) {
        /* A missing source tree is a normal deployment, not a fault: binaries
         * plus manifests are enough. Only say something when a tree was
         * expected to be readable and wasn't. */
        if (!/not a git repository|ENOENT|does not exist/i.test(checkout.e.message)) {
            out.notes.push(`checkout: ${checkout.e.message}`);
        }
    }

    /* Resolve, best evidence first. */
    const fromBinary = out.running?.build_commit
        ? { commit: out.running.build_commit,
            branch: self?.v?.build_branch || null,
            repo:   out.manifest?.repo || out.checkout?.repo || null,
            dirty:  out.running.built_from_dirty_tree,
            provenance: 'binary' }
        : null;
    /* A manifest whose hash no longer matches the binary describes a previous
     * build, so it ranks below the checkout rather than above it. */
    const fromManifest = out.manifest?.commit && out.manifest.verified !== false
        ? { ...out.manifest, provenance: 'manifest' } : null;
    const fromCheckout = out.checkout?.commit
        ? { ...out.checkout, provenance: 'checkout' } : null;

    const best = fromBinary || fromManifest || fromCheckout;
    if (best) {
        out.commit       = best.commit;
        out.commit_short = best.commit.slice(0, 7);
        out.branch       = best.branch ?? null;
        out.repo         = best.repo ?? out.manifest?.repo ?? out.checkout?.repo ?? null;
        out.provenance   = best.provenance;
        out.commit_url   = best.commit_url || commitUrl(out.repo, best.commit);
        if (out.provenance === 'checkout') {
            out.notes.push('commit read from the source tree on disk — it is ' +
                           'what would be built now, not proof of what is running');
        }
    } else {
        out.error = out.notes.length ? out.notes.join('; ')
                                     : 'no build provenance available';
    }

    /* Cross-check whatever pairs exist. Comparing the artifact to the tree is
     * the one that catches "someone pulled and never rebuilt". */
    const pairs = [
        sameCommit(out.running?.build_commit, out.checkout?.commit),
        sameCommit(out.running?.build_commit, out.manifest?.commit),
        sameCommit(out.manifest?.commit,      out.checkout?.commit),
    ].filter(v => v !== null);
    if (pairs.length) out.commit_matches = pairs.every(Boolean);

    return out;
}

/* --- public API --------------------------------------------------------- */

let cache    = null;   /* { at: ms, value } */
let inFlight = null;

async function collect() {
    const specs = componentSpecs();
    const components = await Promise.all(specs.map(s =>
        describeComponent(s).catch(e => ({
            id: s.id, label: s.label, version: null, commit: null,
            provenance: null, running: null, manifest: null, checkout: null,
            commit_matches: null, notes: [], error: e.message,
        }))));
    return summarize(components);
}

/* One flag an operator can alert on without reading four sub-objects.
 * "Clean" is a narrow claim: every component named a commit, is running the
 * binary still on disk, was built from a committed tree, and — where two
 * sources could be compared — they agreed. Anything else lands in
 * needs_review, which is a prompt to look rather than an outage. */
export function summarize(components) {
    const suspect = components.filter(c =>
        c.error ||
        !c.commit ||
        c.commit_matches === false ||
        c.manifest?.verified === false ||
        c.running?.binary_replaced ||
        c.running?.process_found === false ||
        c.running?.built_from_dirty_tree ||
        c.checkout?.dirty);
    return {
        checked_at:   Math.floor(Date.now() / 1000),
        all_clean:    suspect.length === 0,
        needs_review: suspect.map(c => c.id),
        components,
    };
}

/* Cached; pass { force: true } to bypass (the admin page after a redeploy). */
export async function versions({ force = false } = {}) {
    if (!force && cache && Date.now() - cache.at < TTL_MS) return cache.value;
    if (inFlight) return inFlight;                 /* de-dupe concurrent hits */
    inFlight = collect()
        .then(v => { cache = { at: Date.now(), value: v }; return v; })
        .finally(() => { inFlight = null; });
    return inFlight;
}

/* The last snapshot without triggering a collection — for render paths that
 * must not block on subprocesses and a sha256. Null until something has
 * called versions() at least once. */
export function currentVersions() {
    return cache ? cache.value : null;
}
