// On-screen gamepad for phones and tablets. On a touch device, pressing Play goes fullscreen (where the
// browser allows it), asks for landscape (a "rotate your phone" card shows while the device is upright)
// and draws a controller over the game. The game sees it as pad 0 (loader.js reads rcTouch.state() in
// pollGamepads); a real gamepad on that slot still wins. ?touch=1 forces it on, ?touch=0 off.
(() => {
  const q = new URLSearchParams(location.search).get('touch');
  const isTouch = q === '1' || (q !== '0' && matchMedia('(pointer: coarse)').matches && navigator.maxTouchPoints > 0);
  const api = window.rcTouch = { enabled: isTouch, active: false, buttons: 0, ax: [0, 0, 0, 0],
    state() { return { buttons: this.buttons, ax: this.ax }; } };
  if (!isTouch) return;

  const stage = document.getElementById('stage');
  const css = document.createElement('style');
  css.textContent = `
  body.touch-play { overflow: hidden; overscroll-behavior: none; }
  body.touch-play main { padding: 0; }
  body.touch-play #stage { position: fixed; inset: 0; width: 100%; height: 100dvh; aspect-ratio: auto; border-radius: 0;
    border: 0; box-shadow: none; z-index: 50; }
  body.touch-play #fs, body.touch-play #fstip, body.touch-play #kbd, body.touch-play #kbd-show { display: none !important; }
  #tpad, #rotate { position: absolute; inset: 0; z-index: 5; display: none; user-select: none; -webkit-user-select: none;
    -webkit-touch-callout: none; touch-action: none; }
  #tpad { pointer-events: none; --u: min(11vh, 11vw); }
  #tpad.on { display: block; }
  #tpad .c { position: absolute; pointer-events: auto; touch-action: none; display: grid; place-items: center; color: #fff;
    font: 700 calc(var(--u) * .34)/1 ui-sans-serif, system-ui, sans-serif; background: rgba(20, 18, 14, .42);
    border: 2px solid rgba(255, 255, 255, .35); backdrop-filter: blur(2px); -webkit-tap-highlight-color: transparent; }
  #tpad .c.down { background: rgba(240, 201, 112, .6); border-color: #f0c970; }
  #tpad .top { position: absolute; left: 50%; top: calc(var(--u) * .3); transform: translateX(-50%); display: flex; gap: calc(var(--u) * .2); pointer-events: none; }
  #tpad .top .c { position: static; }
  #tpad .c.tog { background: rgba(20, 18, 14, .42); }
  #tpad .c.tog.on { background: rgba(240, 201, 112, .55); border-color: #f0c970; }
  body.touch-play canvas { touch-action: none; }
  #tkb { position: absolute; left: 0; bottom: 0; width: 1px; height: 1px; opacity: .01; border: 0; padding: 0; font-size: 16px; z-index: 4; }
  #tpad .b { width: calc(var(--u) * 1.15); height: calc(var(--u) * 1.15); border-radius: 50%; }
  #tpad .s { width: calc(var(--u) * 1.5); height: calc(var(--u) * .62); border-radius: 99px; font-size: calc(var(--u) * .26); }
  #tpad .sh { width: calc(var(--u) * 1.5); height: calc(var(--u) * .78); border-radius: 14px; font-size: calc(var(--u) * .3); }
  #tpad .stick { width: calc(var(--u) * 2.6); height: calc(var(--u) * 2.6); border-radius: 50%; }
  #tpad .stick.r { width: calc(var(--u) * 1.9); height: calc(var(--u) * 1.9); }
  #tpad .stick i { position: absolute; left: 50%; top: 50%; width: 42%; height: 42%; margin: -21% 0 0 -21%; border-radius: 50%;
    background: rgba(255, 255, 255, .55); border: 2px solid rgba(255, 255, 255, .8); pointer-events: none; }
  #tpad .stick.down { background: rgba(20, 18, 14, .5); border-color: #f0c970; }
  #rotate { display: none; background: #0b0a09; color: #f4efe3; place-items: center; text-align: center; z-index: 9; padding: 24px; }
  #rotate.on { display: grid; }
  #rotate svg { width: 84px; height: 84px; margin-bottom: 14px; animation: rot 2.4s ease-in-out infinite; }
  #rotate b { display: block; font-size: 20px; margin-bottom: 6px; }
  #rotate span { color: #b8b0a0; font-size: 14px; }
  @keyframes rot { 0%, 20% { transform: rotate(0); } 60%, 100% { transform: rotate(-90deg); } }
  `;
  document.head.appendChild(css);

  const pad = document.createElement('div');
  pad.id = 'tpad';
  // bit numbers are the Gamepad API "standard" layout that input_gamepad() takes (see loader.js)
  const B = (bit, label, cls, pos) => `<div class="c ${cls}" data-bit="${bit}" style="${pos}">${label}</div>`;
  const u = 'var(--u)';
  pad.innerHTML = [
    '<div class="c stick l" data-stick="0" style="left:calc(' + u + '*.7);bottom:calc(' + u + '*.6)"><i></i></div>',
    '<div class="c stick r" data-stick="1" style="right:calc(' + u + '*5.1);bottom:calc(' + u + '*.4)"><i></i></div>',
    B(6, 'L2', 'sh', `left:calc(${u}*.5);top:calc(${u}*.4)`),
    B(4, 'L1', 'sh', `left:calc(${u}*.5);top:calc(${u}*1.35)`),
    B(7, 'R2', 'sh', `right:calc(${u}*.5);top:calc(${u}*.4)`),
    B(5, 'R1', 'sh', `right:calc(${u}*.5);top:calc(${u}*1.35)`),
    `<div class="top"><div class="c s tog on" data-act="cursor">CURSOR</div>${B(8, 'SELECT', 's', '')}${B(9, 'START', 's', '')}<div class="c s tog" data-act="kb">KEYS</div></div>`,
    B(3, '△', 'b', `right:calc(${u}*1.85);bottom:calc(${u}*3.1)`),
    B(0, '✕', 'b', `right:calc(${u}*1.85);bottom:calc(${u}*.5)`),
    B(2, '□', 'b', `right:calc(${u}*3.15);bottom:calc(${u}*1.8)`),
    B(1, '○', 'b', `right:calc(${u}*.55);bottom:calc(${u}*1.8)`),
  ].join('');
  stage.appendChild(pad);

  const rot = document.createElement('div');
  rot.id = 'rotate';
  rot.innerHTML = '<div><svg viewBox="0 0 24 24" fill="none" stroke="#f0c970" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">' +
    '<rect x="7" y="2.5" width="10" height="19" rx="2"/><path d="M11 18.5h2"/></svg><b>Rotate your phone</b>' +
    '<span>Hold it sideways to play with the on-screen controller.</span></div>';
  stage.appendChild(rot);

  // ---- touch handling: every control owns the pointer that pressed it, so several can be held at once
  const held = new Map();   // pointerId -> { el, kind }
  const stickVal = [[0, 0], [0, 0]];
  let cursorMode = true;   // menus first: the left stick moves the cursor and ✕ clicks; switch off to walk
  const game = () => window.recomp;
  const setBit = (bit, on) => {
    api.buttons = on ? api.buttons | (1 << bit) : api.buttons & ~(1 << bit);
    if (bit === 0 && cursorMode) game()?._input_mouse_button(0, on ? 1 : 0);
  };
  const publish = () => { api.ax = cursorMode ? [0, 0, stickVal[1][0], stickVal[1][1]] : [stickVal[0][0], stickVal[0][1], stickVal[1][0], stickVal[1][1]]; };
  const moveStick = (el, h, e) => {
    const r = el.getBoundingClientRect(), R = r.width / 2;
    let dx = (e.clientX - (r.left + R)) / R, dy = (e.clientY - (r.top + R)) / R;
    const m = Math.hypot(dx, dy);
    if (m > 1) { dx /= m; dy /= m; }
    const dz = Math.hypot(dx, dy) < 0.12;   // small dead zone so a resting thumb doesn't drift
    stickVal[h.idx] = dz ? [0, 0] : [dx, dy];
    el.firstChild.style.transform = `translate(${dx * R * .55}px, ${dy * R * .55}px)`;
    publish();
  };
  pad.addEventListener('pointerdown', (e) => {
    const el = e.target.closest('.c');
    if (!el || el.dataset.act) return;   // CURSOR / KEYS are plain taps (see the click handler)
    e.preventDefault();
    try { el.setPointerCapture(e.pointerId); } catch (_) {}
    el.classList.add('down');
    if (el.dataset.stick !== undefined) {
      const h = { el, idx: +el.dataset.stick };
      held.set(e.pointerId, h);
      moveStick(el, h, e);
    } else {
      const bit = +el.dataset.bit;
      held.set(e.pointerId, { el, bit });
      setBit(bit, true);
      navigator.vibrate?.(8);
    }
  });
  pad.addEventListener('pointermove', (e) => {
    const h = held.get(e.pointerId);
    if (h && h.idx !== undefined) { e.preventDefault(); moveStick(h.el, h, e); }
  });
  const release = (e) => {
    const h = held.get(e.pointerId);
    if (!h) return;
    held.delete(e.pointerId);
    h.el.classList.remove('down');
    if (h.idx !== undefined) { stickVal[h.idx] = [0, 0]; h.el.firstChild.style.transform = ''; publish(); }
    else setBit(h.bit, false);
  };
  pad.addEventListener('pointerup', release);
  pad.addEventListener('pointercancel', release);
  pad.addEventListener('contextmenu', (e) => e.preventDefault());
  // no page zoom or scroll gestures while playing
  document.addEventListener('touchmove', (e) => { if (api.active) e.preventDefault(); }, { passive: false });


  // ---- cursor: the game draws its own pointer and reads relative motion
  const cursorLoop = () => {
    if (api.active && cursorMode && pad.classList.contains('on')) {
      const [x, y] = stickVal[0], m = game();
      if (m && (x || y)) m._input_mouse_move(Math.round(x * Math.abs(x) * 22), Math.round(y * Math.abs(y) * 22));
    }
    requestAnimationFrame(cursorLoop);
  };
  requestAnimationFrame(cursorLoop);
  // a finger dragging on the bare picture moves the cursor like a trackpad; a quick tap clicks
  const canvas = document.getElementById('canvas');
  let drag = null;
  canvas.addEventListener('pointerdown', (e) => {
    if (e.pointerType !== 'touch' || !api.active) return;
    e.preventDefault();
    drag = { id: e.pointerId, x: e.clientX, y: e.clientY, t: performance.now(), moved: 0 };
  });
  canvas.addEventListener('pointermove', (e) => {
    if (!drag || e.pointerId !== drag.id) return;
    const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    drag.x = e.clientX; drag.y = e.clientY; drag.moved += Math.abs(dx) + Math.abs(dy);
    const k = 1920 / Math.max(300, canvas.getBoundingClientRect().width);   // CSS px -> game pixels
    game()?._input_mouse_move(Math.round(dx * k), Math.round(dy * k));
  });
  const lift = (e) => {
    if (!drag || e.pointerId !== drag.id) return;
    const tap = e.type === 'pointerup' && drag.moved < 12 && performance.now() - drag.t < 350;
    drag = null;
    if (tap) { const m = game(); m?._input_mouse_button(0, 1); setTimeout(() => m?._input_mouse_button(0, 0), 60); }
  };
  canvas.addEventListener('pointerup', lift);
  canvas.addEventListener('pointercancel', lift);

  // ---- keyboard: the game reads keys itself (profile name entry), so a hidden text field raises the phone's
  // keyboard and what is typed in it is sent to the game as key presses
  const kb = document.createElement('input');
  kb.id = 'tkb'; kb.type = 'text'; kb.autocapitalize = 'off'; kb.autocomplete = 'off'; kb.spellcheck = false;
  kb.setAttribute('autocorrect', 'off'); kb.setAttribute('enterkeyhint', 'done');
  stage.appendChild(kb);
  const HIDK = { Enter: 40, Escape: 41, Backspace: 42, Tab: 43, ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82 };
  const tapKey = (usage, shift) => {
    const m = game();
    if (!m) return;
    if (shift) m._input_key_hid(225, 1);
    m._input_key_hid(usage, 1);
    setTimeout(() => { m._input_key_hid(usage, 0); if (shift) m._input_key_hid(225, 0); }, 40);
  };
  const keyQueue = [];
  const pump = () => { const k = keyQueue.shift(); if (k) { tapKey(k[0], k[1]); setTimeout(pump, 70); } else pumping = false; };
  let pumping = false;
  const press = (usage, shift) => { keyQueue.push([usage, shift]); if (!pumping) { pumping = true; pump(); } };
  const charKey = (ch) => {
    if (/[a-z]/.test(ch)) return [4 + ch.charCodeAt(0) - 97, false];
    if (/[A-Z]/.test(ch)) return [4 + ch.charCodeAt(0) - 65, true];
    if (/[1-9]/.test(ch)) return [29 + +ch, false];
    return { '0': [39, false], ' ': [44, false], '-': [45, false], '.': [55, false], ',': [54, false], "'": [52, false], '/': [56, false] }[ch] || null;
  };
  let prev = '';
  kb.addEventListener('input', () => {
    const v = kb.value;
    let i = 0;
    while (i < prev.length && i < v.length && prev[i] === v[i]) i++;
    for (let k = i; k < prev.length; k++) press(42, false);
    for (const ch of v.slice(i)) { const c = charKey(ch); if (c) press(c[0], c[1]); }
    prev = v;
    if (v.length > 48) { kb.value = ''; prev = ''; }
  });
  kb.addEventListener('keydown', (e) => {
    e.stopPropagation();   // the page-wide key handler must not also send what the input event already did
    const u = HIDK[e.key];
    if (u === undefined) return;
    if (e.key === 'Backspace' && kb.value) return;   // the input event handles it
    e.preventDefault();
    press(u, false);
    if (e.key === 'Enter') kb.blur();
  });
  kb.addEventListener('keyup', (e) => e.stopPropagation());
  const kbBtn = pad.querySelector('[data-act="kb"]'), curBtn = pad.querySelector('[data-act="cursor"]');
  kb.addEventListener('focus', () => kbBtn.classList.add('on'));
  kb.addEventListener('blur', () => kbBtn.classList.remove('on'));
  pad.addEventListener('click', (e) => {
    const el = e.target.closest('[data-act]');
    if (!el) return;
    if (el.dataset.act === 'kb') { if (document.activeElement === kb) kb.blur(); else { kb.value = ''; prev = ''; kb.focus(); } }
    else { cursorMode = !cursorMode; curBtn.classList.toggle('on', cursorMode); stickVal[0] = [0, 0]; publish(); }
  });

  const portrait = matchMedia('(orientation: portrait)');
  const refresh = () => {
    const up = portrait.matches;
    rot.classList.toggle('on', api.active && up);
    pad.classList.toggle('on', api.active && !up);
    if (up) { held.clear(); api.buttons = 0; stickVal[0] = stickVal[1] = [0, 0]; publish(); pad.querySelectorAll('.down').forEach((n) => n.classList.remove('down')); }
  };
  portrait.addEventListener('change', refresh);

  // Play was pressed: take the screen. Fullscreen + orientation lock work on Android Chrome; iOS Safari has
  // neither for web pages, so there the stage just fills the viewport and the rotate card does the asking.
  api.start = () => {
    if (api.active) return;
    api.active = true;
    document.body.classList.add('touch-play');
    const fs = stage.requestFullscreen || stage.webkitRequestFullscreen;
    Promise.resolve(fs && fs.call(stage)).catch(() => {}).then(() => screen.orientation?.lock?.('landscape')).catch(() => {});
    refresh();
  };
  const play = document.getElementById('play');
  if (play) play.addEventListener('click', api.start, true);   // capture: runs inside the tap, which fullscreen needs
  if (new URLSearchParams(location.search).has('autoplay')) api.start();
  addEventListener('resize', refresh);
})();
