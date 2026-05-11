'use strict';

/* Mazu shell, kept self-contained for reliability. */

var SID_KEY = 'mazu_shell_sid';
var TOK_KEY = 'mazu_shell_tok';
var POLL_MS = 250;
var SESSION_RETRY_MS = 2000;

var COMMANDS = [
    'cat', 'echo', 'help', 'ls', 'mem', 'mkdir', 'mount',
    'netstat', 'nslookup', 'ping', 'ps', 'rm',
    'tcpstats', 'uptime', 'write'
];

function getSid() {
    try {
        var stored = sessionStorage.getItem(SID_KEY);
        if (stored) return stored;
    } catch (e) {}
    var sid = String((Math.random() * 0x7fffffff | 0) + 1);
    try { sessionStorage.setItem(SID_KEY, sid); } catch (e) {}
    return sid;
}

var SID = getSid();
var TOK = '';
var readOffset = 0;

var output   = document.getElementById('output');
var cmdInput = document.getElementById('cmd-input');
var sendBtn  = document.getElementById('send-btn');
var statusEl = document.getElementById('status-text');

document.getElementById('session-id').textContent = 'sid=' + SID;

function appendOutput(text, cls) {
    if (!text) return;
    var span = document.createElement('span');
    span.className = 'line-' + cls;
    span.textContent = text;
    output.appendChild(span);
    output.scrollTop = output.scrollHeight;
}

function setShellStatus(state) {
    setStatus(state, 'status-dot');
    statusEl.textContent = state;
}

var cmdHistory = [];
var histIdx = -1;
var pollTimer = null;
var pollInFlight = false;
var connectInFlight = false;

function loadStoredToken() {
    try {
        var tok = sessionStorage.getItem(TOK_KEY);
        if (tok) TOK = tok;
    } catch (e) {}
}

function clearSessionToken() {
    TOK = '';
    try { sessionStorage.removeItem(TOK_KEY); } catch (e) {}
}

function getNextOffset(xhr, fallback) {
    var raw = xhr.getResponseHeader('X-Shell-Next-Offset');
    if (!raw) return fallback;
    var next = parseInt(raw, 10);
    return isFinite(next) && next >= 0 ? next : fallback;
}

function schedulePoll(delayMs) {
    if (!TOK) return;
    if (pollTimer) clearTimeout(pollTimer);
    pollTimer = setTimeout(pollOutput, delayMs);
}

function makeRequest(method, url, body, onDone) {
    var xhr = new XMLHttpRequest();
    xhr.open(method, url, true);
    xhr.setRequestHeader('Cache-Control', 'no-store');
    xhr.setRequestHeader('Pragma', 'no-cache');
    if (body !== null) xhr.setRequestHeader('Content-Type', 'text/plain');
    xhr.onload = function () { onDone(null, xhr); };
    xhr.onerror = function () { onDone(new Error('network failure')); };
    xhr.timeout = 10000;
    xhr.ontimeout = function () { onDone(new Error('timeout')); };
    xhr.send(body);
}

function createSession() {
    return new Promise(function (resolve) {
        makeRequest('GET', '/api/shell/in?sid=' + SID, null, function (err, xhr) {
            if (err) {
                appendOutput('session error: ' + err.message + '\n', 'error');
                resolve();
                return;
            }
            if (xhr.status !== 200) {
                appendOutput('session error: HTTP ' + xhr.status + '\n', 'error');
                resolve();
                return;
            }
            var m = xhr.responseText.match(/^tok=(\d+)/);
            if (!m) {
                appendOutput('session error: unexpected response\n', 'error');
                resolve();
                return;
            }
            TOK = m[1];
            try { sessionStorage.setItem(TOK_KEY, TOK); } catch (e) {}
            resolve();
        });
    });
}

function ensureSession() {
    if (TOK) return Promise.resolve(true);
    if (connectInFlight) {
        return new Promise(function (resolve) {
            var retry = function () {
                if (!connectInFlight) {
                    resolve(!!TOK);
                    return;
                }
                setTimeout(retry, 50);
            };
            retry();
        });
    }
    connectInFlight = true;
    return createSession().then(function () {
        connectInFlight = false;
        return !!TOK;
    }, function () {
        connectInFlight = false;
        return false;
    });
}

var MAX_SEND_RETRY = 2;

function sendCommand(cmd, retry) {
    cmd = cmd.trim();
    if (!cmd) return;

    if (retry === undefined) {
        cmdHistory.unshift(cmd);
        if (cmdHistory.length > 50) cmdHistory.pop();
        histIdx = -1;
        appendOutput('$ ' + cmd + '\n', 'input');
        retry = 0;
    }

    ensureSession().then(function (ok) {
        if (!ok) {
            appendOutput('error: cannot create shell session\n', 'error');
            setShellStatus('offline');
            return;
        }
        if (pollTimer) { clearTimeout(pollTimer); pollTimer = null; }
        var beforeOffset = readOffset;
        makeRequest('POST',
                    '/api/shell/in?sid=' + SID + '&tok=' + TOK,
                    cmd + '\n',
                    function (err, xhr) {
            if (err) {
                appendOutput('command error: ' + err.message + '\n', 'error');
                setShellStatus('offline');
                schedulePoll(POLL_MS * 4);
                return;
            }
            if (xhr.status === 403 || xhr.status === 404) {
                if (retry >= MAX_SEND_RETRY) {
                    clearSessionToken();
                    appendOutput('error: session keeps rejecting; giving up\n',
                                 'error');
                    setShellStatus('offline');
                    return;
                }
                clearSessionToken();
                setShellStatus('connecting');
                appendOutput('session expired, reconnecting\n', 'system');
                ensureSession().then(function (retryOk) {
                    if (!retryOk) {
                        appendOutput('error: session restore failed\n', 'error');
                        setShellStatus('offline');
                        return;
                    }
                    sendCommand(cmd, retry + 1);
                });
                return;
            }
            if (xhr.status !== 200) {
                appendOutput('error: HTTP ' + xhr.status + '\n', 'error');
                if (xhr.responseText)
                    appendOutput(xhr.responseText + '\n', 'error');
                setShellStatus('offline');
                return;
            }
            var immediate = xhr.responseText || '';
            var nextOffset = getNextOffset(xhr, beforeOffset + immediate.length);
            if (immediate) {
                readOffset = nextOffset;
                appendOutput(immediate, 'output');
            }
            setShellStatus('online');
            if (!immediate) {
                readOffset = nextOffset;
                appendOutput('[no command output]\n', 'system');
            }
            schedulePoll(POLL_MS);
        });
    });
}

function pollOutput() {
    if (pollInFlight || !TOK) return;
    pollInFlight = true;
    var snapOffset = readOffset;
    makeRequest('GET',
                '/api/shell/out?sid=' + SID + '&from=' + snapOffset + '&tok=' + TOK,
                null,
                function (err, xhr) {
        pollInFlight = false;
        if (err) {
            setShellStatus('offline');
            schedulePoll(POLL_MS * 4);
            return;
        }
        if (xhr.status === 403 || xhr.status === 404) {
            clearSessionToken();
            setShellStatus('connecting');
            connectSession();
            return;
        }
        if (xhr.status !== 200) {
            setShellStatus('offline');
            schedulePoll(POLL_MS * 4);
            return;
        }
        setShellStatus('online');
        if (xhr.responseText && readOffset === snapOffset) {
            readOffset = getNextOffset(xhr, snapOffset + xhr.responseText.length);
            appendOutput(xhr.responseText, 'output');
        } else if (readOffset === snapOffset) {
            readOffset = getNextOffset(xhr, snapOffset);
        }
        schedulePoll(POLL_MS);
    });
}

function connectSession() {
    ensureSession().then(function (ok) {
        if (ok) {
            setShellStatus('online');
            schedulePoll(0);
        } else {
            setShellStatus('offline');
            setTimeout(connectSession, SESSION_RETRY_MS);
        }
    });
}

function tabComplete(value) {
    var parts = value.split(/\s+/);
    var prefix = parts[parts.length - 1];
    if (!prefix) return null;
    var lc = prefix.toLowerCase();
    var matches = COMMANDS.filter(function (c) { return c.indexOf(lc) === 0; });
    if (matches.length === 1) {
        parts[parts.length - 1] = matches[0];
        return parts.join(' ') + ' ';
    }
    return null;
}

cmdInput.addEventListener('keydown', function (e) {
    if (e.key === 'Enter') {
        var cmd = cmdInput.value; cmdInput.value = '';
        sendCommand(cmd);
    } else if (e.key === 'Tab') {
        e.preventDefault();
        var result = tabComplete(cmdInput.value);
        if (result) cmdInput.value = result;
    } else if (e.key === 'ArrowUp') {
        e.preventDefault();
        if (histIdx + 1 < cmdHistory.length) { histIdx++; cmdInput.value = cmdHistory[histIdx]; }
    } else if (e.key === 'ArrowDown') {
        e.preventDefault();
        if (histIdx > 0) { histIdx--; cmdInput.value = cmdHistory[histIdx]; }
        else { histIdx = -1; cmdInput.value = ''; }
    }
});

sendBtn.addEventListener('click', function () {
    var cmd = cmdInput.value; cmdInput.value = '';
    sendCommand(cmd);
});

loadStoredToken();
appendOutput('Mazu shell — type "help" for commands, Tab to complete\n', 'system');
setShellStatus('connecting');
cmdInput.focus();
connectSession();
