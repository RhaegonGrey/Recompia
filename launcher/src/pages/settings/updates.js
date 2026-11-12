// About > updates: check for a new version, install it, pick a specific build, open a release page.
import { api } from '../../api.js';
import { store } from '../../store.js';
import { chooseBuild } from '../builds.js';

export function createUpdateActions(h) {
  const { st } = h;

  return {
    actions: {
      async 'check-updates'() {
        st.checking = true;
        h.draw();
        await store.refreshNews(true);
        st.checking = false;
        st.checked = true;
        h.draw();
      },
      update: () => store.installUpdate(),
      'open-url': (url) => api.openLink({ url }),
      async 'builds-open'() {
        await h.flush();
        await chooseBuild(st.src.config.channel === 'testing' ? 'testing' : 'stable', false);
      },
    },
  };
}
