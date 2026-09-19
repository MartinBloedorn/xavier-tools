/* Builders for the controls that live in a widget's parameter bar.
 *
 * Plain DOM, no framework. Every control is small, stateless apart from its
 * pressed attribute, and reports changes through one callback -- the widget
 * owns the state and writes it to the store.
 */

function el(tag, className, props = {}) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  Object.assign(node, props);
  return node;
}

/** One row of a parameter bar. A widget appends groups to rows, not to the
 * bar itself; every bar is two rows tall, which is what keeps their heights
 * equal without each widget having to agree on a number. */
export function row() {
  return el('div', 'bar-row');
}

/** A labelled cluster of controls, separated from its neighbours by a rule. */
export function group(label, { scroll = false } = {}) {
  const node = el('div', 'bar-group' + (scroll ? ' is-scroll' : ''));
  if (label) node.appendChild(el('span', 'bar-label', { textContent: label }));
  return node;
}

/** A momentary/latching toggle with an indicator LED. */
export function toggle(label, value, onChange, { color } = {}) {
  const node = el('button', 'switch', { type: 'button' });
  node.setAttribute('aria-pressed', String(!!value));
  if (color) node.style.setProperty('--led', color);
  node.appendChild(el('span', 'led'));
  node.appendChild(el('span', null, { textContent: label }));
  node.addEventListener('click', () => {
    const next = node.getAttribute('aria-pressed') !== 'true';
    node.setAttribute('aria-pressed', String(next));
    onChange(next);
  });
  node.set = (v) => node.setAttribute('aria-pressed', String(!!v));
  return node;
}

/** One channel's on/off chip, tinted with that channel's trace colour. */
export function chip(label, value, color, onChange) {
  const node = el('button', 'chip', { type: 'button', textContent: label });
  node.setAttribute('aria-pressed', String(!!value));
  // Untinted chips keep the default accent: a chip that controls more than
  // one series has no one colour to borrow.
  if (color) node.style.setProperty('--chip', color);
  node.addEventListener('click', (event) => {
    const next = node.getAttribute('aria-pressed') !== 'true';
    node.setAttribute('aria-pressed', String(next));
    onChange(next, event);
  });
  node.set = (v) => node.setAttribute('aria-pressed', String(!!v));
  return node;
}

/** A mutually exclusive set of options. */
export function segmented(options, value, onChange) {
  const node = el('div', 'seg');
  const buttons = options.map(([key, label, title]) => {
    const button = el('button', null, { type: 'button', textContent: label });
    if (title) button.title = title;
    button.setAttribute('aria-pressed', String(key === value));
    button.addEventListener('click', () => {
      for (const other of buttons) {
        other.setAttribute('aria-pressed', String(other === button));
      }
      onChange(key);
    });
    node.appendChild(button);
    return button;
  });
  node.set = (v) => {
    options.forEach(([key], i) => buttons[i].setAttribute('aria-pressed', String(key === v)));
  };
  return node;
}

/** A dropdown, for choices too numerous for a segmented control. */
export function select(options, value, onChange) {
  const node = el('select', 'num');
  for (const [key, label] of options) {
    const option = el('option', null, { value: String(key), textContent: label });
    node.appendChild(option);
  }
  node.value = String(value);
  node.addEventListener('change', () => onChange(node.value));
  node.set = (v) => { node.value = String(v); };
  return node;
}

/** A slider with a live readout. `format` renders the readout text. */
export function slider(opts, onChange) {
  const { min, max, step, value, format = (v) => v.toFixed(2) } = opts;
  const wrap = el('span', null);
  wrap.style.display = 'inline-flex';
  wrap.style.alignItems = 'center';
  wrap.style.gap = '6px';
  const input = el('input', 'slider', { type: 'range' });
  input.min = String(min);
  input.max = String(max);
  input.step = String(step);
  input.value = String(value);
  const readout = el('span', 'readout', { textContent: format(value) });
  input.addEventListener('input', () => {
    const next = parseFloat(input.value);
    readout.textContent = format(next);
    onChange(next);
  });
  wrap.appendChild(input);
  wrap.appendChild(readout);
  wrap.set = (v) => { input.value = String(v); readout.textContent = format(v); };
  return wrap;
}

/** A plain push button. */
export function button(label, onClick, title) {
  const node = el('button', 'chip', { type: 'button', textContent: label });
  if (title) node.title = title;
  node.addEventListener('click', onClick);
  return node;
}

export function label(textContent) {
  return el('span', 'bar-label', { textContent });
}
