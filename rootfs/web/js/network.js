'use strict';

var canvas = document.getElementById('graph');
var ctx2d = canvas.getContext('2d');
var txHist = [];
var rxHist = [];
var maxPts = 60;
var prevTx = null;
var prevRx = null;

function stateClass(s) {
    s = s.toLowerCase().replace(/[_-]/g, '');
    if (s === 'established') return 'state-established';
    if (s === 'listen') return 'state-listen';
    if (s === 'timewait') return 'state-time-wait';
    if (s === 'closewait') return 'state-close-wait';
    if (s === 'closed') return 'state-closed';
    return '';
}

function drawGraph() {
    var w = canvas.width = canvas.clientWidth;
    var h = canvas.height = 120;
    var padTop = 8, padBottom = 6;
    ctx2d.clearRect(0, 0, w, h);

    var all = txHist.concat(rxHist);
    var maxVal = Math.max.apply(null, all.length ? all : [1]);
    if (maxVal === 0) maxVal = 1;

    function plot(arr, color) {
        ctx2d.strokeStyle = color;
        ctx2d.lineWidth = 1.5;
        ctx2d.beginPath();
        for (var i = 0; i < arr.length; i++) {
            var x = (i / (maxPts - 1)) * w;
            var y = h - padBottom - (arr[i] / maxVal) * (h - padTop - padBottom);
            if (i === 0) ctx2d.moveTo(x, y);
            else ctx2d.lineTo(x, y);
        }
        ctx2d.stroke();
    }

    plot(txHist, '#3fb950');
    plot(rxHist, '#58a6ff');
}

pollJSON(['/api/stats', '/api/tcp', '/api/arp'], function (stats, tcp, arp) {
    document.getElementById('val-tx').textContent = formatBytes(stats.tcp.bytes_tx);
    document.getElementById('val-rx').textContent = formatBytes(stats.tcp.bytes_rx);
    document.getElementById('val-retx').textContent = stats.tcp.retransmits;

    if (prevTx !== null) {
        txHist.push(stats.tcp.bytes_tx - prevTx);
        rxHist.push(stats.tcp.bytes_rx - prevRx);
        if (txHist.length > maxPts) { txHist.shift(); rxHist.shift(); }
    }
    prevTx = stats.tcp.bytes_tx;
    prevRx = stats.tcp.bytes_rx;
    drawGraph();

    renderRows(document.getElementById('arp-body'), arp.entries, [
        'ip', 'mac',
        function (e) { return formatDuration(e.age_ms); }
    ]);

    renderRows(document.getElementById('tcp-body'), tcp.connections, [
        'host', 'peer',
        function (c) { return { text: c.state, cls: stateClass(c.state) }; }
    ]);
});
