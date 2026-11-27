// SPDX-License-Identifier: GPL-2.0-or-later
// node tools/check-config.mjs <image>...: each image's config against docs/spec/config.schema.json. The JSON Schema
// subset the schema uses: type (one or a list), const, enum, pattern, minimum, required, properties,
// additionalProperties (false or a schema), items. Prints one line per image; exit status 1 if one does not conform.
import fs from 'node:fs'; import path from 'node:path';
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const schema = JSON.parse(fs.readFileSync(path.join(root, 'docs/spec/config.schema.json'), 'utf8'));
const typeOf = (v) => v === null ? 'null' : Array.isArray(v) ? 'array' : Number.isInteger(v) ? 'integer' : typeof v;
function check(s, v, at, errs) {
  if ('const' in s && JSON.stringify(v) !== JSON.stringify(s.const)) errs.push(`${at}: ${JSON.stringify(v)}, not ${JSON.stringify(s.const)}`);
  if (s.enum && !s.enum.some((e) => JSON.stringify(e) === JSON.stringify(v))) errs.push(`${at}: ${JSON.stringify(v)} is not one of ${JSON.stringify(s.enum)}`);
  if (s.type) {
    const want = [].concat(s.type), t = typeOf(v);
    if (!want.includes(t) && !(t === 'integer' && want.includes('number'))) { errs.push(`${at}: a ${t}, not ${want.join(' or ')}`); return; }
  }
  if (typeof v === 'string' && s.pattern && !new RegExp(s.pattern).test(v)) errs.push(`${at}: "${v}" does not match ${s.pattern}`);
  if (typeof v === 'number' && 'minimum' in s && v < s.minimum) errs.push(`${at}: ${v} is below ${s.minimum}`);
  if (typeOf(v) === 'object') {
    for (const k of s.required || []) if (!(k in v)) errs.push(`${at}: no ${k}`);
    for (const [k, x] of Object.entries(v)) {
      if (s.properties && k in s.properties) check(s.properties[k], x, `${at}.${k}`, errs);
      else if (s.additionalProperties === false) errs.push(`${at}: ${k} is not a field of the schema`);
      else if (typeof s.additionalProperties === 'object') check(s.additionalProperties, x, `${at}.${k}`, errs);
    }
  }
  if (Array.isArray(v) && s.items) v.forEach((x, i) => check(s.items, x, `${at}[${i}]`, errs));
}
let bad = 0;
for (const dir of process.argv.slice(2)) {
  const blob = (d) => JSON.parse(fs.readFileSync(path.join(dir, 'blobs/sha256', d.replace(/^sha256:/, '')), 'utf8'));
  const index = JSON.parse(fs.readFileSync(path.join(dir, 'index.json'), 'utf8'));
  const config = blob(blob(index.manifests[0].digest).config.digest);
  const errs = []; check(schema, config, 'config', errs);
  if (errs.length) { bad++; console.log(`${dir}: ${errs.length} differences from the schema:\n  ${errs.slice(0, 12).join('\n  ')}`); }
  else console.log(`${dir}: the config conforms (schema ${config.schema}, ${config.platform}, tree ${config.tree.digest.slice(0, 19)}...)`);
}
process.exit(bad ? 1 : 0);
