// Message bridge to the native side (WebView2 postMessage). Pages use hb.call(); the browser UI uses hb.post().
(function () {
  if (window.top !== window.self) { document.documentElement.remove(); throw new Error("framing is not allowed"); }
  const wv = window.chrome && window.chrome.webview;
  const pending = new Map();
  const handlers = {};
  let counter = 0;

  const hb = {
    available: !!wv,
    post(message) { if (wv) wv.postMessage(message); },
    call(cmd, args) {
      return new Promise((resolve, reject) => {
        if (!wv) { reject(new Error('not running inside HB Browser')); return; }
        const id = ++counter;
        pending.set(id, { resolve, reject });
        wv.postMessage({ id, cmd, args: args || {} });
      });
    },
    on(type, fn) { (handlers[type] = handlers[type] || []).push(fn); },
  };

  if (wv) {
    wv.addEventListener('message', (e) => {
      const m = e.data;
      if (!m || typeof m !== 'object') return;
      if (m.type === 'reply') {
        const p = pending.get(m.id);
        if (!p) return;
        pending.delete(m.id);
        if (m.ok) p.resolve(m.result); else p.reject(new Error(String(m.result)));
        return;
      }
      (handlers[m.type] || []).forEach((fn) => fn(m));
    });
  }
  window.hb = hb;

  // Timing of internal pages goes to dev.log (native side decides whether it is written).
  if (wv && location.pathname.indexOf('/pages/') >= 0) {
    window.addEventListener('load', () => setTimeout(() => {
      try {
        const n = performance.getEntriesByType('navigation')[0] || {};
        const res = performance.getEntriesByType('resource');
        let slow = { name: '', ms: 0 };
        res.forEach((r) => { if (r.duration > slow.ms) slow = { name: r.name.split('/').pop(), ms: r.duration }; });
        hb.call('dev.perf', { page: location.pathname.split('/').pop(), dcl: Math.round(n.domContentLoadedEventEnd || 0),
          load: Math.round(n.loadEventEnd || performance.now()), resources: res.length, slowName: slow.name, slowMs: Math.round(slow.ms) }).catch(() => {});
      } catch (e) { /* diagnostics only */ }
    }, 0));
  }
})();
