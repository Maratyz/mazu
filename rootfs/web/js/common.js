'use strict';

/* Mazu shared utilities */

function fetchJSON(url) {
    return fetch(url, { cache: 'no-store' }).then(function (r) {
        if (!r.ok) throw new Error(r.status);
        return r.json();
    });
}

function formatBytes(b) {
    if (b < 1024) return b + ' B';
    if (b < 1024 * 1024) return (b / 1024).toFixed(1) + ' KB';
    return (b / (1024 * 1024)).toFixed(1) + ' MB';
}

function formatDuration(ms) {
    var s = Math.floor(ms / 1000);
    var m = Math.floor(s / 60); s %= 60;
    var h = Math.floor(m / 60); m %= 60;
    var d = Math.floor(h / 24); h %= 24;
    if (d > 0) return d + 'd ' + h + 'h ' + m + 'm';
    if (h > 0) return h + 'h ' + m + 'm ' + s + 's';
    if (m > 0) return m + 'm ' + s + 's';
    return s + 's';
}

/* Poll timer manager: returns {start, stop}.
 *
 * Self-throttling: a new poll never starts while the previous one is still
 * in flight; the timer reschedules and waits. The "running" flag separates
 * "we have a live poller" from "a timer is currently pending" so stop()
 * works even when called while fn is mid-flight (the .finally schedule()
 * would otherwise resurrect the loop).
 */
function createPoller(fn, intervalMs) {
    var tid = null;
    var inFlight = false;
    var running = false;

    function schedule() {
        if (!running) return;
        if (tid) clearTimeout(tid);
        tid = setTimeout(run, intervalMs);
    }

    function run() {
        if (!running) return;
        if (inFlight) {
            schedule();
            return;
        }

        inFlight = true;
        Promise.resolve()
            .then(fn)
            .catch(function () {
                /* Page-level handlers already update UI state on failure. */
            })
            .finally(function () {
                inFlight = false;
                schedule();
            });
    }
    return {
        start: function () {
            if (running) return;
            running = true;
            run();
        },
        stop: function () {
            running = false;
            if (tid) { clearTimeout(tid); tid = null; }
        }
    };
}

/* Poll one or more JSON endpoints and dispatch the parsed bodies to a
 * render callback. setStatus() is flipped to online/offline automatically.
 * Pass a single URL string for a single fetch, or an array for parallel
 * Promise.all-style fetches (the render callback receives one argument per
 * URL, in order). Returns the underlying poller so callers can stop it.
 */
function pollJSON(urls, render, intervalMs) {
    var fetchAll = typeof urls === 'string'
        ? function () { return fetchJSON(urls).then(function (d) { return [d]; }); }
        : function () { return Promise.all(urls.map(fetchJSON)); };
    var p = createPoller(function () {
        return fetchAll().then(
            function (results) {
                /* Flip the dot before invoking render so a render exception
                 * (DOM lookup failure, missing API field) does not get
                 * conflated with a transport failure and mis-flip offline.
                 */
                setStatus('online');
                try { render.apply(null, results); } catch (e) { /* swallow */ }
            },
            function () { setStatus('offline'); }
        );
    }, intervalMs || 2000);
    p.start();
    return p;
}

/* Status indicator that flips between connecting/online/offline. The DOM id
 * defaults to "dot" but can be overridden.
 */
function setStatus(state, id) {
    var el = document.getElementById(id || 'dot');
    if (!el) return;
    el.className = 'status-dot' + (state === 'online' ? '' : ' ' + state);
}

/* Sidebar markup. Generated once and injected at the page top so every
 * page shares the same nav and only renders one canonical list of links.
 */
var SIDEBAR_LINKS = [
    { href: '/dashboard.html', label: 'Dashboard' },
    { href: '/files.html',     label: 'Files' },
    { href: '/network.html',   label: 'Network' },
    { href: '/telemetry.html', label: 'Telemetry' },
    { href: '/shell.html',     label: 'Shell' }
];

function mountSidebar() {
    var nav = document.createElement('nav');
    nav.className = 'sidebar';
    var title = document.createElement('div');
    title.className = 'sidebar-title';
    title.textContent = 'Mazu OS';
    nav.appendChild(title);

    var path = location.pathname.replace(/^\//, '');
    SIDEBAR_LINKS.forEach(function (l) {
        var a = document.createElement('a');
        a.href = l.href;
        a.textContent = l.label;
        if (l.href.replace(/^\//, '') === path) a.classList.add('active');
        nav.appendChild(a);
    });

    document.body.insertBefore(nav, document.body.firstChild);
}

/* Render a <table> body from an array of row objects. Each column entry
 * describes a static key, a callable returning the cell value, or a
 * callable returning a DOM Node (appended as-is) / a {text, cls} record.
 */
function renderRows(tbody, rows, columns) {
    tbody.innerHTML = '';
    rows.forEach(function (row) {
        var tr = document.createElement('tr');
        columns.forEach(function (col) {
            var td = document.createElement('td');
            var v = typeof col === 'function' ? col(row) : row[col];
            /* Node check must precede the {text, cls} duck-type because
             * HTMLAnchorElement and others have a native .text property.
             */
            if (v instanceof Node) {
                td.appendChild(v);
            } else if (v && typeof v === 'object' && 'text' in v) {
                td.textContent = v.text;
                if (v.cls) td.className = v.cls;
            } else {
                td.textContent = v == null ? '' : v;
            }
            tr.appendChild(td);
        });
        tbody.appendChild(tr);
    });
}

/* Sidebar is mounted synchronously: this script is loaded at end of <body>,
 * so the DOM is already complete. No event listener needed.
 */
mountSidebar();
