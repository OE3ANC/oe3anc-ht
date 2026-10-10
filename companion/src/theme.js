// SPDX-License-Identifier: GPL-3.0-or-later
(() => {
    const key = 'oe3anc-ht-companion-theme';
    const system = window.matchMedia('(prefers-color-scheme: dark)');
    let preference = 'system';
    try {
        const saved = window.localStorage.getItem(key);
        if (['system', 'light', 'dark'].includes(saved)) {
            preference = saved;
        }
    } catch {
        // Storage may be blocked; switching still works for this page.
    }

    function apply() {
        document.documentElement.dataset.theme = preference === 'system'
            ? (system.matches ? 'dark' : 'light')
            : preference;
    }

    // Apply before the stylesheet loads to avoid flashing the wrong palette.
    apply();
    system.addEventListener('change', apply);
    document.addEventListener('DOMContentLoaded', () => {
        const select = document.querySelector('#companion-theme');
        select.value = preference;
        select.addEventListener('change', () => {
            preference = select.value;
            apply();
            try {
                window.localStorage.setItem(key, preference);
            } catch {
                // Keep the selected palette even when persistence is unavailable.
            }
        });
    });
})();
