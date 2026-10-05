// Injected only by material_probe's bounded optics_interaction sequence.
// Selectors follow AppHeader, ClipboardTypeFilter and SettingsPanel; the
// fixture fixes app.language=zh. Pointer events are synthetic, not OS input.
(async () => {
    const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
    const frames = async () => {
        await new Promise(requestAnimationFrame);
        await new Promise(requestAnimationFrame);
    };
    const wait = async (predicate, description) => {
        const deadline = performance.now() + 8000;
        while (!predicate()) {
            if (performance.now() >= deadline) throw new Error(`DOM readiness timeout: ${description}`);
            await delay(25);
        }
        await frames();
    };
    const need = selector => {
        const element = document.querySelector(selector);
        if (!(element instanceof HTMLElement)) throw new Error(`Missing fixture target: ${selector}`);
        return element;
    };
    const bounds = element => {
        const rect = element.getBoundingClientRect();
        return { x: rect.x, y: rect.y, width: rect.width, height: rect.height };
    };
    const nativeLens = host => host.querySelector(':scope > .glass-lens[data-native-glass="true"]');
    const group = title => {
        const header = [...document.querySelectorAll('.settings-view .group-header')]
            .find(element => element.querySelector('h3')?.textContent?.trim() === title);
        if (!(header instanceof HTMLElement) || !header.parentElement?.matches('.settings-group')) {
            throw new Error(`Missing settings group: ${title}`);
        }
        return { header, body: header.parentElement };
    };
    const scrollTo = async target => {
        const view = need('.settings-view');
        const rect = target.getBoundingClientRect();
        const viewRect = view.getBoundingClientRect();
        view.scrollTo({ top: view.scrollTop + rect.top - viewRect.top - 28, behavior: 'instant' });
        await frames();
        await delay(250);
        const after = target.getBoundingClientRect();
        if (after.bottom <= viewRect.top || after.top >= viewRect.bottom) throw new Error('Scroll target stayed outside settings viewport');
    };
    const openGroup = async title => {
        const selected = group(title);
        for (const name of ['常规设置', '剪贴板设置', '界面设置', '数据管理']) {
            if (name !== title) {
                const other = group(name);
                if (!other.body.classList.contains('collapsed')) other.header.click();
            }
        }
        if (selected.body.classList.contains('collapsed')) selected.header.click();
        await wait(() => selected.body.querySelector('.group-content'), `${title} content`);
        await scrollTo(selected.header);
        return selected.body;
    };
    let target;
    let inputMethod = 'DOM inspection and scroll only';
    try {
        await wait(() => document.documentElement.classList.contains('native-glass-ready'), 'native ready presentation');
        if (step.action === 'dom_ready') {
            if (matchMedia('(prefers-reduced-motion: reduce)').matches) {
                throw new Error('Reduced motion is active; production correctly disables pressure animation');
            }
            await wait(() => document.querySelector('.header-actions button[title="设置"]')
                && nativeLens(document.querySelector('.header-actions'))
                && document.querySelector('.liquid-search > .glass-lens[data-native-glass="true"]')
                && document.querySelector('.type-filter-btn > .glass-lens[data-native-glass="true"]'), 'main glass controls');
            target = need('.header-actions');
        } else if (/^(header|search|chip)_(press|release)$/.test(step.action)) {
            const kind = step.action.split('_')[0];
            target = need(kind === 'header' ? '.header-actions button[title="设置"]'
                : kind === 'search' ? '.liquid-search .search-input' : '.type-filter-btn');
            const host = kind === 'header' ? target.closest('.header-actions')
                : kind === 'search' ? target.closest('.liquid-search') : target;
            await wait(() => nativeLens(host), `${kind} native lens`);
            const pressed = step.action.endsWith('_press');
            const rect = target.getBoundingClientRect();
            target.dispatchEvent(new PointerEvent(pressed ? 'pointerdown' : 'pointerup', {
                bubbles: true, cancelable: true, composed: true, pointerId: 41,
                pointerType: 'mouse', isPrimary: true, button: 0, buttons: pressed ? 1 : 0,
                clientX: rect.left + rect.width / 2, clientY: rect.top + rect.height / 2,
                pressure: pressed ? 0.5 : 0
            }));
            inputMethod = 'synthetic PointerEvent, isTrusted=false; no click or OS input';
        } else if (step.action === 'settings_open') {
            need('.header-actions button[title="设置"]').click();
            inputMethod = 'HTMLElement.click on production settings button';
            await wait(() => document.querySelector('.settings-view .group-header h3'), 'settings page');
            target = need('.settings-view');
        } else if (step.action === 'general_open') {
            target = await openGroup('常规设置');
            await wait(() => target.querySelector('.toggle > .glass-lens'), 'general toggle glass');
            inputMethod = 'HTMLElement.click on settings group headers; DOM scroll';
        } else if (step.action === 'appearance_open') {
            target = await openGroup('界面设置');
            await wait(() => target.querySelector('.theme-choice-btn > .glass-lens'), 'theme choice glass');
            inputMethod = 'HTMLElement.click on settings group headers; DOM scroll';
        } else if (step.action === 'appearance_ranges' || step.action === 'appearance_surface') {
            const appearance = group('界面设置').body;
            target = step.action === 'appearance_ranges'
                ? appearance.querySelector('.settings-glass-range')
                : appearance.querySelector('.glass-transparency-slider')?.closest('.glass-slider-control');
            if (!(target instanceof HTMLElement)) throw new Error('Missing appearance glass range');
            await scrollTo(target.closest('.setting-item') || target);
        } else if (step.action === 'clipboard_open') {
            target = await openGroup('剪贴板设置');
            await wait(() => target.querySelector('.toggle > .glass-lens'), 'clipboard toggle glass');
            inputMethod = 'HTMLElement.click on settings group headers; DOM scroll';
        } else if (step.action === 'clipboard_shortcut') {
            target = group('剪贴板设置').body.querySelector('.key-group');
            if (!(target instanceof HTMLElement)) throw new Error('Missing clipboard shortcut control');
            await scrollTo(target.closest('.setting-item') || target);
        } else if (step.action === 'data_open') {
            target = await openGroup('数据管理');
            await wait(() => target.querySelector('.data-panel > .glass-lens'), 'data panel glass');
            inputMethod = 'HTMLElement.click on settings group headers; DOM scroll';
        } else if (step.action === 'settings_back') {
            need('.header-back').click();
            inputMethod = 'HTMLElement.click on production back button';
            await wait(() => document.querySelector('.liquid-search .search-input')
                && !document.querySelector('.settings-view'), 'main page restored');
            target = need('.header-actions');
        } else {
            throw new Error('Unknown bounded optics action');
        }
        await frames();
        await window.__TAURI_INTERNALS__.invoke('qa_optics_interaction_ack', {
            index: step.index, action: step.action, ok: true, inputMethod,
            target: { className: target.className, bounds: bounds(target) },
            settingsScrollTop: document.querySelector('.settings-view')?.scrollTop ?? null,
            nativeReady: document.documentElement.classList.contains('native-glass-ready'),
            nativeLensCount: document.querySelectorAll('.glass-lens[data-native-glass="true"]').length,
            viewportIntersectingLensCount: [...document.querySelectorAll('.glass-lens')].filter(lens => {
                const rect = lens.getBoundingClientRect();
                return rect.width > 0 && rect.height > 0 && rect.bottom > 0 && rect.top < innerHeight;
            }).length
        });
    } catch (error) {
        await window.__TAURI_INTERNALS__.invoke('qa_optics_interaction_ack', {
            index: step.index, action: step.action, ok: false, inputMethod,
            error: error instanceof Error ? error.message : String(error)
        });
    }
})();
