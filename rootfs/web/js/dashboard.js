'use strict';

pollJSON('/api/stats', function (d) {
    document.getElementById('val-uptime').textContent = formatDuration(d.uptime_ms);
    document.getElementById('uptime').textContent = formatDuration(d.uptime_ms);

    var mem = d.memory;
    var used = mem.total_pages - mem.free_pages;
    var pct = mem.total_pages ? Math.round(100 * used / mem.total_pages) : 0;
    document.getElementById('val-mem').textContent = pct + '%';
    document.getElementById('val-mem-detail').textContent =
        used + ' / ' + mem.total_pages + ' pages';
    var gf = document.getElementById('mem-gauge');
    gf.style.width = pct + '%';
    gf.className = 'gauge-fill' + (pct > 90 ? ' danger' : pct > 70 ? ' warn' : '');

    var tcp = d.tcp;
    document.getElementById('val-tcp').textContent = tcp.connections;
    document.getElementById('val-tcp-detail').textContent = 'retx: ' + tcp.retransmits;
    document.getElementById('val-4xx').textContent = d.http['4xx'];
    document.getElementById('val-5xx').textContent = d.http['5xx'];
    document.getElementById('val-tx').textContent = formatBytes(tcp.bytes_tx);
    document.getElementById('val-rx').textContent = formatBytes(tcp.bytes_rx);

    renderRows(document.getElementById('task-body'), d.tasks, [
        'id', 'state', 'prio',
        function (t) { return t.cpu_us + ' us'; }
    ]);
});
