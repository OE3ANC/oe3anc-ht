// SPDX-License-Identifier: GPL-3.0-or-later
export function setupToolTabs(root, changed) {
    const tabs = [...root.querySelectorAll('.task-nav [role="tab"]')];

    function select(tab, focus = false) {
        for (const item of tabs) {
            const selected = item === tab;
            item.setAttribute('aria-selected', String(selected));
            item.tabIndex = selected ? 0 : -1;
            root.querySelector(`#${item.getAttribute('aria-controls')}`).hidden = !selected;
        }
        if (focus) {
            tab.focus();
        }
        changed();
    }

    function fromHash() {
        const tab = tabs.find(item => `#${item.getAttribute('aria-controls')}` === location.hash);
        select(tab ?? tabs[0]);
    }

    function activate(tab) {
        select(tab, true);
        history.replaceState(null, '', `#${tab.getAttribute('aria-controls')}`);
    }

    for (const [index, tab] of tabs.entries()) {
        tab.addEventListener('click', () => activate(tab));
        tab.addEventListener('keydown', event => {
            let next;
            if (event.key === 'ArrowRight') {
                next = (index + 1) % tabs.length;
            } else if (event.key === 'ArrowLeft') {
                next = (index + tabs.length - 1) % tabs.length;
            } else if (event.key === 'Home') {
                next = 0;
            } else if (event.key === 'End') {
                next = tabs.length - 1;
            } else {
                return;
            }
            event.preventDefault();
            activate(tabs[next]);
        });
    }
    window.addEventListener('hashchange', fromHash);
    fromHash();
}
