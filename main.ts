require('node:v8').setFlagsFromString('--no-incremental-marking');
require('node:v8').setFlagsFromString('--no-turbofan --always-sparkplug');
const { readFileSync, writeFileSync } = require('node:fs');
const { run } = require('./solve.js');
const input = (process.env.MAKEFUN_INPUT_DIR ?? '/input') + '/cases.json';
const output = (process.env.MAKEFUN_OUTPUT_DIR ?? '/output') + '/answers.json';

let source = readFileSync(input, 'latin1');
const latin1 = !source.includes('\\u') || !/\\u(?!00[0-7][0-9a-f])[0-9a-f]{4}/i.test(source);
if (!latin1) source = Buffer.from(source, 'latin1').toString('utf8');
const answers = JSON.parse(source).map(({ input }) => run(input, { json: true, latin1 }));
writeFileSync(output, '[' + answers.join(',') + ']', latin1 ? 'latin1' : 'utf8');
