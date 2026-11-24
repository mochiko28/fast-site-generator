require('node:v8').setFlagsFromString('--no-incremental-marking --no-turbofan --always-sparkplug --semi-space-growth-factor=1 --no-parallel-scavenge --no-minor-gc-task');
const { readFileSync, writeFileSync, openSync, closeSync, writeSync } = require('node:fs');
const { run } = require('./solve.js');
const input = (process.env.MAKEFUN_INPUT_DIR ?? '/input') + '/cases.json';
const output = (process.env.MAKEFUN_OUTPUT_DIR ?? '/output') + '/answers.json';

let source = readFileSync(input, 'latin1');
const latin1 = !source.includes('\\u') || !/\\u(?!00[0-7][0-9a-f])[0-9a-f]{4}/i.test(source);
if (!latin1) source = Buffer.from(source, 'latin1').toString('utf8');
const cases = JSON.parse(source);
source = undefined;

const fd = openSync(output, 'w');
const options = { latin1, writeChunk, chunkSize: 32 * 1024 };
function writeChunk(chunk) {
  if (latin1) {
    const written = writeSync(fd, chunk, null, 'latin1');
    if (written < chunk.length) writeFileSync(fd, Buffer.from(chunk, 'latin1').subarray(written));
  } else {
    writeFileSync(fd, chunk, 'utf8');
  }
}
try {
  let separator = '[';
  for (let i = 0; i < cases.length; i++) {
    const site = cases[i].input;
    cases[i] = null;
    writeSync(fd, separator);
    run(site, options);
    separator = ',';
  }
  writeSync(fd, cases.length ? ']' : '[]');
} finally {
  closeSync(fd);
}
// answers.json is written and closed: skip Node's environment and heap teardown.
process.reallyExit(0);
