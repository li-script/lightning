document.addEventListener('DOMContentLoaded', () => {
  const body = document.body;
  const menuButton = document.getElementById('menu-button');
  const backdrop = document.querySelector('.sidebar-backdrop');
  const searchInput = document.getElementById('search-input');

  const setMenuOpen = (open) => {
    body.classList.toggle('menu-open', open);
    if (menuButton) menuButton.setAttribute('aria-expanded', String(open));
    if (open && searchInput) searchInput.focus();
  };

  if (menuButton) {
    menuButton.addEventListener('click', () => setMenuOpen(!body.classList.contains('menu-open')));
  }
  if (backdrop) backdrop.addEventListener('click', () => setMenuOpen(false));
  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape') setMenuOpen(false);
  });
  document.querySelectorAll('.nav-link').forEach((link) => {
    link.addEventListener('click', () => setMenuOpen(false));
  });

  if (searchInput) {
    searchInput.addEventListener('input', (event) => {
      const query = event.target.value.toLowerCase().trim();
      let visibleCount = 0;
      document.querySelectorAll('.nav-group').forEach((group) => {
        let groupCount = 0;
        group.querySelectorAll('.nav-link').forEach((link) => {
          const visible = link.textContent.toLowerCase().includes(query);
          link.hidden = !visible;
          if (visible) groupCount += 1;
        });
        group.hidden = groupCount === 0;
        visibleCount += groupCount;
      });
      const emptyState = document.querySelector('.sidebar-empty');
      if (emptyState) emptyState.style.display = visibleCount === 0 ? 'block' : 'none';
    });
  }

  document.querySelectorAll('.markdown-body h1[id], .markdown-body h2[id], .markdown-body h3[id], .markdown-body h4[id]').forEach((heading) => {
    const anchor = document.createElement('a');
    anchor.className = 'heading-anchor';
    anchor.href = `#${heading.id}`;
    anchor.setAttribute('aria-label', `Link to ${heading.textContent}`);
    anchor.textContent = '#';
    heading.prepend(anchor);
  });

  document.querySelectorAll('.btn-code-copy').forEach((button) => {
    button.addEventListener('click', async () => {
      const code = button.closest('.code-block-wrapper').querySelector('code').innerText;
      try {
        await navigator.clipboard.writeText(code);
      } catch (_) {
        const textArea = document.createElement('textarea');
        textArea.value = code;
        textArea.style.position = 'fixed';
        textArea.style.opacity = '0';
        document.body.appendChild(textArea);
        textArea.select();
        document.execCommand('copy');
        textArea.remove();
      }
      button.textContent = 'Copied';
      button.dataset.state = 'copied';
      window.setTimeout(() => {
        button.textContent = 'Copy';
        delete button.dataset.state;
      }, 1500);
    });
  });
});