// Fa que la web de control obri sense internet després d'haver-la obert un cop amb connexió.
// Va al mateix directori de GitHub Pages que l'HTML.
// Estratègia: sempre prova la xarxa primer (per tenir l'última versió) i, si no n'hi ha,
// fa servir la còpia guardada.
const CACHE = 'pantalla-frequencies-v1';

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));

self.addEventListener('fetch', e => {
  if (e.request.method !== 'GET') return;
  e.respondWith(
    fetch(e.request)
      .then(resp => {
        const copy = resp.clone();
        caches.open(CACHE).then(c => c.put(e.request, copy));
        return resp;
      })
      .catch(() => caches.match(e.request, { ignoreSearch: true }))
  );
});
