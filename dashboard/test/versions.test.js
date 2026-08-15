/* Build provenance.
 *
 * The whole point of lib/versions.js is that a checkout's HEAD is not proof of
 * what is running, so these tests are built around the cases where the sources
 * disagree: a binary built from an older commit, a manifest left behind by a
 * rebuild, a tree with uncommitted changes, a deployment with no source at all.
 * Fixtures are real — real git repos, real executables, real manifests written
 * by the real script — because the parsing and the plumbing are most of what
 * could break.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { describeComponent, summarize, normalizeRemote } from '../lib/versions.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const RECORD_BUILD = path.resolve(__dirname, '../../scripts/record-build.sh');

function tmpdir(tag) {
    return fs.mkdtempSync(path.join(os.tmpdir(), `sp-versions-${tag}-`));
}

/* A git repo with one commit. Identity is set locally so the test doesn't
 * depend on (or touch) the machine's git config. */
function makeRepo({ remote = 'git@github.com:LayerTwo-Labs/simplepool.git' } = {}) {
    const dir = tmpdir('repo');
    const git = (...args) => execFileSync('git', ['-C', dir, ...args], {
        env: { ...process.env, GIT_AUTHOR_NAME: 't', GIT_AUTHOR_EMAIL: 't@e',
               GIT_COMMITTER_NAME: 't', GIT_COMMITTER_EMAIL: 't@e' },
        stdio: 'pipe',
    }).toString().trim();
    execFileSync('git', ['init', '-q', '-b', 'main', dir]);
    fs.writeFileSync(path.join(dir, 'file.txt'), 'one\n');
    git('add', '.');
    git('commit', '-q', '-m', 'first commit');
    if (remote) git('remote', 'add', 'origin', remote);
    return { dir, git, head: git('rev-parse', 'HEAD') };
}

/* An executable that prints what the real daemon prints for --version. */
function makeBin(output) {
    const dir  = tmpdir('bin');
    const file = path.join(dir, 'fake-daemon');
    fs.writeFileSync(file, `#!/bin/sh\ncat <<'EOF'\n${output}\nEOF\n`);
    fs.chmodSync(file, 0o755);
    return file;
}

/* Uses the real recorder, so the test also covers the file it writes. */
function record(component, repoDir, bin) {
    execFileSync(RECORD_BUILD, [component, repoDir, bin], { stdio: 'pipe' });
    return `${bin}.build.json`;
}

const spec = (o) => ({ id: 'c', label: 'c', repo: '', bin: '', manifest: '', ...o });

/* --- the binary speaking for itself -------------------------------------- */

test('a binary that names its own commit is believed over everything else', async () => {
    const repo = makeRepo();
    const bin  = makeBin(`bip300301_enforcer_lib v0.3.4\n commit: ${repo.head.slice(0, 7)}\n binary: bip300301_enforcer`);

    const c = await describeComponent(spec({ id: 'enforcer', repo: repo.dir, bin }));

    assert.equal(c.error, null);
    assert.equal(c.provenance, 'binary');
    assert.equal(c.version, 'bip300301_enforcer_lib v0.3.4');
    assert.equal(c.commit, repo.head.slice(0, 7));
    /* An abbreviated build commit still has to match the full checkout hash —
     * the enforcer prints 7 characters and the tree stores 40. */
    assert.equal(c.commit_matches, true);
});

test('a binary built from a different commit than the tree is flagged', async () => {
    const repo = makeRepo();
    const bin  = makeBin('bip300301_enforcer_lib v0.3.4\n commit: deadbee\n binary: bip300301_enforcer');

    const c = await describeComponent(spec({ id: 'enforcer', repo: repo.dir, bin }));

    assert.equal(c.commit_matches, false);
    assert.equal(c.commit, 'deadbee', 'the artifact wins; the tree is the one that moved');
    assert.deepEqual(summarize([c]).needs_review, ['enforcer']);
});

/* --- the manifest, for binaries that cannot ------------------------------ */

test('a manifest answers for a binary that prints no commit', async () => {
    /* thunder and bitcoind do this: a version number and nothing else. */
    const repo = makeRepo({ remote: 'https://github.com/LayerTwo-Labs/thunder-rust.git' });
    const bin  = makeBin('thunder_app 0.17.2');
    const manifest = record('thunder', repo.dir, bin);

    const c = await describeComponent(spec({ id: 'thunder', repo: repo.dir, bin, manifest }));

    assert.equal(c.provenance, 'manifest');
    assert.equal(c.commit, repo.head);
    assert.equal(c.branch, 'main');
    assert.equal(c.manifest.verified, true, 'sha256 pins the manifest to this binary');
    assert.equal(c.commit_url,
                 `https://github.com/LayerTwo-Labs/thunder-rust/commit/${repo.head}`);
});

test('the manifest works with the source deleted — that is the point of it', async () => {
    const repo = makeRepo();
    const bin  = makeBin('thunder_app 0.17.2');
    const manifest = record('thunder', repo.dir, bin);
    fs.rmSync(repo.dir, { recursive: true, force: true });

    const c = await describeComponent(spec({ id: 'thunder', repo: repo.dir, bin, manifest }));

    assert.equal(c.error, null, 'a missing source tree is a deployment, not a fault');
    assert.equal(c.checkout, null);
    assert.equal(c.provenance, 'manifest');
    assert.equal(c.commit, repo.head);
    /* Nothing is actually running this fixture, which is the only thing left
     * for the summary to object to — the absent source contributes nothing. */
    assert.equal(summarize([{ ...c, running: { ...c.running, process_found: true } }]).all_clean,
                 true, 'a source-less deployment is a clean deployment');
});

test('a manifest left behind by a rebuild is caught, not trusted', async () => {
    const repo = makeRepo();
    const bin  = makeBin('thunder_app 0.17.2');
    const manifest = record('thunder', repo.dir, bin);

    /* Rebuild without re-recording: same path, different bytes. */
    fs.writeFileSync(bin, '#!/bin/sh\necho "thunder_app 0.17.3"\n');
    fs.chmodSync(bin, 0o755);

    const c = await describeComponent(spec({ id: 'thunder', repo: repo.dir, bin, manifest }));

    assert.equal(c.manifest.verified, false);
    /* A stale manifest must not outrank the tree: it describes a binary that
     * no longer exists. */
    assert.notEqual(c.provenance, 'manifest');
    assert.deepEqual(summarize([c]).needs_review, ['thunder']);
});

test('no manifest and no embedded commit falls back to the tree, and says so', async () => {
    const repo = makeRepo();
    const c = await describeComponent(spec({
        id: 'thunder', repo: repo.dir, bin: makeBin('thunder_app 0.17.2'),
    }));

    assert.equal(c.provenance, 'checkout');
    assert.equal(c.commit, repo.head);
    assert.equal(c.commit_matches, null, 'nothing was cross-checked, so nothing is confirmed');
    assert.ok(c.notes.some(n => /not proof of what is running/.test(n)));
});

/* --- the running process vs the file at that path ------------------------ */

/* Needs a real process running a real binary, so /proc/<pid>/exe has something
 * to resolve. Linux only — on a macOS dev box the code falls back to the
 * configured path and there is nothing to assert. */
const HAS_PROC = fs.existsSync('/proc/self/exe');

test('the version comes from the running process, not the file that replaced it',
     { skip: HAS_PROC ? false : 'requires /proc' }, async () => {
    /* `sleep --version` prints "sleep (GNU coreutils) 9.x" — a real binary
     * that stays alive and reports a parseable version, which a shell script
     * cannot do (its /proc/<pid>/exe would resolve to the shell). */
    const dir = tmpdir('replaced');
    const bin = path.join(dir, 'daemon');
    fs.copyFileSync('/bin/sleep', bin);

    const child = spawn(bin, ['300'], { stdio: 'ignore' });
    try {
        for (let i = 0; i < 50 && !fs.existsSync(`/proc/${child.pid}/exe`); i++) {
            await sleep(20);
        }

        /* Rebuild-in-place: same path, different program. The live process
         * keeps executing the old inode. */
        fs.rmSync(bin);
        fs.writeFileSync(bin, '#!/bin/sh\necho "daemon 99.9.9"\necho " commit: feedface"\n');
        fs.chmodSync(bin, 0o755);

        const c = await describeComponent(spec({ id: 'thunder', bin }));

        assert.equal(c.running.pid, child.pid);
        assert.equal(c.running.binary_replaced, true);
        assert.equal(c.running.version_source, 'process');
        /* The point of the whole exercise: what is running, not what is
         * staged to run. */
        assert.match(c.running.version, /^sleep /);
        assert.equal(c.running.build_commit, null);
        /* And the pending build is reported alongside rather than instead. */
        assert.equal(c.on_disk.version, 'daemon 99.9.9');
        assert.equal(c.on_disk.build_commit, 'feedface');
        assert.ok(c.notes.some(n => /rebuilt but not restarted/.test(n)));
    } finally {
        child.kill('SIGKILL');
    }
});

test('with no process running it, the file at the path is the answer',
     { skip: HAS_PROC ? false : 'requires /proc' }, async () => {
    const c = await describeComponent(spec({ id: 'thunder', bin: makeBin('thunder_app 0.17.2') }));

    assert.equal(c.running.process_found, false);
    assert.equal(c.running.version_source, 'path');
    assert.equal(c.running.version, 'thunder_app 0.17.2');
    assert.equal(c.on_disk, null, 'nothing pending when nothing is running');
});

/* --- dirty trees, bad URLs, missing pieces ------------------------------- */

test('a dirty tree is reported, and untracked files alone are not dirty', async () => {
    const repo = makeRepo();
    const bin  = makeBin('thunder_app 0.17.2');

    /* Untracked build output litters a working server; if that counted, the
     * flag would be permanently on and would say nothing. */
    fs.writeFileSync(path.join(repo.dir, 'scratch.log'), 'noise\n');
    let c = await describeComponent(spec({ id: 'thunder', repo: repo.dir, bin }));
    assert.equal(c.checkout.dirty, false);

    /* A tracked file edited in place means the commit above no longer
     * describes the source the binary was built from. */
    fs.writeFileSync(path.join(repo.dir, 'file.txt'), 'patched by hand\n');
    c = await describeComponent(spec({ id: 'thunder', repo: repo.dir, bin }));
    assert.equal(c.checkout.dirty, true);
    assert.equal(c.checkout.dirty_files, 1);
    assert.deepEqual(summarize([c]).needs_review, ['thunder']);
});

test('a build from a dirty tree is recorded as dirty in the manifest', async () => {
    const repo = makeRepo();
    fs.writeFileSync(path.join(repo.dir, 'file.txt'), 'hotfix\n');
    const bin = makeBin('thunder_app 0.17.2');
    const manifest = record('thunder', repo.dir, bin);

    const c = await describeComponent(spec({ id: 'thunder', repo: repo.dir, bin, manifest }));
    assert.equal(c.manifest.dirty, true,
                 'the commit alone would misrepresent a hand-patched build');
});

test('an ssh remote is normalised, and credentials never reach the public JSON', async () => {
    assert.equal(normalizeRemote('git@github.com:LayerTwo-Labs/simplepool.git'),
                 'https://github.com/LayerTwo-Labs/simplepool');

    const repo = makeRepo({ remote: 'https://someuser:tokenvalue@github.com/LayerTwo-Labs/simplepool.git' });
    const c = await describeComponent(spec({
        id: 'simplepool', repo: repo.dir, bin: makeBin('simplepool 0.1.0'),
    }));

    assert.equal(c.checkout.repo, 'https://github.com/LayerTwo-Labs/simplepool');
    assert.ok(!JSON.stringify(c).includes('tokenvalue'));
});

test('a binary that does not understand --version is not quoted as one', async () => {
    /* A simplepool build predating the flag prints a config error and exits
     * non-zero. Reporting that first line as "the version" would be worse than
     * reporting nothing. */
    const repo = makeRepo();
    const dir  = tmpdir('oldbin');
    const bin  = path.join(dir, 'old-simplepool');
    fs.writeFileSync(bin, `#!/bin/sh\necho "config error: cannot open config '--version'" >&2\nexit 2\n`);
    fs.chmodSync(bin, 0o755);

    const c = await describeComponent(spec({ id: 'simplepool', repo: repo.dir, bin }));

    assert.equal(c.version, null);
    assert.equal(c.running.version, null);
    assert.ok(c.notes.some(n => /does not report --version/.test(n)));
    /* The tree still answers, at checkout confidence. */
    assert.equal(c.provenance, 'checkout');
});

test('a component with nothing to say reports an error rather than guessing', async () => {
    const c = await describeComponent(spec({
        id: 'bitcoind', repo: '/nonexistent/repo', bin: '/nonexistent/bitcoind',
    }));

    assert.equal(c.commit, null);
    assert.equal(c.provenance, null);
    assert.ok(c.error);
    assert.equal(c.running.process_found, false);
    assert.equal(summarize([c]).all_clean, false);
});

test('all_clean only when every component named a commit and nothing disagreed', () => {
    const clean = {
        id: 'x', error: null, commit: 'abc1234', commit_matches: true,
        running: { process_found: true, binary_replaced: false, built_from_dirty_tree: null },
        manifest: { verified: true }, checkout: { dirty: false },
    };
    assert.equal(summarize([clean]).all_clean, true);

    /* A binary replaced on disk since the process started: the service is
     * running code that is no longer at that path. */
    assert.equal(summarize([{ ...clean,
        running: { ...clean.running, binary_replaced: true } }]).all_clean, false);

    /* Nothing is running the binary at all. */
    assert.equal(summarize([{ ...clean,
        running: { ...clean.running, process_found: false } }]).all_clean, false);

    /* Built from a tree with uncommitted changes: the commit is not the whole
     * truth about that artifact. */
    assert.equal(summarize([{ ...clean,
        running: { ...clean.running, built_from_dirty_tree: true } }]).all_clean, false);
});
