#!/usr/bin/env node
// Reproducible synthetic repository generator for proj benchmarks.
//
//   node bench/generate.mjs <output-dir> --files <N> [--seed <S>]
//
// Produces JavaScript (with some TypeScript and Python) modules with imports,
// classes, cross-module calls, Express routers, throw/try sites, one malformed
// file and one oversized file. The same seed and size always produce the same
// tree. The output directory must not exist or must be empty.
import { existsSync, mkdirSync, readdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

function parseArgs(argv) {
  const args = { out: null, files: 1000, seed: 1 };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--files') args.files = Number(argv[++i]);
    else if (a === '--seed') args.seed = Number(argv[++i]);
    else if (!args.out) args.out = a;
    else throw new Error(`unexpected argument ${a}`);
  }
  if (!args.out || !Number.isInteger(args.files) || args.files < 10) {
    throw new Error('usage: generate.mjs <output-dir> --files <N >= 10> [--seed <S>]');
  }
  return args;
}

// Mulberry32: small deterministic PRNG.
function prng(seed) {
  let t = seed >>> 0;
  return () => {
    t = (t + 0x6d2b79f5) >>> 0;
    let r = Math.imul(t ^ (t >>> 15), 1 | t);
    r = (r + Math.imul(r ^ (r >>> 7), 61 | r)) ^ r;
    return ((r ^ (r >>> 14)) >>> 0) / 4294967296;
  };
}

const PER_DIR = 50;
const FUNCS = 4;

function dirOf(i) {
  return `pkg${String(Math.floor(i / PER_DIR)).padStart(4, '0')}`;
}

function jsModule(i, rand, total) {
  const deps = new Set();
  const depCount = i === 0 ? 0 : 1 + Math.floor(rand() * 3);
  for (let k = 0; k < depCount; k++) deps.add(Math.floor(rand() * i));
  const lines = [];
  const imports = [...deps].map((d) => {
    const rel = dirOf(d) === dirOf(i) ? `./mod${d}` : `../${dirOf(d)}/mod${d}`;
    lines.push(`const m${d} = require('${rel}');`);
    return d;
  });
  lines.push(`const { format } = require('util');`);
  lines.push('');
  lines.push(`class Store${i} {`);
  lines.push(`  find(id) {`);
  lines.push(`    if (id < 0) {`);
  lines.push(`      throw new RangeError('negative id ' + id);`);
  lines.push(`    }`);
  lines.push(`    return { id, value: format('%d', id) };`);
  lines.push(`  }`);
  lines.push(`}`);
  lines.push('');
  lines.push(`class Service${i} {`);
  lines.push(`  constructor() {`);
  lines.push(`    this.store = new Store${i}();`);
  lines.push(`  }`);
  lines.push('');
  lines.push(`  load(id) {`);
  lines.push(`    try {`);
  lines.push(`      return this.store.find(id);`);
  lines.push(`    } catch (err) {`);
  lines.push(`      return null;`);
  lines.push(`    }`);
  lines.push(`  }`);
  lines.push(`}`);
  lines.push('');
  for (let f = 0; f < FUNCS; f++) {
    lines.push(`function fn${i}_${f}(x) {`);
    lines.push(`  const service = new Service${i}();`);
    if (imports.length > 0) {
      const d = imports[Math.floor(rand() * imports.length)];
      lines.push(`  const other = m${d}.fn${d}_${Math.floor(rand() * FUNCS)}(x - 1);`);
    } else {
      lines.push(`  const other = x;`);
    }
    if (f > 0) lines.push(`  const local = fn${i}_${f - 1}(x);`);
    lines.push(`  return service.load(other) || ${f > 0 ? 'local' : 'x'};`);
    lines.push(`}`);
    lines.push('');
  }
  const exportsList = Array.from({ length: FUNCS }, (_, f) => `fn${i}_${f}`);
  if (i % 20 === 0) {
    lines.push(`const express = require('express');`);
    lines.push(`const router = express.Router();`);
    lines.push(`router.get('/items${i}/:id', (req, res) => {`);
    lines.push(`  res.json(fn${i}_${FUNCS - 1}(Number(req.params.id)));`);
    lines.push(`});`);
    lines.push(`router.post('/items${i}', fn${i}_0);`);
    exportsList.push('router');
  }
  lines.push(`module.exports = { ${exportsList.join(', ')}, Service${i} };`);
  return lines.join('\n') + '\n';
}

function tsModule(i) {
  return `export interface Item${i} {
  id: number;
  name: string;
}

export class Repo${i} {
  private items: Item${i}[] = [];

  all(): Item${i}[] {
    return this.items;
  }

  add(item: Item${i}): void {
    if (!item.name) {
      throw new Error('name required');
    }
    this.items.push(item);
  }
}

export function makeRepo${i}(): Repo${i} {
  const repo = new Repo${i}();
  repo.add({ id: ${i}, name: 'n${i}' });
  return repo;
}
`;
}

function pyModule(i, rand) {
  const dep = i > 0 ? Math.floor(rand() * i) : null;
  return `${dep !== null ? `from .py${dep} import helper${dep}\n` : ''}

class Model${i}:
    def __init__(self):
        self.items = []

    def add(self, value):
        if value is None:
            raise ValueError("value required")
        self.items.append(value)
        return len(self.items)


def helper${i}(x):
    model = Model${i}()
    ${dep !== null ? `return model.add(helper${dep}(x))` : 'return model.add(x)'}
`;
}

function main() {
  const { out, files, seed } = parseArgs(process.argv.slice(2));
  if (existsSync(out) && readdirSync(out).length > 0) throw new Error(`refusing to write into non-empty directory ${out}`);
  mkdirSync(out, { recursive: true });
  const rand = prng(seed);
  const jsCount = Math.floor(files * 0.8);
  const tsCount = Math.floor(files * 0.1);
  const pyCount = files - jsCount - tsCount - 2;
  let written = 0;
  for (let i = 0; i < jsCount; i++) {
    const dir = join(out, 'src', dirOf(i));
    if (i % PER_DIR === 0) mkdirSync(dir, { recursive: true });
    writeFileSync(join(dir, `mod${i}.js`), jsModule(i, rand, jsCount));
    written++;
  }
  mkdirSync(join(out, 'ts'), { recursive: true });
  for (let i = 0; i < tsCount; i++) {
    writeFileSync(join(out, 'ts', `repo${i}.ts`), tsModule(i));
    written++;
  }
  mkdirSync(join(out, 'py', 'pkg'), { recursive: true });
  writeFileSync(join(out, 'py', 'pkg', '__init__.py'), '');
  for (let i = 0; i < pyCount; i++) {
    writeFileSync(join(out, 'py', 'pkg', `py${i}.py`), pyModule(i, rand));
    written++;
  }
  // One malformed and one oversized file (beyond the default 1 MiB limit).
  writeFileSync(join(out, 'src', 'malformed.js'), 'function broken( {\n  return 1\n');
  writeFileSync(join(out, 'src', 'oversized.js'), '// generated\n' + 'const x = 1;\n'.repeat(90000));
  written += 2;
  writeFileSync(
    join(out, 'package.json'),
    JSON.stringify({ name: 'synthetic', version: '1.0.0', main: 'src/pkg0000/mod0.js', dependencies: { express: '4' } }, null, 2),
  );
  process.stdout.write(JSON.stringify({ out, files: written, seed }) + '\n');
}

main();
