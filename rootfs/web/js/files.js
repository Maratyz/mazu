'use strict';

var currentPath = '/';
var breadcrumb = document.getElementById('breadcrumb');
var fileBody = document.getElementById('file-body');
var fileView = document.getElementById('file-view');

function renderBreadcrumb(p) {
    breadcrumb.innerHTML = '';
    var root = document.createElement('a');
    root.textContent = '/';
    root.onclick = function () { navigate('/'); };
    breadcrumb.appendChild(root);

    var acc = '';
    p.split('/').filter(Boolean).forEach(function (seg) {
        var sep = document.createElement('span');
        sep.textContent = ' / ';
        breadcrumb.appendChild(sep);
        acc += '/' + seg;
        var link = document.createElement('a');
        link.textContent = seg;
        (function (target) { link.onclick = function () { navigate(target); }; })(acc);
        breadcrumb.appendChild(link);
    });
}

function entryRow(parent, e) {
    var full = (parent === '/' ? '/' : parent + '/') + e.name;
    var isDir = e.type === 'dir';

    var tr = document.createElement('tr');
    var nameCell = document.createElement('td');
    var a = document.createElement('a');
    a.textContent = isDir ? e.name + '/' : e.name;
    a.style.cursor = 'pointer';
    a.style.color = isDir ? 'var(--blue)' : 'var(--fg-bright)';
    a.onclick = isDir
        ? function () { navigate(full); }
        : function () { viewFile(full); };
    nameCell.appendChild(a);

    var typeCell = document.createElement('td');
    typeCell.textContent = e.type;

    tr.appendChild(nameCell);
    tr.appendChild(typeCell);
    return tr;
}

function navigate(path) {
    currentPath = path;
    fileView.style.display = 'none';
    renderBreadcrumb(path);
    fetchJSON('/api/fs?path=' + encodeURIComponent(path)).then(function (d) {
        fileBody.innerHTML = '';
        d.entries.forEach(function (e) {
            fileBody.appendChild(entryRow(path, e));
        });
    }).catch(function () {
        fileBody.innerHTML = '<tr><td colspan="2">Error loading directory</td></tr>';
    });
}

function viewFile(path) {
    fetch('/api/fs/read?path=' + encodeURIComponent(path), { cache: 'no-store' })
        .then(function (r) {
            if (!r.ok) throw new Error(r.status);
            return r.text();
        })
        .then(function (text) {
            fileView.style.display = 'block';
            fileView.textContent = text;
        })
        .catch(function () {
            fileView.style.display = 'block';
            fileView.textContent = '(binary or unreadable)';
        });
}

navigate('/');
