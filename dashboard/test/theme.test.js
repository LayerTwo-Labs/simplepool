/* The light/dark switch.
 *
 * The theme is applied in three places that have to stay in step, and each
 * one has already failed once during the change that introduced them:
 *
 *   - partial/head.ejs re-applies the saved theme BEFORE the stylesheet
 *     paints. Every page here reloads itself on a <meta refresh> timer, so a
 *     theme applied from the deferred script flashes the other one every 15
 *     seconds.
 *   - script.js loads on every page. The button lives in the shared nav, so
 *     a view that does not load the script renders a nav with a permanently
 *     hidden button — which is what eight of them did, because the <script>
 *     tag was pasted at the end of each body instead of living in the head.
 *   - it loads exactly ONCE. Two copies bind two click handlers, the theme
 *     flips twice, and the button appears dead.
 *
 * These are asserted against the view sources rather than a rendered page:
 * the invariant is about which views opt in, and a render test only covers
 * the views someone remembered to write a case for — which is the same
 * failure again.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const VIEWS  = path.resolve(__dirname, '../views');
const PUBLIC = path.resolve(__dirname, '../public');

/* Top-level views are whole documents; partials are fragments. */
const pages = fs.readdirSync(VIEWS)
    .filter(f => f.endsWith('.ejs'))
    .map(f => ({ name: f, src: fs.readFileSync(path.join(VIEWS, f), 'utf8') }))
    .filter(p => p.src.includes('<html'));

test('there are pages to check, so a rename cannot silently empty this file', () => {
    assert.ok(pages.length >= 10, `only found ${pages.length} page views`);
});

test('every page takes its <head> from the partial', () => {
    for (const p of pages) {
        assert.match(p.src, /include\('partial\/head'/,
                     `${p.name} hand-rolls its <head> and will ignore the saved theme`);
    }
});

test('the head re-applies the saved theme before the stylesheet', () => {
    const head = fs.readFileSync(path.join(VIEWS, 'partial/head.ejs'), 'utf8');
    const script = head.indexOf("localStorage.getItem('sp-theme')");
    const sheet  = head.indexOf('href="/static/style.css"');
    assert.ok(script > -1, 'the head does not read the saved theme');
    assert.ok(sheet  > -1, 'the head does not link the stylesheet');
    assert.ok(script < sheet,
              'the theme must be set before the stylesheet paints, or the ' +
              'meta-refresh flashes the other theme');
    assert.match(head, /try\s*\{/,
                 'localStorage throws outright with cookies blocked — losing ' +
                 'the theme must not take the page down with it');
});

test('the toggle script loads on every page, exactly once', () => {
    const head = fs.readFileSync(path.join(VIEWS, 'partial/head.ejs'), 'utf8');
    assert.match(head, /<script src="\/static\/script\.js" defer><\/script>/);

    for (const p of pages) {
        const own = p.src.match(/src="\/static\/script\.js"/g) || [];
        assert.equal(own.length, 0,
            `${p.name} loads script.js itself as well as via the head — two ` +
            'click handlers means the toggle flips twice and looks dead');
    }
});

test('the button ships hidden, because without JS it could not do anything', () => {
    const toggle = fs.readFileSync(path.join(VIEWS, 'partial/theme-toggle.ejs'), 'utf8');
    assert.match(toggle, /\bhidden\b/);
    assert.match(toggle, /id="theme-toggle"/);
    const js = fs.readFileSync(path.join(PUBLIC, 'script.js'), 'utf8');
    assert.match(js, /btn\.hidden = false/, 'nothing ever reveals the button');
});

test('both navs carry the toggle', () => {
    for (const nav of ['partial/nav-public.ejs', 'partial/nav-admin.ejs']) {
        const src = fs.readFileSync(path.join(VIEWS, nav), 'utf8');
        assert.match(src, /include\('theme-toggle'\)/, `${nav} has no toggle`);
    }
});

test('no view hardcodes a colour the theme cannot reach', () => {
    /* Every colour is a --token, so a hex in a view is a value that stays
     * dark-theme-coloured on a light page. .ok / .bad / .warn-text / .callout
     * exist for exactly the cases that used to do this. */
    const offenders = [];
    for (const f of fs.readdirSync(VIEWS, { recursive: true })) {
        if (!String(f).endsWith('.ejs')) continue;
        const src = fs.readFileSync(path.join(VIEWS, String(f)), 'utf8');
        for (const m of src.matchAll(/style="[^"]*#[0-9a-fA-F]{3,6}\b[^"]*"/g)) {
            offenders.push(`${f}: ${m[0]}`);
        }
    }
    assert.deepEqual(offenders, []);
});

test('the stylesheet defines both palettes over the same token names', () => {
    const css = fs.readFileSync(path.join(PUBLIC, 'style.css'), 'utf8');
    const block = (sel) => {
        const i = css.indexOf(sel + ' {');
        assert.ok(i > -1, `no ${sel} block`);
        return css.slice(i, css.indexOf('}', i));
    };
    const names = (b) => new Set([...b.matchAll(/(--[a-z0-9-]+):/g)].map(m => m[1]));
    const light = names(block(':root'));
    const dark  = names(block(':root[data-theme="dark"]'));
    assert.ok(light.size > 15, 'suspiciously few tokens in the light palette');
    for (const n of light) {
        if (n === '--shadow-none') continue;   /* a constant, not a colour */
        assert.ok(dark.has(n), `${n} has no dark value — it will leak across themes`);
    }
});
