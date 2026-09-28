/* Run: node tests/TimelineClips.test.js
 * Exercises the shipped ExtendScript and generated expressions against a small
 * AE-like host. This is not a substitute for opening the plug-in in AE.
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../scripts/TimelineClips.jsx'), 'utf8');
const controlNames = ['Motion A index', 'Motion B index', 'Motion A time (seconds)',
  'Motion B time (seconds)', 'Transition A to B (%)'];
const expressionNames = ['Expression A index', 'Expression B index',
  'Expression A weight (%)', 'Expression B weight (%)'];
// Match names observed in real AE: validation/release-1.2.0/ae-host/schema.txt.
// Keep this fixture independent of the implementation's property resolver.
const parameterDiskIds = {
  'Loop motion': 103, 'Motion A slot': 109, 'Motion B slot': 110,
  'Keyframe motion time': 111, 'Motion A time (seconds)': 112,
  'Motion B time (seconds)': 113, 'Transition A to B (%)': 114,
  'Timeline binding': 115, 'Expression A slot': 118, 'Expression B slot': 119,
  'Expression A weight (%)': 120, 'Expression B weight (%)': 121,
  'Expression binding': 122, 'Motion A index': 134, 'Motion B index': 135,
  'Expression A index': 136, 'Expression B index': 137
};
const parameterMatchName = name => `L2DAE Native Renderer-${String(parameterDiskIds[name]).padStart(4, '0')}`;
function parameter(effect, name) {
  // Independent breadth-first fixture lookup also supports host topic groups.
  const pending = [effect], match = parameterMatchName(name);
  while (pending.length) {
    const parent = pending.shift(), direct = parent.property(match);
    if (direct && direct.matchName === match) return direct;
    for (let i = 1; i <= parent.numProperties; ++i) {
      const child = parent.property(i);
      if (child && typeof child.property === 'function') pending.push(child);
    }
  }
  return null;
}
let assertions = 0;
let nextItemId = 1;
function equal(a, b, message) { assert.deepEqual(a, b, message); assertions++; }
function ok(a, message) { assert.ok(a, message); assertions++; }
function near(a, b, message) { ok(Math.abs(a - b) < 1e-7, `${message || ''}: ${a} != ${b}`); }
function throws(fn, pattern) { assert.throws(fn, pattern); assertions++; }
function copy(value) { return JSON.parse(JSON.stringify(value)); }

class Property {
  constructor(value = 0) {
    this.value = value; this.numKeys = 0; this._expression = '';
    this.expressionEnabled = false; this.canSetExpression = true; this.expressionError = '';
  }
  get expression() { return this._expression; }
  set expression(value) {
    if (this.failExpressionOnce && value) { this.failExpressionOnce = false; throw Error('Injected expression assignment failure'); }
    if (value) new vm.Script(value); // Actual expression source must parse.
    this._expression = value;
    this.expressionError = this.injectExpressionError && value ? 'Injected AE expression error' : '';
  }
  setValue(value) { this.value = value; }
  clone() {
    const p = new Property(this.value);
    p.numKeys = this.numKeys; p._expression = this._expression;
    p.expressionEnabled = this.expressionEnabled; p.canSetExpression = this.canSetExpression;
    p.name = this.name; p.matchName = this.matchName;
    return p;
  }
}
class MarkerValue {
  constructor(comment) {
    this.comment = comment; this.duration = 0; this.label = 0;
    this.chapter = ''; this.url = ''; this.parameters = {};
  }
  getParameters() { return copy(this.parameters); }
  setParameters(value) { this.parameters = copy(value); }
}
class Markers {
  constructor() { this.keys = []; }
  get numKeys() { return this.keys.length; }
  keyValue(index) { return Object.assign(new MarkerValue(''), copy(this.keys[index - 1].value)); }
  key(index) { return this.keyValue(index); }
  setValueAtTime(time, value) {
    const old = this.keys.find(k => k.time === time);
    if (old) old.value = copy(value); else this.keys.push({time, value: copy(value)});
    this.keys.sort((a, b) => a.time - b.time);
  }
  setValueAtKey(index, value) {
    if (this.failOnce) { this.failOnce = false; throw Error('Injected marker write failure'); }
    this.keys[index - 1].value = copy(value);
  }
  clone() { const m = new Markers(); m.keys = copy(this.keys); return m; }
}
class Effect {
  constructor(binding) {
    this.matchName = 'L2DAE Native Renderer';
    this.name = 'AeGO Flash'; this.props = {};
    for (const n of controlNames) this.props[n] = new Property(n.indexOf('slot') >= 0 ? 1 : 0);
    for (const n of expressionNames) this.props[n] = new Property(n.indexOf('slot') >= 0 ? 1 : 0);
    for (const n of ['Motion A slot', 'Motion B slot', 'Expression A slot', 'Expression B slot']) this.props[n] = new Property(1);
    this.props['Timeline binding'] = new Property(binding);
    this.props['Expression binding'] = new Property(0);
    this.props['Keyframe motion time'] = new Property(0);
    this.props['Loop motion'] = new Property(1);
    for (const name of Object.keys(this.props)) {
      this.props[name].name = name;
      this.props[name].matchName = parameterMatchName(name);
    }
  }
  property(name) {
    if (this.lookups) this.lookups.push(name);
    if (this.groupRoot) return this.groupRoot.property(name);
    const properties = Object.values(this.props);
    if (typeof name === 'number') return properties[name - 1] || null;
    return properties.find(p => p.matchName === name) || properties.find(p => p.name === name) || null;
  }
  get numProperties() { return this.groupRoot ? this.groupRoot.numProperties : Object.keys(this.props).length; }
  get propertyIndex() { return this.layer.effects.indexOf(this) + 1; }
  clone() {
    const e = new Effect(0); e.matchName = this.matchName; e.name = this.name;
    for (const n in this.props) e.props[n] = this.props[n].clone();
    if (this.groupRoot) groupParameterFixture(e);
    return e;
  }
}
class ParameterGroup {
  constructor(name, matchName, children) { this.name = name; this.matchName = matchName; this.children = children; this.lookups = []; }
  get numProperties() { return this.children.length; }
  property(name) {
    this.lookups.push(name);
    if (typeof name === 'number') return this.children[name - 1] || null;
    return this.children.find(p => p.matchName === name) || this.children.find(p => p.name === name) || null;
  }
}
function groupParameterFixture(effect) {
  // A host-tree fixture, deliberately nesting internal controls too, exercises
  // recursive stable-ID lookup independently of the current native panel order.
  const motionNames = [...controlNames, 'Timeline binding', 'Loop motion', 'Keyframe motion time', 'Motion A slot', 'Motion B slot'];
  const faceNames = [...expressionNames, 'Expression binding', 'Expression A slot', 'Expression B slot'];
  const motion = new ParameterGroup('动作控制 / 任意名称', 'fixture-motion', motionNames.map(n => effect.props[n]));
  const face = new ParameterGroup('表情控制 / 任意名称', 'fixture-expression', faceNames.map(n => effect.props[n]));
  const inner = new ParameterGroup('第二层分组', 'fixture-inner', [motion, face]);
  effect.groupRoot = new ParameterGroup('效果根', effect.matchName, [inner]);
  return [effect.groupRoot, inner, motion, face];
}
class Layer {
  constructor(comp, duration, effects = []) {
    this.comp = comp; this.name = 'Layer'; this.duration = duration; this.effects = effects;
    for (const e of effects) e.layer = this;
    this._startTime = 0; this.inPoint = 0; this.outPoint = duration;
    this.stretch = 100; this.locked = false; this.guideLayer = false; this.enabled = true;
    this.label = 0; this.comment = ''; this.marker = new Markers();
    this.selected = false; this.source = null; this.nullLayer = false;
  }
  get index() { return this.comp._layers.indexOf(this) + 1; }
  get startTime() { return this._startTime; }
  set startTime(value) {
    const delta = value - this._startTime;
    this.inPoint += delta; this.outPoint += delta;
    for (const k of this.marker.keys) k.time += delta;
    this._startTime = value;
  }
  property(name) {
    if (name === 'ADBE Marker') return this.marker;
    if (name === 'ADBE Effect Parade') return {
      numProperties: this.effects.length, property: index => this.effects[index - 1]
    };
    return null;
  }
  sourceTime(time) { return (time - this.startTime) * 100 / this.stretch; }
  cloneInto(comp) {
    const l = new Layer(comp, this.duration, this.effects.map(e => e.clone()));
    for (const n of ['name', '_startTime', 'inPoint', 'outPoint', 'stretch', 'locked', 'guideLayer', 'enabled', 'label', 'comment', 'source', 'selected', 'nullLayer']) l[n] = this[n];
    l.marker = this.marker.clone(); return l;
  }
  duplicate() {
    const l = this.cloneInto(this.comp);
    this.comp._layers.splice(this.index - 1, 0, l);
    for (const other of this.comp._layers) other.selected = other === l;
    return l;
  }
  remove() { this.comp._layers.splice(this.index - 1, 1); }
}
class FootageItem {
  constructor(project) { this.id = nextItemId++; this.project = project; this._name = `空 ${this.id}`; }
  get name() { return this._name; }
  set name(value) {
    if (this.failRenameOnce) { this.failRenameOnce = false; throw Error('Injected source rename failure'); }
    this._name = value;
  }
  get usedIn() { return this.project.items.filter(c => c instanceof CompItem && c._layers.some(l => l.source === this)); }
  remove() { this.project.items.splice(this.project.items.indexOf(this), 1); }
}
class CompItem {
  constructor(duration = 20) {
    this.id = nextItemId++;
    this._layers = []; this.duration = duration; this.frameDuration = 1 / 30; this.time = 0;
    this.layers = {addNull: duration => {
      if (this.failAddNull) throw Error('Injected addNull failure');
      const l = new Layer(this, duration);
      l.nullLayer = true;
      if (this.reuseSource) l.source = this.reuseSource;
      else if (this.project) { l.source = new FootageItem(this.project); this.project.items.push(l.source); }
      this._layers.unshift(l);
      for (const other of this._layers) other.selected = other === l;
      return l;
    }};
  }
  get numLayers() { return this._layers.length; }
  layer(index) { return this._layers[index - 1]; }
  model(binding) { const l = new Layer(this, this.duration, [new Effect(binding)]); this._layers.push(l); return l; }
  clone() { const c = new CompItem(this.duration); c.project = this.project; c.time = this.time; c._layers = this._layers.map(l => l.cloneInto(c)); return c; }
}
function host() {
  const comp = new CompItem(), model = comp.model(100), effect = model.effects[0];
  const project = {items: [comp], get numItems() { return this.items.length; }, item(i) { return this.items[i - 1]; }};
  comp.project = project; model.selected = true;
  const app = {project, depth: 0, undoGroups: 0,
    beginUndoGroup() { this.depth++; this.undoGroups++; }, endUndoGroup() { this.depth--; }};
  const context = vm.createContext({app, CompItem, MarkerValue});
  vm.runInContext(source, context, {filename: 'TimelineClips.jsx'});
  return {comp, model, effect, app, context, add: payload => context.L2DAE_addClip(payload), batch: payloads => context.L2DAE_addClips(payloads)};
}
function payload(binding = 100, slot = 1, duration = 4, previousBinding = 0, append = true) {
  // Most historical fixtures deliberately retain their original linear curve.
  // Tests below separately exercise omitted/default and explicit new imports.
  return {binding, previousBinding, slot, duration, label: '待机 → 微笑.motion3.json', append, transitionFrames: 6, transitionCurve: 0};
}
function expressionPayload(binding = 200, slot = 1, duration = 3, previousBinding = 0, append = false) {
  return {kind: 'expression', binding, previousBinding, slot, duration, label: '微笑.exp3.json', append, transitionFrames: 6};
}
function tagged(comp, owner, kind = 'motion') {
  const prefix = kind === 'expression' ? 'L2DEXPR' : 'L2DCLIP';
  return comp._layers.filter(l => l.marker.keys.some(k =>
    (k.value.parameters.l2daeKind === kind && Number(k.value.parameters.l2daeOwner) === owner) ||
    k.value.comment.startsWith(`${prefix}|${owner}|`))); 
}
function evaluate(effect, comp, time) {
  return controlNames.map(n => vm.runInNewContext(parameter(effect, n).expression, {thisComp: comp, time}));
}
function legacyOvershootPayload(...args) {
  const result = payload(...args);
  result.transitionCurve = 2;
  return result;
}
function defaultMotionPayload(...args) {
  const result = payload(...args);
  delete result.transitionCurve;
  return result;
}
function evaluateExpressions(effect, comp, time) {
  return expressionNames.map(n => vm.runInNewContext(parameter(effect, n).expression, {thisComp: comp, time}));
}
function clockResult(result, expected) { expected.forEach((x, i) => near(result[i], x, `result[${i}]`)); }
function rebind(h, binding) { parameter(h.effect, 'Timeline binding').setValue(binding); }
function rebindExpression(h, binding = 200) { parameter(h.effect, 'Expression binding').setValue(binding); }
function channelSnapshot(effect, comp, kind) {
  const expression = kind === 'expression';
  const names = expression ? [...expressionNames, 'Expression binding'] :
    [...controlNames, 'Timeline binding', 'Keyframe motion time', 'Loop motion'];
  const prefix = expression ? 'L2DEXPR|' : 'L2DCLIP|';
  return JSON.stringify({properties: names.map(n => {
    const p = parameter(effect, n);
    return {name: n, value: p.value, expression: p.expression, enabled: p.expressionEnabled, keys: p.numKeys};
  }), clips: comp._layers.filter(l => l.marker.keys.some(k => k.value.parameters.l2daeKind === kind || k.value.comment.startsWith(prefix))).map(l =>
    ({name: l.name, marker: l.marker.keys, start: l.startTime, begin: l.inPoint, end: l.outPoint,
      stretch: l.stretch, enabled: l.enabled, locked: l.locked, comment: l.comment}))});
}
function snapshot(comp) {
  return JSON.stringify(comp._layers.map(l => ({name: l.name, marker: l.marker.keys, start: l.startTime,
    source: l.source ? {id: l.source.id, name: l.source.name} : null,
    begin: l.inPoint, end: l.outPoint, comment: l.comment, selected: l.selected,
    effects: l.effects.map(e => Object.fromEntries(Object.entries(e.props).map(([n, p]) =>
      [n, {value: p.value, expression: p.expression, enabled: p.expressionEnabled}])))})));
}

// Import, source-clock sampling, append overlap, natural hold, rename, reorder.
{
  const h = host(); h.comp.time = 2;
  equal(h.add(payload()), 'OK'); equal(h.app.depth, 0);
  const a = tagged(h.comp, 100)[0];
  equal(h.model.selected, true); equal(a.selected, false);
  equal([a.inPoint, a.outPoint, a.startTime], [2, 6, 2]);
  equal([a.guideLayer, a.enabled], [true, true]); ok(a.name.includes('待机'));
  equal(h.effect.property('Keyframe motion time').value, 1);
  equal(h.effect.property('Loop motion').value, 0);
  clockResult(evaluate(h.effect, h.comp, 0), [1, 1, 0, 0, 0]);
  clockResult(evaluate(h.effect, h.comp, 3), [1, 1, 1, 1, 0]);
  clockResult(evaluate(h.effect, h.comp, 6), [1, 1, 4, 4, 0]);
  rebind(h, 101); equal(h.add(payload(101, 2, 3, 100)), 'OK');
  equal(tagged(h.comp, 100).length, 0); equal(tagged(h.comp, 101).length, 2);
  const b = tagged(h.comp, 101).find(l => l !== a);
  equal(h.model.selected, true); equal(b.selected, false);
  near(b.inPoint, 5.8); near(b.outPoint, 8.8);
  clockResult(evaluate(h.effect, h.comp, 5.9), [1, 2, 3.9, .1, 50]);
  clockResult(evaluate(h.effect, h.comp, 6.3), [2, 2, .5, .5, 0]);
  b.startTime = 8; b.name = '用户改名的动作'; h.comp._layers.reverse();
  clockResult(evaluate(h.effect, h.comp, 7), [1, 1, 4, 4, 0]);
  clockResult(evaluate(h.effect, h.comp, 9), [2, 2, 1, 1, 0]);
  clockResult(evaluate(h.effect, h.comp, 19), [2, 2, 3, 3, 0]);
  b.enabled = false;
  clockResult(evaluate(h.effect, h.comp, 9), [1, 1, 4, 4, 0]);
  a.enabled = false;
  clockResult(evaluate(h.effect, h.comp, 9), [1, 1, 0, 0, 0]);
  a.enabled = b.enabled = true;
  a.inPoint = 3; a.outPoint = 5;
  clockResult(evaluate(h.effect, h.comp, 1), [1, 1, 1, 1, 0]);
  clockResult(evaluate(h.effect, h.comp, 7), [1, 1, 3, 3, 0]);
  const d = a.duplicate(); d.startTime += 12;
  clockResult(evaluate(h.effect, h.comp, 16), [1, 1, 2, 2, 0]);
  // Splitting preserves startTime, so the second piece continues the same motion.
  d.outPoint = 16; const split = d.duplicate(); split.inPoint = 16; split.outPoint = 17;
  clockResult(evaluate(h.effect, h.comp, 16.5), [1, 1, 2.5, 2.5, 0]);
  for (const l of tagged(h.comp, 101)) l.remove();
  clockResult(evaluate(h.effect, h.comp, 3), [1, 1, 0, 0, 0]);
}

// Native positive/negative stretch and reversed in/out points.
{
  const h = host(); h.add(payload()); const a = tagged(h.comp, 100)[0];
  a.stretch = 200; a.outPoint = 8;
  clockResult(evaluate(h.effect, h.comp, 6), [1, 1, 3, 3, 0]);
  a.stretch = -100; a.startTime = 4; a.inPoint = 4; a.outPoint = 0;
  clockResult(evaluate(h.effect, h.comp, 0), [1, 1, 4, 4, 0]);
  clockResult(evaluate(h.effect, h.comp, 1.5), [1, 1, 2.5, 2.5, 0]);
  clockResult(evaluate(h.effect, h.comp, 4), [1, 1, 0, 0, 0]);
}

// A shorter clip nested inside a long one fades in/out, avoiding a jump at its end.
{
  const h = host(); h.add(payload(100, 1, 10));
  h.comp.time = 3; rebind(h, 101); h.add(payload(101, 2, 3, 100, false));
  clockResult(evaluate(h.effect, h.comp, 3), [1, 2, 3, 0, 0]);
  clockResult(evaluate(h.effect, h.comp, 3.1), [1, 2, 3.1, .1, 50]);
  clockResult(evaluate(h.effect, h.comp, 4), [1, 2, 4, 1, 100]);
  clockResult(evaluate(h.effect, h.comp, 5.9), [1, 2, 5.9, 2.9, 50]);
  clockResult(evaluate(h.effect, h.comp, 6), [1, 1, 6, 6, 0]);
  // A synthetic pose value proves both sides approach the same boundary pose.
  const pose = t => {
    const r = evaluate(h.effect, h.comp, t);
    return (100 * r[0] + r[2]) * (1 - r[4] / 100) + (100 * r[1] + r[3]) * r[4] / 100;
  };
  ok(Math.abs(pose(3 - 1e-8) - pose(3 + 1e-8)) < 1e-4);
  ok(Math.abs(pose(6 - 1e-8) - pose(6 + 1e-8)) < 1e-4);
  // Very short inserts divide their duration between incoming/outgoing fades.
  const b = tagged(h.comp, 101).find(l => Number(l.marker.key(1).parameters.l2daeSlot) === 2);
  b.outPoint = 3.1;
  near(evaluate(h.effect, h.comp, 3.05)[4], 100);
  near(evaluate(h.effect, h.comp, 3.075)[4], 50);
}

// Three-way overlap deterministically uses the two latest starts.
{
  const h = host(); h.add(payload(100, 1, 8));
  h.comp.time = 2; rebind(h, 101); h.add(payload(101, 2, 8, 100, false));
  h.comp.time = 4; rebind(h, 102); h.add(payload(102, 3, 8, 101, false));
  clockResult(evaluate(h.effect, h.comp, 5), [2, 3, 3, 1, 100 / 6]);
  // Equal starts use stable layer index ordering, independent of layer name.
  const three = tagged(h.comp, 102).find(l => Number(l.marker.key(1).parameters.l2daeSlot) === 3);
  three.startTime = 2;
  clockResult(evaluate(h.effect, h.comp, 3), [2, 3, 1, 1, 12.5]);
}

// Same-comp duplicated model gets independent copies. Original controls/clips stay intact.
{
  const h = host(); h.add(payload());
  const original = h.effect, originalClip = tagged(h.comp, 100)[0];
  const duplicated = h.model.duplicate(); h.effect = duplicated.effects[0]; h.model = duplicated;
  rebind(h, 101); h.add(payload(101, 2, 3, 100));
  equal(tagged(h.comp, 100).length, 1); equal(tagged(h.comp, 101).length, 2);
  ok(originalClip.marker.key(1).parameters.l2daeOwner === '100');
  clockResult(evaluate(original, h.comp, 4), [1, 1, 4, 4, 0]);
  clockResult(evaluate(h.effect, h.comp, 3.9), [1, 2, 3.9, .1, 50]);
}

// Duplicated compositions are independent without duplicating their own clips again.
{
  const h = host(); h.add(payload()); const first = h.comp;
  const second = first.clone(); h.app.project.items.push(second);
  h.comp = second; h.model = second._layers.find(l => l.effects.length); h.effect = h.model.effects[0];
  rebind(h, 101); h.add(payload(101, 2, 3, 100));
  equal(tagged(first, 100).length, 1); equal(tagged(second, 100).length, 0);
  equal(tagged(second, 101).length, 2);
}

// Retry recovers previous ownership from our expression, even after failed native binding.
{
  const h = host(); h.add(payload());
  rebind(h, 101); h.comp.failAddNull = true;
  throws(() => h.add(payload(101, 2, 3, 100)), /Injected/);
  equal(tagged(h.comp, 100).length, 1); equal(tagged(h.comp, 101).length, 0);
  clockResult(evaluate(h.effect, h.comp, 2), [1, 1, 2, 2, 0]);
  h.comp.failAddNull = false; rebind(h, 102);
  h.add(payload(102, 2, 3, 101));
  equal(tagged(h.comp, 100).length, 0); equal(tagged(h.comp, 102).length, 2);
}

// AE's Windows line-ending normalization does not break ownership on a later import.
for (const newline of ['\r\n', '\r']) {
  const h = host(); h.add(payload());
  for (const n of controlNames) h.effect.property(n).expression = h.effect.property(n).expression.replace(/\n/g, newline);
  rebind(h, 101); h.add(payload(101, 2, 3, 100));
  equal(tagged(h.comp, 100).length, 0); equal(tagged(h.comp, 101).length, 2);
}

// Preflight rejects ambiguous/deleted targets, edited controls, locks and bad input without mutation.
for (const mutate of [
  h => { h.effect.property(controlNames[0]).numKeys = 1; },
  h => { h.effect.property(controlNames[1]).expression = 'time * 2'; },
  h => { h.effect.property(controlNames[2]).canSetExpression = false; },
  h => { h.effect.property('Keyframe motion time').numKeys = 1; },
  h => { h.effect.property('Loop motion').expression = '1'; },
  h => { h.model.locked = true; },
  h => { h.model.duplicate(); },
  h => { h.effect.property('Timeline binding').setValue(999); }
]) {
  const h = host(); mutate(h); const before = snapshot(h.comp);
  throws(() => h.add(payload()), /AeGO Flash:/); equal(snapshot(h.comp), before); equal(h.app.undoGroups, 0);
}
for (const invalid of [
  {...payload(), binding: 0}, {...payload(), binding: Infinity}, {...payload(), slot: 16777217},
  {...payload(), slot: 1.5}, {...payload(), duration: NaN}, {...payload(), duration: 0},
  {...payload(), append: 1}, {...payload(), label: null}
]) {
  const h = host(); throws(() => h.add(invalid), /无效/); equal(h.app.undoGroups, 0);
}
{
  const h = host(); h.add(payload()); rebind(h, 101);
  h.effect.property(controlNames[1]).expression = '';
  const before = snapshot(h.comp);
  throws(() => h.add(payload(101, 2, 3, 100)), /已被移除/); equal(snapshot(h.comp), before);
}
{
  const h = host(); h.add(payload()); rebind(h, 101); tagged(h.comp, 100)[0].locked = true;
  const before = snapshot(h.comp);
  throws(() => h.add(payload(101, 2, 3, 100)), /解锁/); equal(snapshot(h.comp), before);
}

// Failure after mutations restores expressions, manual/loop values, metadata, and created layers.
for (const inject of [
  h => { h.effect.property(controlNames[3]).failExpressionOnce = true; },
  h => { h.effect.property(controlNames[3]).injectExpressionError = true; },
  h => { tagged(h.comp, 100)[0].marker.failOnce = true; }
]) {
  const h = host(); h.add(payload()); rebind(h, 101); inject(h);
  const before = snapshot(h.comp), itemsBefore = h.app.project.numItems;
  throws(() => h.add(payload(101, 2, 3, 100)), /Injected/);
  equal(snapshot(h.comp), before); equal(h.app.depth, 0); equal(h.app.project.numItems, itemsBefore);
}
{
  const h = host(); h.effect.property(controlNames[2]).failExpressionOnce = true;
  const before = snapshot(h.comp);
  throws(() => h.add(payload()), /Injected/); equal(snapshot(h.comp), before);
  equal(h.effect.property('Loop motion').value, 1); equal(h.effect.property('Keyframe motion time').value, 0);
  equal(h.app.project.numItems, 1); equal(h.model.selected, true);
}

// Preserve a different selection (for a locked Effect Controls panel), including rollback.
for (const fail of [false, true]) {
  const h = host(), unrelated = h.comp.layers.addNull(2);
  h.model.selected = false; unrelated.selected = true;
  if (fail) {
    h.effect.property(controlNames[2]).failExpressionOnce = true;
    throws(() => h.add(payload()), /Injected/);
  } else h.add(payload());
  equal(h.model.selected, false); equal(unrelated.selected, true);
  for (const clip of tagged(h.comp, 100)) equal(clip.selected, false);
}

// A host may reuse a pre-existing footage item; rollback must never delete it.
{
  const h = host(), preExisting = new FootageItem(h.app.project);
  h.app.project.items.push(preExisting); h.comp.reuseSource = preExisting;
  h.effect.property(controlNames[2]).failExpressionOnce = true;
  throws(() => h.add(payload()), /Injected/);
  ok(h.app.project.items.includes(preExisting)); equal(preExisting.usedIn.length, 0);
}

// Preserve other users' metadata and malformed/nonmatching clip markers.
{
  const h = host(); h.add(payload()); const a = tagged(h.comp, 100)[0];
  a.comment = '用户备注';
  const metadata = a.marker.keyValue(1); metadata.url = 'https://example.test/'; metadata.parameters.keep = 'yes';
  a.marker.setValueAtKey(1, metadata); a.marker.setValueAtTime(1, new MarkerValue('用户标记'));
  const unrelated = h.comp.layers.addNull(10); unrelated.comment = '不要改';
  unrelated.marker.setValueAtTime(0, new MarkerValue('L2DCLIP|100|0|4'));
  unrelated.marker.setValueAtTime(1, new MarkerValue('L2DCLIP|999|8|4'));
  rebind(h, 101); h.add(payload(101, 2, 3, 100));
  equal(a.comment, '用户备注'); equal(a.marker.key(1).url, 'https://example.test/');
  equal(a.marker.key(1).parameters.keep, 'yes'); equal(a.marker.key(2).comment, '用户标记');
  equal(unrelated.comment, '不要改'); equal(unrelated.marker.key(1).comment, 'L2DCLIP|100|0|4');
}

// Composition boundaries and tiny motion durations are explicit.
{
  const h = host(); h.comp.duration = 5; h.comp.time = 3; h.add(payload());
  const a = tagged(h.comp, 100)[0]; equal([a.inPoint, a.outPoint], [3, 5]); equal(h.comp.duration, 5);
  clockResult(evaluate(h.effect, h.comp, 5), [1, 1, 2, 2, 0]);
}
for (const time of [-1, 20, 21, 19.999]) {
  const h = host(); h.comp.time = time;
  throws(() => h.add(payload()), /剩余时长不足/); equal(h.app.undoGroups, 0);
}
{
  const h = host(); throws(() => h.add(payload(100, 1, .0001)), /剩余时长不足/);
}

// Expression defaults, neutral edges, native edits, mute, and deletion.
{
  const h = host(); rebindExpression(h); h.comp.time = 2;
  const motionBefore = channelSnapshot(h.effect, h.comp, 'motion');
  equal(h.add({kind: 'expression', binding: 200, slot: 1, label: '微笑.exp3.json', transitionFrames: 6}), 'OK');
  equal(channelSnapshot(h.effect, h.comp, 'motion'), motionBefore);
  const a = tagged(h.comp, 200, 'expression')[0];
  equal([a.inPoint, a.outPoint], [2, 5]); equal(a.name, '微笑');
  equal([a.guideLayer, a.enabled, a.selected, h.model.selected], [true, true, false, true]);
  equal(a.marker.key(1).comment, '微笑');
  equal(a.marker.key(1).parameters.l2daeKind, 'expression');
  clockResult(evaluateExpressions(h.effect, h.comp, 1), [1, 1, 0, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 2), [1, 1, 0, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 2.1), [1, 1, 50, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 3), [1, 1, 100, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 4.9), [1, 1, 50, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 5), [1, 1, 0, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 10), [1, 1, 0, 0]);
  a.startTime = 5; a.name = '用户改名'; h.comp._layers.reverse();
  clockResult(evaluateExpressions(h.effect, h.comp, 6), [1, 1, 100, 0]);
  a.inPoint = 6; a.outPoint = 6.3;
  clockResult(evaluateExpressions(h.effect, h.comp, 6.075), [1, 1, 50, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 6.15), [1, 1, 100, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 6.225), [1, 1, 50, 0]);
  a.stretch = -100; a.inPoint = 10; a.outPoint = 7;
  clockResult(evaluateExpressions(h.effect, h.comp, 7.1), [1, 1, 50, 0]);
  a.enabled = false;
  clockResult(evaluateExpressions(h.effect, h.comp, 8), [1, 1, 0, 0]);
  a.remove();
  clockResult(evaluateExpressions(h.effect, h.comp, 8), [1, 1, 0, 0]);
  equal(channelSnapshot(h.effect, h.comp, 'motion'), motionBefore);
}

// Even a very short overlap crossfades at full strength, without dipping to neutral.
{
  const h = host(); rebindExpression(h); h.add(expressionPayload());
  h.comp.time = 2.9; rebindExpression(h, 201); h.add(expressionPayload(201, 2, 3, 200));
  equal(tagged(h.comp, 200, 'expression').length, 0);
  clockResult(evaluateExpressions(h.effect, h.comp, 2.85), [1, 1, 100, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 2.9), [1, 2, 100, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 2.95), [1, 2, 50, 50]);
  clockResult(evaluateExpressions(h.effect, h.comp, 3), [2, 2, 100, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 5.8), [2, 2, 50, 0]);
  for (let t = 2.8; t < 3.2; t += .01) {
    const weights = evaluateExpressions(h.effect, h.comp, t);
    near(weights[2] + weights[3], 100);
  }
  // Connected chains only fade at the outside edges of the whole overlap group.
  h.comp.time = 5.8; rebindExpression(h, 202); h.add(expressionPayload(202, 3, 3, 201));
  clockResult(evaluateExpressions(h.effect, h.comp, 5.75), [2, 2, 100, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 5.85), [2, 3, 50, 50]);
  clockResult(evaluateExpressions(h.effect, h.comp, 8.7), [3, 3, 50, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 9), [1, 1, 0, 0]);
}

// Nested expression returns to the underlying expression, then both return to neutral.
{
  const h = host(); rebindExpression(h); h.add(expressionPayload(200, 1, 10));
  h.comp.time = 3; rebindExpression(h, 201); h.add(expressionPayload(201, 2, 3, 200));
  clockResult(evaluateExpressions(h.effect, h.comp, 3), [1, 2, 100, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 3.1), [1, 2, 50, 50]);
  clockResult(evaluateExpressions(h.effect, h.comp, 4), [1, 2, 0, 100]);
  clockResult(evaluateExpressions(h.effect, h.comp, 5.9), [1, 2, 50, 50]);
  clockResult(evaluateExpressions(h.effect, h.comp, 6), [1, 1, 100, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 9.9), [1, 1, 50, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 10), [1, 1, 0, 0]);
  const pose = t => { const r = evaluateExpressions(h.effect, h.comp, t); return r[0] * r[2] + r[1] * r[3]; };
  ok(Math.abs(pose(3 - 1e-8) - pose(3 + 1e-8)) < 1e-4);
  ok(Math.abs(pose(6 - 1e-8) - pose(6 + 1e-8)) < 1e-4);
}

// Touching clips and gaps fade through neutral; a muted clip cannot extend an envelope.
{
  const h = host(); rebindExpression(h); h.add(expressionPayload());
  h.comp.time = 3; rebindExpression(h, 201); h.add(expressionPayload(201, 2, 3, 200));
  clockResult(evaluateExpressions(h.effect, h.comp, 2.9), [1, 1, 50, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 3), [2, 2, 0, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 3.1), [2, 2, 50, 0]);
  const b = tagged(h.comp, 201, 'expression').find(l => Number(l.marker.key(1).parameters.l2daeSlot) === 2);
  b.startTime = 5;
  clockResult(evaluateExpressions(h.effect, h.comp, 4), [1, 1, 0, 0]);
  b.startTime = 2.9; b.enabled = false;
  clockResult(evaluateExpressions(h.effect, h.comp, 2.9), [1, 1, 50, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 4), [1, 1, 0, 0]);
}

// Three-way overlap remains deterministic, taking the two newest starts.
{
  const h = host(); rebindExpression(h); h.add(expressionPayload(200, 1, 8));
  h.comp.time = 2; rebindExpression(h, 201); h.add(expressionPayload(201, 2, 8, 200));
  h.comp.time = 4; rebindExpression(h, 202); h.add(expressionPayload(202, 3, 8, 201));
  clockResult(evaluateExpressions(h.effect, h.comp, 5), [2, 3, 100 * 5 / 6, 100 / 6]);
}

// Channels remain independent even when binding numbers coincide or other controls are animated.
{
  const h = host(); h.add(payload()); tagged(h.comp, 100)[0].locked = true;
  h.effect.property('Loop motion').numKeys = 2;
  h.effect.property('Loop motion').expression = 'time > 2';
  h.effect.property('Keyframe motion time').numKeys = 2;
  rebindExpression(h, 100); // Same number is valid because the namespaces differ.
  const motionBefore = channelSnapshot(h.effect, h.comp, 'motion');
  h.add(expressionPayload(100));
  equal(channelSnapshot(h.effect, h.comp, 'motion'), motionBefore);
  clockResult(evaluate(h.effect, h.comp, 2), [1, 1, 2, 2, 0]);
  rebindExpression(h, 101); h.comp.time = 1;
  h.add(expressionPayload(101, 2, 3, 100));
  equal(channelSnapshot(h.effect, h.comp, 'motion'), motionBefore);
  // Motion import also preserves arbitrary expression-channel keyframes and locks.
  tagged(h.comp, 100)[0].locked = false;
  h.effect.property('Loop motion').numKeys = 0; h.effect.property('Loop motion').expression = '';
  h.effect.property('Keyframe motion time').numKeys = 0;
  h.effect.property(expressionNames[0]).numKeys = 3;
  tagged(h.comp, 101, 'expression')[0].locked = true;
  const expressionBefore = channelSnapshot(h.effect, h.comp, 'expression');
  rebind(h, 102); h.add({...payload(102, 2, 3, 100), kind: 'motion'});
  equal(channelSnapshot(h.effect, h.comp, 'expression'), expressionBefore);
}

// Uniqueness checks only the selected binding namespace, not another channel's token.
{
  const h = host(); rebindExpression(h); h.comp.model(200);
  h.add(expressionPayload()); equal(tagged(h.comp, 200, 'expression').length, 1);
}
{
  const h = host(); rebindExpression(h);
  const other = new CompItem(); other.model(999).effects[0].property('Expression binding').setValue(200);
  h.app.project.items.push(other);
  const before = snapshot(h.comp);
  throws(() => h.add(expressionPayload()), /被复制/); equal(snapshot(h.comp), before);
}

// Duplicating the model copies only the imported channel's clips, leaving the other untouched.
{
  const h = host(); h.add(payload()); rebindExpression(h); h.add(expressionPayload());
  const originalFx = h.effect, originalExpression = tagged(h.comp, 200, 'expression')[0];
  h.model = h.model.duplicate(); h.effect = h.model.effects[0];
  const motionBefore = channelSnapshot(h.effect, h.comp, 'motion');
  rebindExpression(h, 201); h.comp.time = 1; h.add(expressionPayload(201, 2, 3, 200));
  equal(channelSnapshot(h.effect, h.comp, 'motion'), motionBefore);
  equal(tagged(h.comp, 200, 'expression').length, 1); equal(tagged(h.comp, 201, 'expression').length, 2);
  ok(originalExpression.marker.key(1).parameters.l2daeOwner === '200');
  clockResult(evaluateExpressions(originalFx, h.comp, .1), [1, 1, 50, 0]);
  const expressionBefore = channelSnapshot(h.effect, h.comp, 'expression');
  rebind(h, 101); h.add(payload(101, 2, 3, 100));
  equal(channelSnapshot(h.effect, h.comp, 'expression'), expressionBefore);
  equal(tagged(h.comp, 100).length, 1); equal(tagged(h.comp, 101).length, 2);
}

// A duplicated composition retags its expression clips without duplicating them again.
{
  const h = host(); h.add(payload()); rebindExpression(h); h.add(expressionPayload());
  const original = h.comp, second = original.clone(); h.app.project.items.push(second);
  h.comp = second; h.model = second._layers.find(l => l.effects.length); h.effect = h.model.effects[0];
  const motionBefore = channelSnapshot(h.effect, second, 'motion');
  rebindExpression(h, 201); h.add(expressionPayload(201, 2, 3, 200));
  equal(tagged(original, 200, 'expression').length, 1); equal(tagged(second, 200, 'expression').length, 0);
  equal(tagged(second, 201, 'expression').length, 2);
  equal(channelSnapshot(h.effect, second, 'motion'), motionBefore);
}

// Expression ownership survives native failed binding updates and Windows newline normalization.
for (const newline of ['\n', '\r\n', '\r']) {
  const h = host(); h.add(payload()); rebindExpression(h); h.add(expressionPayload());
  const motionBefore = channelSnapshot(h.effect, h.comp, 'motion');
  for (const n of expressionNames) h.effect.property(n).expression = h.effect.property(n).expression.replace(/\n/g, newline);
  rebindExpression(h, 201); h.comp.failAddNull = true;
  throws(() => h.add(expressionPayload(201, 2, 3, 200)), /Injected/);
  equal(tagged(h.comp, 200, 'expression').length, 1); equal(tagged(h.comp, 201, 'expression').length, 0);
  clockResult(evaluateExpressions(h.effect, h.comp, 1), [1, 1, 100, 0]);
  h.comp.failAddNull = false; rebindExpression(h, 202);
  h.add(expressionPayload(202, 2, 3, 201));
  equal(tagged(h.comp, 200, 'expression').length, 0); equal(tagged(h.comp, 202, 'expression').length, 2);
  equal(channelSnapshot(h.effect, h.comp, 'motion'), motionBefore);
}

// Expression transaction rollback restores only its own channel, footage and selection.
for (const inject of [
  h => { h.effect.property(expressionNames[3]).failExpressionOnce = true; },
  h => { h.effect.property(expressionNames[3]).injectExpressionError = true; },
  h => { tagged(h.comp, 200, 'expression')[0].marker.failOnce = true; }
]) {
  const h = host(); h.add(payload()); rebindExpression(h); h.add(expressionPayload());
  rebindExpression(h, 201); inject(h);
  const before = snapshot(h.comp), itemsBefore = h.app.project.numItems;
  throws(() => h.add(expressionPayload(201, 2, 3, 200)), /Injected/);
  equal(snapshot(h.comp), before); equal(h.app.project.numItems, itemsBefore); equal(h.app.depth, 0);
  equal(h.model.selected, true);
}
{
  const h = host(); h.add(payload()); rebindExpression(h);
  h.effect.property(expressionNames[2]).failExpressionOnce = true;
  const before = snapshot(h.comp), itemsBefore = h.app.project.numItems;
  throws(() => h.add(expressionPayload()), /Injected/);
  equal(snapshot(h.comp), before); equal(h.app.project.numItems, itemsBefore);
}

// Reject malformed kinds and conflicting expression controls before any script mutation.
for (const kind of [null, 1, '', 'Expression', 'face', {}]) {
  const h = host(), before = snapshot(h.comp);
  throws(() => h.add({...payload(), kind}), /片段类型无效/);
  equal(snapshot(h.comp), before); equal(h.app.undoGroups, 0);
}
for (const mutate of [
  h => { h.effect.property(expressionNames[0]).numKeys = 1; },
  h => { h.effect.property(expressionNames[1]).expression = 'time'; },
  h => { h.effect.property(expressionNames[2]).canSetExpression = false; },
  h => { h.effect.property('Expression binding').setValue(999); }
]) {
  const h = host(); h.add(payload()); rebindExpression(h); mutate(h);
  const before = snapshot(h.comp), undoBefore = h.app.undoGroups;
  throws(() => h.add(expressionPayload()), /AeGO Flash:/);
  equal(snapshot(h.comp), before); equal(h.app.undoGroups, undoBefore);
}
{
  const h = host(); rebindExpression(h); h.add(expressionPayload()); rebindExpression(h, 201);
  h.effect.property(expressionNames[2]).expression = '';
  const before = snapshot(h.comp);
  throws(() => h.add(expressionPayload(201, 2, 3, 200)), /已被移除/); equal(snapshot(h.comp), before);
}

// The omitted duration is exactly 30 composition frames, independent of frame rate.
for (const fps of [24, 25, 30, 60, 24000 / 1001]) {
  const h = host(); h.comp.frameDuration = 1 / fps;
  const first = payload(); delete first.transitionFrames;
  h.add(first);
  const a = tagged(h.comp, 100)[0];
  equal(a.name, '待机 → 微笑'); equal(a.marker.key(1).comment, a.name);
  near(Number(a.marker.key(1).parameters.l2daeFade), 30 / fps);
  rebind(h, 101);
  const next = payload(101, 19, 3, 100); delete next.transitionFrames;
  h.add(next);
  const b = tagged(h.comp, 101).find(l => l !== a);
  near(b.inPoint, 4 - 30 / fps);
  clockResult(evaluate(h.effect, h.comp, 4 - 15 / fps), [1, 19, 4 - 15 / fps, 15 / fps, 50]);
  rebindExpression(h); h.comp.time = 8;
  h.add({kind: 'expression', binding: 200, slot: 21, label: 'C:\\faces\\笑容.EXP3.JSON'});
  const face = tagged(h.comp, 200, 'expression')[0];
  equal(face.name, '笑容'); equal(face.marker.key(1).comment, '笑容');
  equal(face.marker.key(1).parameters.l2daeCurve, '0', 'omitted expression curve stays linear');
  near(Number(face.marker.key(1).parameters.l2daeFade), 30 / fps);
  clockResult(evaluateExpressions(h.effect, h.comp, 8 + 15 / fps), [21, 21, 50, 0]);
}

// Dynamic numeric IDs retain every earlier association beyond the former eight slots.
{
  const h = host(); h.comp.duration = 200;
  for (let index = 1; index <= 40; ++index) {
    rebind(h, 99 + index);
    h.add({...payload(99 + index, index, 4, index === 1 ? 0 : 98 + index), transitionFrames: 0});
  }
  const clips = tagged(h.comp, 139);
  equal(clips.length, 40);
  equal(new Set(clips.map(c => Number(c.marker.key(1).parameters.l2daeSlot))).size, 40);
  for (const id of [1, 8, 9, 17, 40]) clockResult(evaluate(h.effect, h.comp, (id - 1) * 4 + 1), [id, id, 1, 1, 0]);
  rebindExpression(h); h.add(expressionPayload(200, 16777216));
  clockResult(evaluateExpressions(h.effect, h.comp, 1), [16777216, 16777216, 100, 0]);
}

// Zero is an intentional instant cut for append and contained overlays, never NaN.
{
  const h = host(); h.add({...payload(100, 1, 8), transitionFrames: 0});
  h.comp.time = 2; rebind(h, 101);
  h.add({...payload(101, 9, 2, 100, false), transitionFrames: 0});
  clockResult(evaluate(h.effect, h.comp, 2), [1, 9, 2, 0, 100]);
  clockResult(evaluate(h.effect, h.comp, 4), [1, 1, 4, 4, 0]);
  rebind(h, 102); h.add({...payload(102, 20, 1, 101), transitionFrames: 0});
  near(tagged(h.comp, 102).find(l => l.marker.key(1).parameters.l2daeSlot === '20').inPoint, 8);
  rebindExpression(h); h.comp.time = 0;
  h.add({...expressionPayload(200, 10, 5), transitionFrames: 0});
  clockResult(evaluateExpressions(h.effect, h.comp, 0), [10, 10, 100, 0]);
  h.comp.time = 1; rebindExpression(h, 201);
  h.add({...expressionPayload(201, 11, 2, 200), transitionFrames: 0});
  clockResult(evaluateExpressions(h.effect, h.comp, 1), [10, 11, 0, 100]);
  clockResult(evaluateExpressions(h.effect, h.comp, 3), [10, 10, 100, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 5), [1, 1, 0, 0]);
}

// A too-short appended clip clamps its 15-frame overlap to half each clip.
{
  const h = host(); h.add({...payload(100, 1, .2), transitionFrames: 15});
  rebind(h, 101); h.add({...payload(101, 2, .1, 100), transitionFrames: 15});
  near(tagged(h.comp, 101).find(l => l.marker.key(1).parameters.l2daeSlot === '2').inPoint, .15);
  near(evaluate(h.effect, h.comp, .175)[4], 50);
}

// Combined imports have one undo unit and shared placement, even if the
// expression command arrives first and its previous group ends elsewhere.
for (const append of [false, true]) {
  const h = host(); h.add(payload()); rebindExpression(h);
  h.comp.time = 10; h.add(expressionPayload()); h.comp.time = 1;
  rebind(h, 101); rebindExpression(h, 201);
  const undoBefore = h.app.undoGroups;
  equal(h.batch([{...expressionPayload(201, 23, 3, 200), transitionFrames: 15},
    {...payload(101, 27, 4, 100, append), transitionFrames: 15}]), 'OK');
  equal(h.app.undoGroups, undoBefore + 1); equal(h.app.depth, 0);
  const motion = tagged(h.comp, 101).find(c => c.marker.key(1).parameters.l2daeSlot === '27');
  const face = tagged(h.comp, 201, 'expression').find(c => c.marker.key(1).parameters.l2daeSlot === '23');
  near(motion.inPoint, append ? 3.5 : 1); near(face.inPoint, motion.inPoint);
  equal(h.model.selected, true); equal(face.selected, false); equal(motion.selected, false);
}

// Failures in the second channel roll back the first channel and all retags,
// copied groups, created source items and selection; native bindings stay intact.
for (const duplicate of [false, true]) {
  const h = host(); h.add(payload()); rebindExpression(h); h.add(expressionPayload());
  const old = tagged(h.comp, 100)[0].marker.keyValue(1); old.parameters.custom = '用户参数';
  tagged(h.comp, 100)[0].marker.setValueAtKey(1, old);
  if (duplicate) { h.model = h.model.duplicate(); h.effect = h.model.effects[0]; }
  rebind(h, 101); rebindExpression(h, 201);
  h.effect.property(expressionNames[3]).failExpressionOnce = true;
  const before = snapshot(h.comp), itemsBefore = h.app.project.numItems, undoBefore = h.app.undoGroups;
  throws(() => h.batch([payload(101, 15, 4, 100), expressionPayload(201, 20, 3, 200)]), /Injected/);
  equal(snapshot(h.comp), before); equal(h.app.project.numItems, itemsBefore);
  equal(h.app.depth, 0); equal(h.app.undoGroups, undoBefore + 1);
}

// Preflight on either channel protects user edits before starting an undo group.
for (const legacyName of ['Motion A slot', 'Expression B slot']) {
  for (const custom of [false, true]) {
    const h = host(); rebindExpression(h);
    if (custom) h.effect.property(legacyName).expression = 'time + 1';
    else h.effect.property(legacyName).numKeys = 1;
    const before = snapshot(h.comp);
    throws(() => h.batch([payload(), expressionPayload()]), /现有关键帧或自定义表达式/);
    equal(snapshot(h.comp), before); equal(h.app.undoGroups, 0);
  }
}
for (const invalid of [[], [payload(), payload()], [expressionPayload(), expressionPayload()],
  [{...payload(), transitionFrames: -1}], [{...payload(), transitionFrames: NaN}],
  [{...payload(), transitionFrames: Infinity}], [{...payload(), transitionFrames: .5}],
  [{...payload(), transitionFrames: 100001}]]) {
  const h = host(); rebindExpression(h); const before = snapshot(h.comp);
  throws(() => h.batch(invalid), /AeGO Flash:/); equal(snapshot(h.comp), before); equal(h.app.undoGroups, 0);
}
{
  const h = host(), other = h.comp.model(999); other.effects[0].property('Expression binding').setValue(200);
  const before = snapshot(h.comp);
  throws(() => h.batch([payload(), expressionPayload()]), /同一个模型效果/);
  equal(snapshot(h.comp), before); equal(h.app.undoGroups, 0);
}

// Existing V1 popup expressions and opaque comments remain untouched on script
// load, then migrate together at import; rollback restores the original format.
for (const fail of [false, true]) {
  const h = host();
  const oldClip = h.comp.layers.addNull(4); oldClip.name = 'L2D · 旧动作.motion3.json';
  const marker = new MarkerValue('L2DCLIP|90|8|4'); marker.setParameters({custom: 'retain'});
  oldClip.marker.setValueAtTime(0, marker);
  const oldNames = ['Motion A slot', 'Motion B slot', ...controlNames.slice(2)];
  oldNames.forEach((name, i) => { h.effect.property(name).expression = '// L2DAE_TIMELINE_V1 owner=90\n' + (i < 2 ? '8' : '0'); });
  vm.runInContext(source, h.context);
  equal(oldClip.marker.key(1).comment, 'L2DCLIP|90|8|4');
  ok(h.effect.property('Motion A slot').expression.startsWith('// L2DAE_TIMELINE_V1'));
  if (fail) h.effect.property(controlNames[3]).failExpressionOnce = true;
  const before = snapshot(h.comp);
  if (fail) {
    throws(() => h.add(payload(100, 9, 4, 90)), /Injected/); equal(snapshot(h.comp), before);
  } else {
    h.add(payload(100, 9, 4, 90));
    equal(h.effect.property('Motion A slot').expression, '');
    ok(h.effect.property('Motion A index').expression.startsWith('// L2DAE_TIMELINE_V2'));
    equal(oldClip.name, '旧动作'); equal(oldClip.marker.key(1).comment, '旧动作');
    equal(oldClip.marker.key(1).parameters.custom, 'retain');
    equal(oldClip.marker.key(1).parameters.l2daeFade, '0.2');
    clockResult(evaluate(h.effect, h.comp, 1), [8, 8, 1, 1, 0]);
  }
}

// AE's default Timeline column shows Source Name, independently of Layer Name.
// Both names and the marker comment must identify the asset on a fresh import.
{
  const h = host(); rebindExpression(h);
  h.batch([{...payload(), label: '动作库/转头.motion3.json'}, {...expressionPayload(), label: '脸红.exp3.json'}]);
  const motion = tagged(h.comp, 100)[0], face = tagged(h.comp, 200, 'expression')[0];
  equal([motion.name, motion.source.name, motion.marker.key(1).comment], ['转头', '转头', '转头']);
  equal([face.name, face.source.name, face.marker.key(1).comment], ['脸红', '脸红', '脸红']);
  ok(motion.source.id !== face.source.id, 'each newly created null retains its own named footage');
}

// Safe migration covers legacy opaque markers and current readable markers
// whose null footage still has its host-generated Source Name.
for (const legacy of [false, true]) {
  const h = host(); h.add({...payload(), label: '呼吸.motion3.json'});
  const old = tagged(h.comp, 100)[0]; old.source.name = legacy ? 'Null 72' : '空42';
  if (legacy) {
    old.name = 'L2D · 呼吸.motion3.json';
    const m = old.marker.keyValue(1); m.comment = 'L2DCLIP|100|1|4'; m.parameters = {custom: 'keep'};
    old.marker.setValueAtKey(1, m);
  }
  rebind(h, 101); h.add({...payload(101, 2, 3, 100), label: '挥手.motion3.json'});
  equal([old.name, old.source.name, old.marker.key(1).comment], ['呼吸', '呼吸', '呼吸']);
  if (legacy) equal(old.marker.key(1).parameters.custom, 'keep');
  equal(tagged(h.comp, 101).find(c => c !== old).source.name, '挥手');
}

// A source intentionally renamed by the user remains unchanged. Layer/marker
// names still retain independent editing and private ownership continues working.
{
  const h = host(); h.add(payload()); const old = tagged(h.comp, 100)[0];
  old.source.name = '项目里我命名的素材'; old.name = '自定义轨道名';
  rebind(h, 101); h.add(payload(101, 2, 3, 100));
  equal(old.source.name, '项目里我命名的素材'); equal(old.name, '自定义轨道名');
  equal(old.marker.key(1).comment, '待机 → 微笑');
}

// Inspect all source usages, including additional layers in the same comp and
// another comp; usedIn alone cannot distinguish these shared-source cases.
for (const elsewhere of [false, true]) {
  const h = host(); h.add(payload()); const old = tagged(h.comp, 100)[0]; old.source.name = '空 88';
  const otherComp = elsewhere ? new CompItem() : h.comp;
  if (elsewhere) { otherComp.project = h.app.project; h.app.project.items.push(otherComp); }
  const unrelated = new Layer(otherComp, 4); unrelated.source = old.source; unrelated.name = '不要修改';
  otherComp._layers.push(unrelated);
  rebind(h, 101); h.add(payload(101, 2, 3, 100));
  equal(old.source.name, '空 88'); equal(unrelated.source.name, '空 88'); equal(unrelated.name, '不要修改');
}

// Duplicated clips in the same owned group can safely share their asset name.
// Conflicting marker labels prevent a source rename that would mislabel one.
for (const conflicting of [false, true]) {
  const h = host(); h.add({...payload(), label: '待机.motion3.json'});
  const old = tagged(h.comp, 100)[0]; old.source.name = 'Null 53';
  const duplicate = old.duplicate(); duplicate.startTime = 8;
  if (conflicting) {
    const marker = duplicate.marker.keyValue(1); marker.comment = '用户另一标记名'; duplicate.marker.setValueAtKey(1, marker);
  }
  rebind(h, 101); h.add(payload(101, 2, 3, 100));
  equal(old.source.name, conflicting ? 'Null 53' : '待机'); equal(duplicate.source.name, old.source.name);
}

// Importing into a duplicated model/comp never renames the original group's
// shared footage. Only the entirely new imported null receives a new source name.
for (const duplicateComp of [false, true]) {
  const h = host(); h.add(payload()); const originalClip = tagged(h.comp, 100)[0]; originalClip.source.name = '空 12';
  if (duplicateComp) {
    h.comp = h.comp.clone(); h.app.project.items.push(h.comp); h.model = h.comp._layers.find(l => l.effects.length);
  } else h.model = h.model.duplicate();
  h.effect = h.model.effects[0]; rebind(h, 101);
  h.add({...payload(101, 2, 3, 100), label: '新动作.motion3.json'});
  equal(originalClip.source.name, '空 12');
  equal(tagged(h.comp, 101).find(l => l.marker.key(1).parameters.l2daeSlot === '2').source.name, '新动作');
}

// An AE version that reuses existing null footage cannot make us rename a
// user's project item; this remains true even if it has no other layer usages.
{
  const h = host(), existing = new FootageItem(h.app.project); existing.name = 'Null 77';
  h.app.project.items.push(existing); h.comp.reuseSource = existing;
  h.add({...payload(), label: '动作.motion3.json'});
  const added = tagged(h.comp, 100)[0]; equal(added.source, existing); equal(existing.name, 'Null 77');
  equal(added.name, '动作'); equal(added.marker.key(1).comment, '动作');
}

// Metadata moved onto ordinary footage by the user is not permission to rename
// that footage, even when its name happens to look like an automatic null name.
{
  const h = host(); h.add(payload()); const old = tagged(h.comp, 100)[0];
  old.nullLayer = false; old.source.name = 'Null 62';
  rebind(h, 101); h.add(payload(101, 2, 3, 100)); equal(old.source.name, 'Null 62');
}

// A source-name failure late in a paired import restores already-renamed old
// sources, markers, expressions, selection, and removes both new null sources.
{
  const h = host(); h.add(payload()); rebindExpression(h); h.add(expressionPayload());
  const oldMotion = tagged(h.comp, 100)[0], oldFace = tagged(h.comp, 200, 'expression')[0];
  oldMotion.source.name = '空 15'; oldFace.source.name = '空 16'; oldFace.source.failRenameOnce = true;
  rebind(h, 101); rebindExpression(h, 201);
  const before = snapshot(h.comp), itemsBefore = h.app.project.numItems;
  throws(() => h.batch([payload(101, 2, 3, 100), expressionPayload(201, 2, 3, 200)]), /source rename failure/);
  equal(snapshot(h.comp), before); equal(h.app.project.numItems, itemsBefore); equal(h.app.depth, 0);
  equal(oldMotion.source.name, '空 15'); equal(oldFace.source.name, '空 16');
}

function localizeEffect(effect, prefix = '中文参数') {
  effect.name = '用户自定义的角色效果';
  for (const [name, p] of Object.entries(effect.props)) p.name = `${prefix}${parameterDiskIds[name]}`;
}

// Both channels must import using the native, persistent match names. Neither a
// Chinese UI nor any future display-name translation is part of the data schema.
for (const prefix of ['中文参数', 'Autre libellé ']) {
  const h = host(); localizeEffect(h.effect, prefix);
  for (const name of Object.keys(parameterDiskIds)) equal(h.effect.property(name), null);
  h.effect.lookups = [];
  rebindExpression(h);
  equal(h.batch([{...payload(), transitionFrames: 15},
    {...expressionPayload(200, 1, 4), transitionFrames: 15}]), 'OK');
  clockResult(evaluate(h.effect, h.comp, 2), [1, 1, 2, 2, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 2), [1, 1, 100, 0]);
  equal(parameter(h.effect, 'Keyframe motion time').value, 1);
  equal(parameter(h.effect, 'Loop motion').value, 0);
  ok(h.effect.lookups.length > 0 && h.effect.lookups.every(name => /^L2DAE Native Renderer-\d{4}$/.test(name)),
    'all effect-property access uses persistent IDs, including hidden bindings and legacy controls');
}

// A V2 project created with English names keeps all existing expression text and
// source-clock values after localization; subsequent imports extend both groups.
// The duplicate-owner check must use match names on the old, localized effect too.
{
  const h = host(); h.add(payload()); rebindExpression(h); h.add(expressionPayload(200, 1, 4));
  const names = [...controlNames, ...expressionNames];
  const savedExpressions = names.map(name => parameter(h.effect, name).expression);
  const beforeMotion = evaluate(h.effect, h.comp, 2), beforeFace = evaluateExpressions(h.effect, h.comp, 2);
  localizeEffect(h.effect);
  equal(names.map(name => parameter(h.effect, name).expression), savedExpressions);
  equal(evaluate(h.effect, h.comp, 2), beforeMotion);
  equal(evaluateExpressions(h.effect, h.comp, 2), beforeFace);
  rebind(h, 101); rebindExpression(h, 201);
  equal(h.batch([payload(101, 9, 3, 100), expressionPayload(201, 17, 3, 200)]), 'OK');
  clockResult(evaluate(h.effect, h.comp, 3.9), [1, 9, 3.9, .1, 50]);
  clockResult(evaluateExpressions(h.effect, h.comp, 3.9), [1, 17, 50, 50]);
  const originalFx = h.effect;
  const originalExpressions = names.map(name => parameter(originalFx, name).expression);
  h.model = h.model.duplicate(); h.effect = h.model.effects[0];
  rebind(h, 102); rebindExpression(h, 202);
  equal(h.batch([payload(102, 10, 2, 101), expressionPayload(202, 18, 2, 201)]), 'OK');
  equal(names.map(name => parameter(originalFx, name).expression), originalExpressions);
  equal(tagged(h.comp, 101).length, 2); equal(tagged(h.comp, 201, 'expression').length, 2);
  equal(tagged(h.comp, 102).length, 3); equal(tagged(h.comp, 202, 'expression').length, 3);
  clockResult(evaluate(originalFx, h.comp, 3.9), [1, 9, 3.9, .1, 50]);
  clockResult(evaluateExpressions(originalFx, h.comp, 3.9), [1, 17, 50, 50]);
}

// Legacy V1 projects store expressions on popup slots. Localizing their hidden
// names must still migrate both channels and keep the existing clip association.
{
  const h = host(); rebindExpression(h);
  const oldMotion = h.comp.layers.addNull(4), oldFace = h.comp.layers.addNull(4);
  oldMotion.name = 'L2D · 旧动作'; oldFace.name = 'L2D 表情 · 旧表情';
  oldMotion.marker.setValueAtTime(0, new MarkerValue('L2DCLIP|90|8|4'));
  oldFace.marker.setValueAtTime(0, new MarkerValue('L2DEXPR|190|7|4'));
  const legacyMotion = ['Motion A slot', 'Motion B slot', ...controlNames.slice(2)];
  const legacyFace = ['Expression A slot', 'Expression B slot', ...expressionNames.slice(2)];
  legacyMotion.forEach((name, i) => { parameter(h.effect, name).expression = '// L2DAE_TIMELINE_V1 owner=90\n' + (i < 2 ? '8' : '0'); });
  legacyFace.forEach((name, i) => { parameter(h.effect, name).expression = '// L2DAE_EXPRESSION_V1 owner=190\n' + (i < 2 ? '7' : '0'); });
  localizeEffect(h.effect);
  equal(h.batch([payload(100, 9, 4, 90), expressionPayload(200, 8, 4, 190)]), 'OK');
  for (const name of ['Motion A slot', 'Motion B slot', 'Expression A slot', 'Expression B slot'])
    equal(parameter(h.effect, name).expression, '');
  clockResult(evaluate(h.effect, h.comp, 1), [8, 8, 1, 1, 0]);
  clockResult(evaluateExpressions(h.effect, h.comp, 1), [7, 7, 100, 0]);
  equal(oldMotion.marker.key(1).parameters.l2daeOwner, '100');
  equal(oldFace.marker.key(1).parameters.l2daeOwner, '200');
  equal(oldMotion.marker.key(1).parameters.l2daeSlot, '8');
  equal(oldFace.marker.key(1).parameters.l2daeSlot, '7');
}

// Rollback on a later expression failure must restore localized V2 controls and
// all earlier clips; resolving by display names would fail before this point.
{
  const h = host(); h.add(payload()); rebindExpression(h); h.add(expressionPayload());
  localizeEffect(h.effect); rebind(h, 101); rebindExpression(h, 201);
  parameter(h.effect, expressionNames[3]).failExpressionOnce = true;
  const before = snapshot(h.comp), itemsBefore = h.app.project.numItems;
  throws(() => h.batch([payload(101, 2, 3, 100), expressionPayload(201, 2, 3, 200)]), /Injected/);
  equal(snapshot(h.comp), before); equal(h.app.project.numItems, itemsBefore); equal(h.app.depth, 0);
}

// A matching English display name cannot substitute for a missing native stream.
// This rejects a wrong schema without writing to a superficially similar control.
for (const name of ['Motion A index', 'Timeline binding']) {
  const h = host(); localizeEffect(h.effect);
  const wrong = parameter(h.effect, name); wrong.matchName = 'L2DAE Native Renderer-0999'; wrong.name = name;
  const before = snapshot(h.comp);
  throws(() => h.add(payload()), name === 'Timeline binding' ? /找不到目标模型效果/ : /缺少必要的 AeGO Flash 效果参数/);
  equal(snapshot(h.comp), before); equal(wrong.expression, ''); equal(h.app.depth, 0);
}

// Internal curve IDs preserve saved clip behavior. Invalid native payloads fail
// before any project mutation; omitted new-motion imports now use overshoot.
for (const transitionCurve of [null, false, true, '0', '1', '2', '3', '4', '5', NaN, Infinity, -1, 6, .5, 3.5, {}, []]) {
  const h = host(), before = snapshot(h.comp);
  throws(() => h.add({...payload(), transitionCurve}), /片段设置无效/);
  equal(snapshot(h.comp), before); equal(h.app.undoGroups, 0);
}
{
  const h = host(); h.add({...payload(), transitionCurve: 1});
  const a = tagged(h.comp, 100)[0];
  equal(a.marker.key(1).parameters.l2daeCurve, '1');
  rebind(h, 101); h.add({...payload(101, 2, 3, 100), transitionCurve: 1});
  const b = tagged(h.comp, 101).find(l => l !== a);
  equal(b.marker.key(1).parameters.l2daeCurve, '1');
  equal(a.marker.key(1).parameters.l2daeCurve, '1', 'retagging preserves the earlier choice');
  const start = b.inPoint, span = a.outPoint - start;
  // Analytic landmarks: reach B, rebound toward A, then settle with a much
  // smaller rebound. This distinguishes Q-elastic from a renamed linear ease.
  const landmarks = [[0, 0], [.125, 66.50390625], [.25, 100], [.5, 87.5],
    [.75, 100], [.875, 99.90234375]];
  for (const [u, expected] of landmarks) {
    const t = start + span * u, r = evaluate(h.effect, h.comp, t);
    clockResult(r, [1, 2, t, t - b.startTime, expected]);
  }
  const script = new vm.Script(parameter(h.effect, controlNames[4]).expression);
  const context = vm.createContext({thisComp: h.comp, time: 0});
  let peak = 0, largestDrop = 0, previous = 0;
  for (let i = 0; i < 400; ++i) {
    context.time = start + span * i / 400;
    const weight = script.runInContext(context);
    ok(Number.isFinite(weight) && weight >= 0 && weight <= 100, 'Q-elastic remains inside the two source poses');
    peak = Math.max(peak, weight); largestDrop = Math.max(largestDrop, previous - weight); previous = weight;
  }
  near(peak, 100); ok(largestDrop > 0, 'rebound must genuinely move back before settling');
  const pose = t => {
    const r = evaluate(h.effect, h.comp, t), weight = r[4] / 100;
    return (100 * r[0] + r[2]) * (1 - weight) + (100 * r[1] + r[3]) * weight;
  };
  ok(Math.abs(pose(start - 1e-9) - pose(start + 1e-9)) < 1e-4, 'incoming endpoint is continuous');
  ok(Math.abs(pose(a.outPoint - 1e-9) - pose(a.outPoint + 1e-9)) < 1e-4, 'B/B handoff is continuous');
  clockResult(evaluate(h.effect, h.comp, a.outPoint), [2, 2, span, span, 0]);
}

// The incoming clip selects the curve. Existing clips without the new marker
// field remain exactly linear even after their group is rewritten by an import.
{
  const h = host(); h.add({...payload(), transitionCurve: 1});
  rebind(h, 101); h.add({...payload(101, 2, 4, 100), transitionCurve: 0});
  const clips = tagged(h.comp, 101).sort((a, b) => a.inPoint - b.inPoint);
  const start = clips[1].inPoint, span = clips[0].outPoint - start;
  const before = [0, .125, .25, .5, .75, .875].map(u => evaluate(h.effect, h.comp, start + span * u));
  before.forEach((r, i) => near(r[4], [0, 12.5, 25, 50, 75, 87.5][i]));
  for (const clip of clips) {
    const marker = clip.marker.key(1); delete marker.parameters.l2daeCurve; clip.marker.setValueAtKey(1, marker);
  }
  rebind(h, 102); h.add({...payload(102, 3, 4, 101), transitionCurve: 1});
  equal(clips.map(l => l.marker.key(1).parameters.l2daeCurve), ['0', '0']);
  [0, .125, .25, .5, .75, .875].forEach((u, i) =>
    equal(evaluate(h.effect, h.comp, start + span * u), before[i], 'legacy overlap stays bit-for-bit unchanged'));
  equal(tagged(h.comp, 102).find(l => Number(l.marker.key(1).parameters.l2daeSlot) === 3)
    .marker.key(1).parameters.l2daeCurve, '1');
}

// V1 comment-only clips migrate to curve 0; imports and same-comp model clones
// preserve previously selected curves instead of imposing the newest selection.
{
  const h = host(); h.add(payload()); const old = tagged(h.comp, 100)[0];
  const value = old.marker.key(1); value.comment = 'L2DCLIP|100|1|4'; value.parameters = {};
  old.marker.setValueAtKey(1, value);
  rebind(h, 101); h.add({...payload(101, 2, 3, 100), transitionCurve: 1});
  equal(old.marker.key(1).parameters.l2daeCurve, '0');
  const originalEffect = h.effect, originalClips = tagged(h.comp, 101);
  const duplicated = h.model.duplicate(); h.model = duplicated; h.effect = duplicated.effects[0];
  rebind(h, 102); h.add({...payload(102, 3, 3, 101), transitionCurve: 0});
  equal(originalClips.map(l => l.marker.key(1).parameters.l2daeCurve).sort(), ['0', '1']);
  equal(tagged(h.comp, 102).map(l => l.marker.key(1).parameters.l2daeCurve).sort(), ['0', '0', '1']);
  near(evaluate(originalEffect, h.comp, 3.9)[4], 87.5);
}

// A failed re-import restores the complete marker, including an absent old
// curve field, and leaves previously Q-elastic clips and expressions untouched.
{
  const h = host(); h.add(payload());
  const old = tagged(h.comp, 100)[0], marker = old.marker.key(1);
  delete marker.parameters.l2daeCurve; old.marker.setValueAtKey(1, marker);
  rebind(h, 101); const before = snapshot(h.comp);
  parameter(h.effect, controlNames[3]).failExpressionOnce = true;
  throws(() => h.add({...payload(101, 2, 3, 100), transitionCurve: 1}), /Injected/);
  equal(snapshot(h.comp), before);
  equal(old.marker.key(1).parameters.l2daeCurve, undefined);
}

// A nested Q-elastic insert uses one shared envelope; its exit is the time
// reversal of entry, and both boundaries converge to the surrounding motion.
{
  const h = host(); h.add(payload(100, 1, 10));
  h.comp.time = 3; rebind(h, 101);
  h.add({...payload(101, 2, 3, 100, false), transitionCurve: 1});
  for (const [delta, weight] of [[0, 0], [.025, 66.50390625], [.05, 100], [.1, 87.5], [.15, 100]]) {
    near(evaluate(h.effect, h.comp, 3 + delta)[4], weight);
    if (delta > 0) near(evaluate(h.effect, h.comp, 6 - delta)[4], weight);
  }
  const pose = t => {
    const r = evaluate(h.effect, h.comp, t), weight = r[4] / 100;
    return (100 * r[0] + r[2]) * (1 - weight) + (100 * r[1] + r[3]) * weight;
  };
  ok(Math.abs(pose(3 - 1e-9) - pose(3 + 1e-9)) < 1e-4);
  ok(Math.abs(pose(6 - 1e-9) - pose(6 + 1e-9)) < 1e-4);
  clockResult(evaluate(h.effect, h.comp, 6), [1, 1, 6, 6, 0]);
}

// Native time stretch, negative stretch and trimmed in/out points affect the
// source clocks independently. Q-elastic follows the chronological overlap.
{
  const h = host(); h.add(payload()); const a = tagged(h.comp, 100)[0];
  a.stretch = 200; a.outPoint = 8;
  rebind(h, 101); h.add({...payload(101, 2, 4, 100), transitionFrames: 60, transitionCurve: 1});
  const b = tagged(h.comp, 101).find(l => l !== a);
  clockResult(evaluate(h.effect, h.comp, 6.5), [1, 2, 3.25, .5, 100]);
  b.stretch = 200; b.outPoint = 14;
  clockResult(evaluate(h.effect, h.comp, 6.5), [1, 2, 3.25, .25, 100]);
  a.stretch = -100; a.startTime = 8; a.inPoint = 8; a.outPoint = 0;
  b.stretch = -200; b.startTime = 14; b.inPoint = 14; b.outPoint = 6;
  clockResult(evaluate(h.effect, h.comp, 6.5), [1, 2, 1.5, 3.75, 100]);
  b.outPoint = 6.25;
  clockResult(evaluate(h.effect, h.comp, 7.125), [1, 2, .875, 3.4375, 87.5]);
  b.name = '倒放并裁切的动作'; h.comp._layers.reverse();
  clockResult(evaluate(h.effect, h.comp, 7.125), [1, 2, .875, 3.4375, 87.5]);
}

// The 15-frame setting scales with the composition frame rate, while curve
// landmarks depend only on overlap progress, not on a hard-coded 30 fps clock.
for (const fps of [23.976, 24, 25, 30, 60]) {
  const h = host(); h.comp.frameDuration = 1 / fps;
  h.add({...payload(), transitionFrames: 15, transitionCurve: 1});
  const a = tagged(h.comp, 100)[0];
  rebind(h, 101); h.add({...payload(101, 2, 4, 100), transitionFrames: 15, transitionCurve: 1});
  const b = tagged(h.comp, 101).find(l => l !== a), span = a.outPoint - b.inPoint;
  near(span * fps, 15);
  near(evaluate(h.effect, h.comp, b.inPoint + span * .25)[4], 100);
  near(evaluate(h.effect, h.comp, b.inPoint + span * .5)[4], 87.5);
  near(evaluate(h.effect, h.comp, b.inPoint + span * .875)[4], 99.90234375);
}

// A zero-frame cut remains a cut; expression fades retain their established
// behavior even when a combined import carries the same curve field.
{
  const h = host(); h.add({...payload(), transitionCurve: 1});
  rebind(h, 101); h.add({...payload(101, 2, 3, 100), transitionFrames: 0, transitionCurve: 1});
  clockResult(evaluate(h.effect, h.comp, 3.999), [1, 1, 3.999, 3.999, 0]);
  clockResult(evaluate(h.effect, h.comp, 4), [2, 2, 0, 0, 0]);
  const h0 = host(), h1 = host();
  for (const [hostInstance, transitionCurve] of [[h0, 0], [h1, 1]]) {
    rebindExpression(hostInstance);
    hostInstance.add({...expressionPayload(200, 1, 4), transitionCurve});
    hostInstance.comp.time = 3.8; rebindExpression(hostInstance, 201);
    hostInstance.add({...expressionPayload(201, 2, 4, 200), transitionCurve});
  }
  for (const t of [0, .05, .1, 3.8, 3.85, 3.9, 3.95, 4, 7.75])
    equal(evaluateExpressions(h1.effect, h1.comp, t), evaluateExpressions(h0.effect, h0.comp, t));
}

// Preserve the 1.5 curve-2 overshoot behavior explicitly. Check the actual
// generated expression against independent polynomial landmarks and shape
// constraints, rather than reproducing its implementation as a test oracle.
{
  const h = host(); h.add(legacyOvershootPayload());
  const a = tagged(h.comp, 100)[0];
  equal(a.marker.key(1).parameters.l2daeCurve, '2');
  rebind(h, 101); h.add({...payload(101, 2, 3, 100), transitionCurve: 2});
  const b = tagged(h.comp, 101).find(l => l !== a);
  equal(b.marker.key(1).parameters.l2daeCurve, '2');
  equal(a.marker.key(1).parameters.l2daeCurve, '2');
  const start = b.inPoint, span = a.outPoint - start;
  const landmarks = [[0, 0], [.25, 22.216796875], [.5, 73.4375],
    [.75, 104.150390625], [.8, 104.96], [.9, 102.6675], [.95, 100.882578125]];
  for (const [u, expected] of landmarks) {
    const t = start + span * u;
    clockResult(evaluate(h.effect, h.comp, t), [1, 2, t, t - b.startTime, expected]);
  }
  const compiled = new vm.Script(parameter(h.effect, controlNames[4]).expression);
  const context = vm.createContext({thisComp: h.comp, time: 0});
  const weightAt = u => {
    // At exactly the end the routing becomes B/B with a zero blend; its
    // equivalent A/B weight is 1, which is used for the endpoint limit only.
    if (u === 1) return 1;
    context.time = start + span * u;
    return compiled.runInContext(context) / 100;
  };
  let previous = 0, peak = 0, peakProgress = 0;
  const samples = [];
  for (let i = 0; i <= 400; ++i) {
    const u = i / 400, weight = weightAt(u);
    ok(Number.isFinite(weight) && weight >= 0 && weight <= 1.0496 + 1e-12,
      'new curve has a bounded positive overshoot, not a clamped weight');
    if (i > 0) ok(i <= 320 ? weight > previous : weight < previous,
      'one rise to the 80% peak and one smooth return; no secondary ripples');
    if (weight > peak) { peak = weight; peakProgress = u; }
    previous = weight; samples.push(weight);
  }
  near(peak, 1.0496); near(peakProgress, .8);
  near(10 * (1 - peak) + 20 * peak, 20.496, 'blend must truly pass beyond B');
  ok(weightAt(.01) < .001, 'start eases in instead of jumping rapidly to B');
  const epsilon = 1e-6;
  ok(Math.abs((weightAt(epsilon) - weightAt(0)) / epsilon) < 5e-5, 'start derivative tends to zero');
  ok(Math.abs((weightAt(1) - weightAt(1 - epsilon)) / epsilon) < 5e-5, 'end derivative tends to zero');
  ok(Math.abs((weightAt(.8 + epsilon) - weightAt(.8 - epsilon)) / (2 * epsilon)) < 1e-6,
    'overshoot peak turns without a corner');
  for (const i of [399, 320, 20, 380, 100, 0, 200, 320])
    equal(weightAt(i / 400), samples[i], 'out-of-order visits do not carry spring history');
  const pose = t => {
    const r = evaluate(h.effect, h.comp, t), weight = r[4] / 100;
    return (100 * r[0] + r[2]) * (1 - weight) + (100 * r[1] + r[3]) * weight;
  };
  for (const boundary of [start, a.outPoint]) {
    const delta = 1e-6, center = pose(boundary);
    ok(Math.abs(pose(boundary - delta) - pose(boundary + delta)) < 1e-4,
      'pose is continuous when routing enters or leaves the overlap');
    ok(Math.abs((center - pose(boundary - delta)) / delta -
      (pose(boundary + delta) - center) / delta) < .03,
      'pose velocity remains continuous at the routing boundary');
  }
  clockResult(evaluate(h.effect, h.comp, a.outPoint), [2, 2, span, span, 0]);
}

// Legacy curve-2 imports do not rewrite linear or bounded-rebound curves in an
// existing project. Later model copies retain all three types independently.
{
  const h = host(); h.add(payload());
  rebind(h, 101); h.add({...payload(101, 2, 3, 100), transitionCurve: 1});
  const oldClips = tagged(h.comp, 101).sort((a, b) => a.inPoint - b.inPoint);
  const oldStart = oldClips[1].inPoint, oldSpan = oldClips[0].outPoint - oldStart;
  const times = [0, .125, .5, .875].map(u => oldStart + oldSpan * u);
  const oldValues = times.map(t => evaluate(h.effect, h.comp, t));
  rebind(h, 102); h.add(legacyOvershootPayload(102, 3, 3, 101));
  equal(tagged(h.comp, 102).map(l => l.marker.key(1).parameters.l2daeCurve).sort(), ['0', '1', '2']);
  times.forEach((t, i) => equal(evaluate(h.effect, h.comp, t), oldValues[i]));
  const originalEffect = h.effect, originalClips = tagged(h.comp, 102);
  const duplicated = h.model.duplicate(); h.model = duplicated; h.effect = duplicated.effects[0];
  rebind(h, 103); h.add(legacyOvershootPayload(103, 4, 3, 102));
  equal(originalClips.map(l => l.marker.key(1).parameters.l2daeCurve).sort(), ['0', '1', '2']);
  equal(tagged(h.comp, 103).map(l => l.marker.key(1).parameters.l2daeCurve).sort(), ['0', '1', '2', '2']);
  times.forEach((t, i) => equal(evaluate(originalEffect, h.comp, t), oldValues[i]));
}

// A failed legacy curve-2 import restores old markers, including absent curve
// metadata; already saved overshoot clips are preserved during later failures.
for (const oldCurve of [undefined, '2']) {
  const h = host(); h.add(legacyOvershootPayload());
  const old = tagged(h.comp, 100)[0], marker = old.marker.key(1);
  if (oldCurve === undefined) delete marker.parameters.l2daeCurve;
  old.marker.setValueAtKey(1, marker);
  rebind(h, 101); const before = snapshot(h.comp);
  parameter(h.effect, controlNames[3]).failExpressionOnce = true;
  throws(() => h.add(legacyOvershootPayload(101, 2, 3, 100)), /Injected/);
  equal(snapshot(h.comp), before); equal(old.marker.key(1).parameters.l2daeCurve, oldCurve);
}

// Nested overshoot reaches past the short insert before settling, then returns
// using the reverse envelope. Neither boundary can leave a residual spring pose.
{
  const h = host(); h.add(legacyOvershootPayload(100, 1, 10));
  h.comp.time = 3; rebind(h, 101); h.add(legacyOvershootPayload(101, 2, 3, 100, false));
  for (const [delta, weight] of [[0, 0], [.05, 22.216796875], [.1, 73.4375], [.16, 104.96], [.18, 102.6675]]) {
    near(evaluate(h.effect, h.comp, 3 + delta)[4], weight);
    if (delta > 0) near(evaluate(h.effect, h.comp, 6 - delta)[4], weight);
  }
  near(evaluate(h.effect, h.comp, 4)[4], 100);
  clockResult(evaluate(h.effect, h.comp, 6), [1, 1, 6, 6, 0]);
}

// Stretch changes source clocks, while dragging/cropping changes chronological
// overlap. Overshoot must survive all of them, including reversed playback.
{
  const h = host(); h.add(legacyOvershootPayload()); const a = tagged(h.comp, 100)[0];
  a.stretch = 200; a.outPoint = 8;
  rebind(h, 101); h.add({...legacyOvershootPayload(101, 2, 4, 100), transitionFrames: 60});
  const b = tagged(h.comp, 101).find(l => l !== a);
  clockResult(evaluate(h.effect, h.comp, 7.6), [1, 2, 3.8, 1.6, 104.96]);
  b.stretch = 200; b.outPoint = 14;
  clockResult(evaluate(h.effect, h.comp, 7.6), [1, 2, 3.8, .8, 104.96]);
  a.stretch = -100; a.startTime = 8; a.inPoint = 8; a.outPoint = 0;
  b.stretch = -200; b.startTime = 14; b.inPoint = 14; b.outPoint = 6;
  clockResult(evaluate(h.effect, h.comp, 7.6), [1, 2, .4, 3.2, 104.96]);
  b.outPoint = 6.25;
  clockResult(evaluate(h.effect, h.comp, 7.65), [1, 2, .35, 3.175, 104.96]);
  b.name = '轻微超出后回落'; h.comp._layers.reverse();
  clockResult(evaluate(h.effect, h.comp, 7.65), [1, 2, .35, 3.175, 104.96]);
  const duplicated = b.duplicate(); b.enabled = false;
  equal(duplicated.marker.key(1).parameters.l2daeCurve, '2');
  clockResult(evaluate(h.effect, h.comp, 7.65), [1, 2, .35, 3.175, 104.96]);
  duplicated.startTime += .25;
  // Cropped reversed clip now begins at 6.5; the peak is 80% into 1.5 seconds.
  clockResult(evaluate(h.effect, h.comp, 7.7), [1, 2, .3, 3.275, 104.96]);
}

// Explicit fifteen-frame legacy curve-2 imports preserve their original
// overshoot timing and metadata at all frame rates.
for (const fps of [23.976, 24, 25, 30, 60]) {
  const h = host(); h.comp.frameDuration = 1 / fps;
  h.add(legacyOvershootPayload()); const a = tagged(h.comp, 100)[0];
  const second = {...legacyOvershootPayload(101, 2, 4, 100), transitionFrames: 15};
  rebind(h, 101); h.add(second);
  const b = tagged(h.comp, 101).find(l => l !== a), span = a.outPoint - b.inPoint;
  near(span * fps, 15); equal(b.marker.key(1).parameters.l2daeCurve, '2');
  near(evaluate(h.effect, h.comp, b.inPoint + span * .25)[4], 22.216796875);
  near(evaluate(h.effect, h.comp, b.inPoint + span * .8)[4], 104.96);
  near(evaluate(h.effect, h.comp, b.inPoint + span * .95)[4], 100.882578125);
}

// Explicit cuts never overshoot. Expressions remain linear fades whether their
// payload omits the motion-only setting or carries any legacy/new curve ID.
{
  const h = host(); h.add(legacyOvershootPayload());
  rebind(h, 101); h.add({...legacyOvershootPayload(101, 2, 3, 100), transitionFrames: 0});
  clockResult(evaluate(h.effect, h.comp, 3.999), [1, 1, 3.999, 3.999, 0]);
  clockResult(evaluate(h.effect, h.comp, 4), [2, 2, 0, 0, 0]);
  const reference = host(); rebindExpression(reference); reference.add(expressionPayload(200, 1, 4));
  reference.comp.time = 3.8; rebindExpression(reference, 201);
  reference.add(expressionPayload(201, 2, 4, 200));
  for (const curve of [0, 1, 2, 3, 4, 5]) {
    const sample = host(); rebindExpression(sample);
    sample.add({...expressionPayload(200, 1, 4), transitionCurve: curve});
    sample.comp.time = 3.8; rebindExpression(sample, 201);
    sample.add({...expressionPayload(201, 2, 4, 200), transitionCurve: curve});
    for (const t of [0, .05, .1, 3.8, 3.85, 3.9, 3.95, 4, 7.75])
      equal(evaluateExpressions(sample.effect, sample.comp, t), evaluateExpressions(reference.effect, reference.comp, t));
  }
}

// Reordered/nested effect properties are resolved by stable match names through
// arbitrary localized topic names. Both channels, rollback and cloned ownership
// must work even when a host exposes no direct root-level parameter aliases.
{
  const h = host(); localizeEffect(h.effect); const groups = groupParameterFixture(h.effect);
  rebindExpression(h);
  equal(h.batch([legacyOvershootPayload(), expressionPayload()]), 'OK');
  equal(tagged(h.comp, 100).length, 1); equal(tagged(h.comp, 200, 'expression').length, 1);
  equal(tagged(h.comp, 100)[0].marker.key(1).parameters.l2daeCurve, '2');
  rebind(h, 101); rebindExpression(h, 201); h.comp.time = 3.8;
  equal(h.batch([legacyOvershootPayload(101, 2, 3, 100), expressionPayload(201, 2, 3, 200)]), 'OK');
  clockResult(evaluate(h.effect, h.comp, 3.96), [1, 2, 3.96, .16, 104.96]);
  clockResult(evaluateExpressions(h.effect, h.comp, 3.9), [2, 2, 50, 0]);
  for (const group of groups)
    ok(group.lookups.filter(v => typeof v === 'string').every(v => /^L2DAE Native Renderer-\d{4}$/.test(v)),
      'group traversal uses stable match names, never localized display labels');
  rebind(h, 102); rebindExpression(h, 202);
  const before = snapshot(h.comp);
  parameter(h.effect, expressionNames[3]).failExpressionOnce = true;
  throws(() => h.batch([legacyOvershootPayload(102, 3, 3, 101), expressionPayload(202, 3, 3, 201)]), /Injected/);
  equal(snapshot(h.comp), before); equal(h.app.depth, 0);
  const originalEffect = h.effect, oldSample = evaluate(h.effect, h.comp, 3.96);
  const duplicated = h.model.duplicate(); h.model = duplicated; h.effect = duplicated.effects[0];
  rebind(h, 103); equal(h.add(legacyOvershootPayload(103, 3, 3, 102)), 'OK');
  equal(evaluate(originalEffect, h.comp, 3.96), oldSample);
  equal(tagged(h.comp, 103).length, 3);
}

// A user-renamed display label that happens to resemble the requested match
// name cannot hide a missing stream, including inside a nested topic group.
{
  const h = host(); groupParameterFixture(h.effect);
  const target = parameter(h.effect, 'Motion A index');
  target.name = parameterMatchName('Motion A index'); target.matchName = 'L2DAE Native Renderer-0999';
  const before = snapshot(h.comp);
  throws(() => h.add(legacyOvershootPayload()), /缺少必要的 AeGO Flash 效果参数/);
  equal(snapshot(h.comp), before); equal(target.expression, ''); equal(h.app.depth, 0);
}

// Explicit amplitude selections retain their 1.6.0 curves and 15-frame timing.
// Rebound presets peak earlier so the return occupies 40% of the overlap.
// Expected values use analytic landmarks, not a duplicate implementation.
for (const [curve, amplitude] of [[3, 0], [4, .03], [5, .075]]) {
  const h = host(); localizeEffect(h.effect); groupParameterFixture(h.effect);
  h.add(defaultMotionPayload()); const a = tagged(h.comp, 100)[0];
  equal(a.marker.key(1).parameters.l2daeCurve, '5', 'omitted motion preset defaults to pronounced rebound');
  const next = {...defaultMotionPayload(101, 2, 3, 100), transitionFrames: 15, transitionCurve: curve};
  rebind(h, 101); h.add(next); const b = tagged(h.comp, 101).find(l => l !== a);
  equal(b.marker.key(1).parameters.l2daeCurve, String(curve));
  near((a.outPoint - b.inPoint) / h.comp.frameDuration, 15);
  const start = b.inPoint, span = a.outPoint - start;
  const landmarks = curve === 3 ? [[0, 0], [.25, .103515625], [.5, .5], [.75, .896484375], [.9, .99144]] :
    [[0, 0], [.15, (1 + amplitude) * .103515625], [.3, (1 + amplitude) / 2],
     [.45, (1 + amplitude) * .896484375], [.6, 1 + amplitude],
     [.7, 1 + amplitude * .896484375], [.8, 1 + amplitude / 2],
     [.9, 1 + amplitude * .103515625], [.95, 1 + amplitude * .01605224609375]];
  for (const [u, expected] of landmarks) {
    const t = start + span * u;
    clockResult(evaluate(h.effect, h.comp, t), [1, 2, t, t - b.startTime, expected * 100]);
  }
  const compiled = new vm.Script(parameter(h.effect, controlNames[4]).expression);
  const context = vm.createContext({thisComp: h.comp, time: 0});
  const weightAt = u => {
    // Exactly at the end the actual routing is B/B with zero mix. Use its
    // equivalent A/B weight 1 only for endpoint derivative limits below.
    if (u === 1) return 1;
    context.time = start + span * u;
    return compiled.runInContext(context) / 100;
  };
  let previous = 0, peak = 0, peakProgress = 0;
  const samples = [];
  for (let i = 0; i <= 400; ++i) {
    const u = i / 400, weight = weightAt(u);
    ok(Number.isFinite(weight) && weight >= 0 && weight <= 1 + amplitude + 1e-12,
      'preset stays within its requested amplitude');
    if (i > 0) ok(curve === 3 || i <= 240 ? weight > previous : weight < previous,
      curve === 3 ? 'no-rebound curve is monotone throughout' : 'one smooth peak followed by one monotone return');
    if (weight > peak) { peak = weight; peakProgress = u; }
    previous = weight; samples.push(weight);
  }
  near(peak, 1 + amplitude); near(peakProgress, curve === 3 ? 1 : .6);
  if (curve !== 3) {
    near(weightAt(.8) - 1, amplitude / 2, 'half the rebound remains halfway through the 40% tail');
    ok(weightAt(.95) > 1 && weightAt(.95) < 1 + amplitude * .02,
      'last 5% finishes gradually with less than 2% of the rebound remaining');
  }
  // One-sided first and second differences must approach zero at both ends
  // and, for rebound presets, on both sides of the peak. Smaller step sizes
  // distinguish C2 joining from a merely continuous or zero-velocity corner.
  const sides = [[0, 1], [1, -1]];
  if (curve !== 3) sides.push([.6, -1], [.6, 1]);
  for (const [point, direction] of sides) {
    const first = step => (weightAt(point + direction * step) - weightAt(point)) / step;
    const second = step => (weightAt(point + direction * 2 * step) -
      2 * weightAt(point + direction * step) + weightAt(point)) / (step * step);
    const coarse = 1e-3, fine = 2.5e-4;
    ok(Math.abs(first(fine)) < 1e-5, 'endpoint/join velocity tends to zero');
    ok(Math.abs(second(fine)) < .08, 'endpoint/join acceleration is small near the boundary');
    ok(Math.abs(second(fine)) < Math.abs(second(coarse)) * .3 + 1e-6,
      'endpoint/join acceleration converges to zero rather than a nonzero corner');
  }
  for (const i of [399, 240, 50, 350, 0, 200, 320, 240])
    equal(weightAt(i / 400), samples[i], 'preset is deterministic for out-of-order frame requests');
  const pose = t => {
    const r = evaluate(h.effect, h.comp, t), weight = r[4] / 100;
    return (100 * r[0] + r[2]) * (1 - weight) + (100 * r[1] + r[3]) * weight;
  };
  for (const boundary of [start, a.outPoint]) {
    const delta = 1e-6, center = pose(boundary);
    ok(Math.abs(pose(boundary - delta) - pose(boundary + delta)) < 1e-4);
    ok(Math.abs((center - pose(boundary - delta)) / delta -
      (pose(boundary + delta) - center) / delta) < .002, 'routing preserves the zero-velocity envelope endpoints');
  }
  clockResult(evaluate(h.effect, h.comp, a.outPoint), [2, 2, span, span, 0]);
}

// Every old curve remains byte-for-byte stable as new presets are imported,
// retagged, and cloned with a model; each new preset persists independently.
{
  const h = host(); h.add({...payload(100, 1, 3), transitionCurve: 0});
  rebind(h, 101); h.add({...payload(101, 2, 3, 100), transitionCurve: 1});
  rebind(h, 102); h.add({...payload(102, 3, 3, 101), transitionCurve: 2});
  const times = [2.85, 2.9, 5.65, 5.7], oldValues = times.map(t => evaluate(h.effect, h.comp, t));
  for (const curve of [3, 4, 5]) {
    rebind(h, 100 + curve); h.add({...payload(100 + curve, curve + 1, 3, 99 + curve), transitionCurve: curve});
  }
  const originalEffect = h.effect, original = tagged(h.comp, 105);
  equal(original.map(l => l.marker.key(1).parameters.l2daeCurve).sort(), ['0', '1', '2', '3', '4', '5']);
  times.forEach((t, i) => equal(evaluate(h.effect, h.comp, t), oldValues[i]));
  const duplicate = h.model.duplicate(); h.model = duplicate; h.effect = duplicate.effects[0];
  rebind(h, 106); h.add(defaultMotionPayload(106, 7, 3, 105));
  equal(original.map(l => l.marker.key(1).parameters.l2daeCurve).sort(), ['0', '1', '2', '3', '4', '5']);
  equal(tagged(h.comp, 106).map(l => l.marker.key(1).parameters.l2daeCurve).sort(), ['0', '1', '2', '3', '4', '5', '5']);
  times.forEach((t, i) => equal(evaluate(originalEffect, h.comp, t), oldValues[i]));
}

// Native positive/reverse stretch, cropping, dragging and duplicating affect
// placement/source clocks but preserve the amplitude stored on the new clip.
for (const [curve, middleWeight] of [[3, 50], [4, 103], [5, 107.5]]) {
  const u = curve === 3 ? .5 : .6, h = host(); h.add(defaultMotionPayload());
  const a = tagged(h.comp, 100)[0]; a.stretch = 200; a.outPoint = 8;
  rebind(h, 101); h.add({...payload(101, 2, 4, 100), transitionFrames: 60, transitionCurve: curve});
  const b = tagged(h.comp, 101).find(l => l !== a);
  let t = 6 + 2 * u;
  clockResult(evaluate(h.effect, h.comp, t), [1, 2, t / 2, t - 6, middleWeight]);
  a.stretch = -100; a.startTime = 8; a.inPoint = 8; a.outPoint = 0;
  b.stretch = -200; b.startTime = 14; b.inPoint = 14; b.outPoint = 6;
  clockResult(evaluate(h.effect, h.comp, t), [1, 2, 8 - t, (14 - t) / 2, middleWeight]);
  b.outPoint = 6.25; t = 6.25 + 1.75 * u;
  clockResult(evaluate(h.effect, h.comp, t), [1, 2, 8 - t, (14 - t) / 2, middleWeight]);
  const duplicate = b.duplicate(); b.enabled = false; duplicate.startTime += .25;
  t = 6.5 + 1.5 * u;
  clockResult(evaluate(h.effect, h.comp, t), [1, 2, 8 - t, (14.25 - t) / 2, middleWeight]);
  equal(duplicate.marker.key(1).parameters.l2daeCurve, String(curve));
}

// Nested clips use the same smooth envelope in reverse on the way out; zero
// overlap remains a cut, and failed imports retain every existing marker.
for (const curve of [3, 4, 5]) {
  const h = host(); h.add(defaultMotionPayload(100, 1, 10));
  h.comp.time = 3; rebind(h, 101);
  h.add({...payload(101, 2, 3, 100, false), transitionCurve: curve});
  for (const delta of [.025, .05, .1, .12, .16, .19])
    near(evaluate(h.effect, h.comp, 3 + delta)[4], evaluate(h.effect, h.comp, 6 - delta)[4]);
  clockResult(evaluate(h.effect, h.comp, 6), [1, 1, 6, 6, 0]);
  rebind(h, 102);
  const beforeReimport = snapshot(h.comp); parameter(h.effect, controlNames[3]).failExpressionOnce = true;
  throws(() => h.add({...payload(102, 3, 3, 101), transitionCurve: curve}), /Injected/);
  equal(snapshot(h.comp), beforeReimport);
  const cut = host(); cut.add(defaultMotionPayload()); rebind(cut, 101);
  cut.add({...payload(101, 2, 3, 100), transitionFrames: 0, transitionCurve: curve});
  clockResult(evaluate(cut.effect, cut.comp, 3.999), [1, 1, 3.999, 3.999, 0]);
  clockResult(evaluate(cut.effect, cut.comp, 4), [2, 2, 0, 0, 0]);
}

for (const fps of [23.976, 24, 25, 30, 60]) {
  for (const curve of [3, 4, 5]) {
    const h = host(); h.comp.frameDuration = 1 / fps; h.add(defaultMotionPayload());
    const a = tagged(h.comp, 100)[0], next = {...defaultMotionPayload(101, 2, 4, 100), transitionCurve: curve};
    delete next.transitionFrames; rebind(h, 101); h.add(next);
    const b = tagged(h.comp, 101).find(l => l !== a), span = a.outPoint - b.inPoint;
    near(span * fps, 30);
    near(evaluate(h.effect, h.comp, b.inPoint + span * (curve === 3 ? .5 : .6))[4],
      curve === 3 ? 50 : curve === 4 ? 103 : 107.5);
    near(evaluate(h.effect, h.comp, b.inPoint + span * .8)[4],
      curve === 3 ? 94.208 : curve === 4 ? 101.5 : 103.75);
  }
}

// First-use payloads omit both preferences: 30 frames and pronounced rebound.
// Expression timing receives the same duration default while its fade stays linear.
for (const fps of [23.976, 24, 25, 30, 60]) {
  const h = host(); h.comp.frameDuration = 1 / fps; rebindExpression(h);
  const first = defaultMotionPayload(), facePayload = expressionPayload(200, 1, 4);
  delete first.transitionFrames; delete facePayload.transitionFrames;
  equal(h.batch([first, facePayload]), 'OK');
  const a = tagged(h.comp, 100)[0], face = tagged(h.comp, 200, 'expression')[0];
  equal(a.marker.key(1).parameters.l2daeCurve, '5');
  equal(face.marker.key(1).parameters.l2daeCurve, '0');
  near(Number(a.marker.key(1).parameters.l2daeFade), 30 / fps);
  near(Number(face.marker.key(1).parameters.l2daeFade), 30 / fps);
  clockResult(evaluateExpressions(h.effect, h.comp, 15 / fps), [1, 1, 50, 0]);
  const second = defaultMotionPayload(101, 2, 4, 100); delete second.transitionFrames;
  rebind(h, 101); equal(h.add(second), 'OK');
  const b = tagged(h.comp, 101).find(l => l !== a), span = a.outPoint - b.inPoint;
  near(span * fps, 30); equal(b.marker.key(1).parameters.l2daeCurve, '5');
  const peakTime = b.inPoint + span * .6;
  clockResult(evaluate(h.effect, h.comp, peakTime), [1, 2, peakTime, span * .6, 107.5]);
  near(evaluate(h.effect, h.comp, b.inPoint + span * .8)[4], 103.75);
  near(evaluate(h.effect, h.comp, b.inPoint + span * .95)[4], 100.12039184570313);
}

// Thirty-frame defaults still clamp against half of each source's visible span.
// Test an incoming short clip, a preceding short clip, and both short together.
for (const fps of [23.976, 24, 25, 30, 60]) {
  for (const [previousDuration, nextDuration] of [[4, .1], [.2, 4], [.2, .1]]) {
    const h = host(); h.comp.frameDuration = 1 / fps;
    const first = defaultMotionPayload(100, 1, previousDuration); delete first.transitionFrames;
    h.add(first); const a = tagged(h.comp, 100)[0];
    const next = defaultMotionPayload(101, 2, nextDuration, 100); delete next.transitionFrames;
    rebind(h, 101); h.add(next); const b = tagged(h.comp, 101).find(l => l !== a);
    const span = Math.min(30 / fps, previousDuration / 2, nextDuration / 2);
    near(b.inPoint, previousDuration - span);
    near(Number(b.marker.key(1).parameters.l2daeFade), 30 / fps, 'stored preference is not shortened by the clip clamp');
    equal(b.marker.key(1).parameters.l2daeCurve, '5');
    near(evaluate(h.effect, h.comp, b.inPoint + span * .6)[4], 107.5);
    clockResult(evaluate(h.effect, h.comp, previousDuration), [2, 2, span, span, 0]);
  }
}

// A remembered native selection is explicit in the payload and overrides the
// first-use pronounced fallback. Previously stored metadata is not rewritten.
for (const rememberedCurve of [3, 4, 5]) {
  for (const previousCurve of [0, 1, 2, 3, 4, 5]) {
    for (const previousFrames of [0, 6, 15, 30, 60]) {
      const h = host();
      h.add({...payload(100, 1, 6), transitionFrames: previousFrames, transitionCurve: previousCurve});
      const a = tagged(h.comp, 100)[0], oldParameters = a.marker.key(1).parameters;
      const oldMetadata = {curve: oldParameters.l2daeCurve, fade: oldParameters.l2daeFade};
      near(Number(oldMetadata.fade), previousFrames / 30);
      equal(oldMetadata.curve, String(previousCurve));
      const next = {...defaultMotionPayload(101, 2, 4, 100), transitionCurve: rememberedCurve};
      delete next.transitionFrames; rebind(h, 101); h.add(next);
      const b = tagged(h.comp, 101).find(l => l !== a);
      equal(b.marker.key(1).parameters.l2daeCurve, String(rememberedCurve));
      near(Number(b.marker.key(1).parameters.l2daeFade), 1);
      near(b.inPoint, 5);
      equal(a.marker.key(1).parameters.l2daeCurve, oldMetadata.curve);
      equal(a.marker.key(1).parameters.l2daeFade, oldMetadata.fade);
      near(evaluate(h.effect, h.comp, b.inPoint + (rememberedCurve === 3 ? .5 : .6))[4],
        rememberedCurve === 3 ? 50 : rememberedCurve === 4 ? 103 : 107.5);
    }
  }
}

console.log(`PASS: ${assertions} timeline clip assertions (motion/expression ExtendScript, 30-frame pronounced default, explicit remembered presets, C2 3%/7.5% curves, grouped stable IDs and saved-project compatibility).`);
