'use strict';

var klogEl = document.getElementById('klog');
var droppedEl = document.getElementById('dropped');

var LATENCY_BINS = ['0-10', '10-50', '50-100', '100-500', '500-1000', '>1000'];

function renderHist(tableEl, bins, values) {
    var max = Math.max.apply(null, values.length ? values : [1]);
    if (max === 0) max = 1;
    tableEl.innerHTML = '';
    for (var i = 0; i < bins.length; i++) {
        var raw = values[i] || 0;
        var pct = Math.round(100 * raw / max);

        var labelCell = document.createElement('td');
        labelCell.className = 'hist-label';
        labelCell.textContent = bins[i] + ' us';

        var barCell = document.createElement('td');
        var bar = document.createElement('span');
        bar.className = 'hist-bar';
        bar.style.width = pct + '%';
        barCell.appendChild(bar);

        var valCell = document.createElement('td');
        valCell.className = 'hist-val';
        valCell.textContent = raw;

        var tr = document.createElement('tr');
        tr.className = 'hist-row';
        tr.appendChild(labelCell);
        tr.appendChild(barCell);
        tr.appendChild(valCell);
        tableEl.appendChild(tr);
    }
}

pollJSON(['/api/stats', '/api/klog'], function (d, klog) {
    var sched = d.scheduler;
    document.getElementById('wakeup-max').textContent = sched.wakeup_latency_max_us;
    renderHist(document.getElementById('wakeup-hist'),
        LATENCY_BINS, sched.wakeup_latency_hist);

    var co = d.callout;
    document.getElementById('callout-max').textContent = co.max_late_us;
    renderHist(document.getElementById('callout-hist'),
        LATENCY_BINS, co.late_hist);

    document.getElementById('val-ctxsw').textContent = sched.nr_ctxsw;
    document.getElementById('val-ctxsw-avg').textContent = sched.ctxsw_avg_cycles;
    document.getElementById('val-ctxsw-max').textContent = sched.ctxsw_max_cycles;

    document.getElementById('val-denied').textContent = d.security.nr_denied;
    document.getElementById('val-enosys').textContent = d.security.nr_enosys;
    document.getElementById('val-hung').textContent = d.watchdog.nr_hung;
    document.getElementById('val-warn').textContent = d.watchdog.nr_warnings;

    if (d.cpus) {
        var hbBody = document.getElementById('heartbeat-body');
        hbBody.innerHTML = '';
        d.cpus.forEach(function (c) {
            var tr = document.createElement('tr');
            var state = c.heartbeat_stale ? 'STALE'
                      : c.heartbeat_idle ? 'idle' : 'ok';
            [c.cpu, c.heartbeat_age_us || 0, state].forEach(function (v) {
                var td = document.createElement('td');
                td.textContent = v;
                tr.appendChild(td);
            });
            if (c.heartbeat_stale)
                tr.style.color = 'var(--red, red)';
            hbBody.appendChild(tr);
        });
    }

    if (d.domains) {
        var names = ['web', 'sys'];
        renderRows(document.getElementById('domain-body'), d.domains, [
            function (dm) { return names[dm.id] || dm.id; },
            'state', 'nr_members', 'quantum_ticks', 'consumed_ticks'
        ]);
    }

    /* Preserve user scroll position: only auto-scroll when the user was
     * already near the bottom before the update.
     */
    var pinned = klogEl.scrollHeight - klogEl.scrollTop - klogEl.clientHeight < 8;
    klogEl.textContent = klog.log;
    if (pinned)
        klogEl.scrollTop = klogEl.scrollHeight;
    droppedEl.textContent = klog.dropped > 0 ? klog.dropped + ' bytes dropped' : '';
});
