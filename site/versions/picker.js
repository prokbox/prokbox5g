/* ProkBox5g docs: the version picker (scripts/docs_versions.py copies this
   file to <site>/_versions/ and links it from every page of every version).

   It reads <site>/versions.json, which the deploy writes, so a page built
   for an old release always lists every release, including newer ones,
   without being rebuilt.  On a switch it keeps the reader on the same page
   when the other version has it, else it opens that version's home page.
   "What's coming" and the changelog live once, at the site root (newest
   release): every version links there.  #versions opens the list. */
(function () {
  'use strict';
  var me = document.currentScript;
  var BASE = (me && me.getAttribute('data-base')) || '/';

  function el(tag, attrs, text) {
    var e = document.createElement(tag);
    for (var k in attrs) { if (Object.prototype.hasOwnProperty.call(attrs, k)) e.setAttribute(k, attrs[k]); }
    if (text) e.textContent = text;
    return e;
  }

  function where(v) {
    var path = decodeURIComponent(location.pathname);
    if (path.indexOf(BASE) !== 0) return null;
    var rest = path.slice(BASE.length).replace(/index\.html$/, '');
    var seg = rest.split('/')[0];
    var ids = v.versions.map(function (x) { return x.id; });
    if (ids.indexOf(seg) >= 0 || (v.dev && seg === 'dev')) {
      return { cur: seg, rel: rest.slice(seg.length + 1) };
    }
    return { cur: v.latest, root: true, rel: rest };
  }

  function href(v, target, rel, hash) {
    var living = v.living || [];
    if (living.indexOf(rel) >= 0) return BASE + rel + hash;   // one copy, at the root
    var prefix, pages;
    if (target === v.latest) { prefix = BASE; pages = v.root.pages; }
    else if (target === 'dev') { prefix = BASE + 'dev/'; pages = v.dev.pages; }
    else {
      prefix = BASE + target + '/';
      pages = v.versions.filter(function (x) { return x.id === target; })[0].pages;
    }
    return pages.indexOf(rel) >= 0 ? prefix + rel + hash : prefix;
  }

  function build(v) {
    var w = where(v);
    if (!w) return;
    var hash = location.hash === '#versions' ? '' : location.hash;
    var curLabel = w.cur === 'dev' ? 'Unreleased' : w.cur;

    var box = el('details', { 'class': 'pbv-picker' });
    var sum = el('summary', { 'aria-label': 'Version ' + curLabel + ': choose another version' });
    sum.appendChild(el('span', { 'class': 'pbv-k' }, 'Version'));
    sum.appendChild(el('span', { 'class': 'pbv-v' }, curLabel));
    box.appendChild(sum);
    var menu = el('div', { 'class': 'pbv-menu' });
    var ul = el('ul', { 'aria-label': 'Versions of this guide' });
    function item(id, label, note) {
      var li = el('li');
      var a = el('a', { href: href(v, id, w.rel, hash) });
      a.appendChild(el('span', { 'class': 'pbv-name' }, label));
      if (note) a.appendChild(el('span', { 'class': 'pbv-note' }, note));
      if (id === w.cur) a.setAttribute('aria-current', 'page');
      li.appendChild(a);
      ul.appendChild(li);
    }
    if (v.dev) item('dev', 'Unreleased', 'in development');
    v.versions.forEach(function (x) { item(x.id, x.id, x.id === v.latest ? 'newest' : ''); });
    menu.appendChild(ul);
    var more = el('ul', { 'class': 'pbv-living', 'aria-label': 'Always up to date' });
    (v.living_links || []).forEach(function (l) {
      var li = el('li');
      li.appendChild(el('a', { href: BASE + l.path }, l.title));
      more.appendChild(li);
    });
    menu.appendChild(more);
    box.appendChild(menu);

    var bar = document.querySelector('.site-header .bar');
    if (bar) {
      var before = bar.querySelector('.search-open');
      bar.insertBefore(box, before);
    } else {
      box.classList.add('pbv-floating');
      document.body.appendChild(box);
    }
    var fb = document.querySelector('.pbv-fallback');
    if (fb) fb.hidden = true;
    document.addEventListener('click', function (e) { if (box.open && !box.contains(e.target)) box.open = false; });
    document.addEventListener('keydown', function (e) { if (e.key === 'Escape' && box.open) { box.open = false; sum.focus(); } });
    if (location.hash === '#versions') box.open = true;

    // an older release says so, and offers the same page in the newest one
    if (!w.root && w.cur !== 'dev' && w.cur !== v.latest) {
      var b = el('div', { 'class': 'pbv-banner pbv-old', role: 'note' });
      b.appendChild(el('span', {}, 'You are reading the guide for ProkBox5g ' + w.cur + '. The newest release is ' + v.latest + '. '));
      b.appendChild(el('a', { href: href(v, v.latest, w.rel, hash) }, 'Read this page for ' + v.latest));
      var hdr = document.querySelector('.site-header');
      if (hdr && hdr.parentNode) hdr.parentNode.insertBefore(b, hdr.nextSibling);
      else document.body.insertBefore(b, document.body.firstChild);
    }
  }

  fetch(BASE + 'versions.json', { cache: 'no-cache' })
    .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(build)
    .catch(function (e) { if (window.console) console.warn('version picker: ' + e); });
})();
