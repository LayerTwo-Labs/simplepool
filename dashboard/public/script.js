// The page already uses <meta refresh> for whole-page reload. This is the
// only client-side behaviour on the dashboard: the light/dark switch.
//
// The theme itself is applied by an inline script in partial/head.ejs, which
// runs before the stylesheet paints so the refresh timer does not flash the
// other theme. This file only owns the button: reveal it (it ships hidden,
// because without JS it could not do anything), label it, and write the
// choice back to localStorage.
(function () {
    'use strict';

    var STORAGE_KEY = 'sp-theme';
    var btn = document.getElementById('theme-toggle');
    if (!btn) return;

    function current() {
        return document.documentElement.getAttribute('data-theme') === 'dark'
            ? 'dark' : 'light';
    }

    function label() {
        // The button shows where it will take you, not where you are.
        var goingTo = current() === 'dark' ? 'light' : 'dark';
        btn.textContent = goingTo === 'dark' ? '☾' : '☀';
        btn.setAttribute('aria-label', 'Switch to the ' + goingTo + ' theme');
        btn.setAttribute('title', 'Switch to the ' + goingTo + ' theme');
        btn.setAttribute('aria-pressed', current() === 'dark' ? 'true' : 'false');
    }

    btn.addEventListener('click', function () {
        var next = current() === 'dark' ? 'light' : 'dark';
        document.documentElement.setAttribute('data-theme', next);
        try {
            localStorage.setItem(STORAGE_KEY, next);
        } catch (e) {
            // Storage is blocked. The theme still applies to this page; it
            // just will not survive the next refresh. Better than throwing.
        }
        label();
    });

    label();
    btn.hidden = false;
})();
