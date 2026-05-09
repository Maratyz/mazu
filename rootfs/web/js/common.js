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

/* Poll timer manager: returns {start, stop}. */
function createPoller(fn, intervalMs) {
    var tid = null;
    function run() {
        fn();
        tid = setTimeout(run, intervalMs);
    }
    return {
        start: function () { if (!tid) run(); },
        stop: function () { if (tid) { clearTimeout(tid); tid = null; } }
    };
}

/* Sidebar: highlight current page link. */
(function () {
    var path = location.pathname.replace(/^\//, '');
    var links = document.querySelectorAll('.sidebar a');
    for (var i = 0; i < links.length; i++) {
        var href = links[i].getAttribute('href').replace(/^\//, '');
        if (href === path) links[i].classList.add('active');
    }
})();
