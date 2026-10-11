// CommonJS di proposito: chi fa require('assert') si aspetta la funzione stessa.
function assert(v, msg) { if (!v) throw new Error(msg instanceof Error ? msg.message : (msg || 'Assertion failed')); }
assert.ok = assert;
assert.equal = (a, b, m) => assert(a == b, m); assert.notEqual = (a, b, m) => assert(a != b, m);
assert.strictEqual = (a, b, m) => assert(a === b, m || `Expected ${a} === ${b}`); assert.notStrictEqual = (a, b, m) => assert(a !== b, m);
assert.deepStrictEqual = assert.deepEqual = (a, b, m) => assert(JSON.stringify(a) === JSON.stringify(b), m || 'deepEqual fallito');
assert.throws = (fn, _e, m) => { let t = false; try { fn(); } catch (_) { t = true; } assert(t, m || 'Missing expected exception'); };
assert.fail = (m) => { throw new Error(m || 'Failed'); };
assert.default = assert;
module.exports = assert;
