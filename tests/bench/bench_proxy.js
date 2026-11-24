/* OpenAI Chat Completions -> opencode generate/text bridge for the benchmark.
 * Lets the real a98.exe talk to a model hosted by the local OpenCode service.
 *
 * Live mode (default):
 *   node tests/bench/bench_proxy.js
 * Mock mode (no OpenCode / API key needed):
 *   node tests/bench/bench_proxy.js --mock=csv_parse
 *   set BENCH_MOCK=1 && node tests/bench/bench_proxy.js
 * Optional: --port=8802 (or BENCH_PORT=8802).
 */
'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const argv = process.argv.slice(2);

function argValue(prefix, fallback) {
  const a = argv.find(x => x.startsWith(prefix));
  return a ? a.slice(prefix.length) : fallback;
}

const PORT = Number(argValue('--port=', process.env.BENCH_PORT || '8801')) || 8801;
const OPENCODE = process.env.BENCH_OPENCODE ||
  'C:\\Users\\Administrator\\AppData\\Local\\hermes\\node\\node_modules\\@opencode\\cli\\bin\\opencode.exe';
/* tests/bench is two levels below the repository root. */
const LOCATION = path.resolve(__dirname, '..', '..');
const MODEL = { id: 'deepseek-flash', providerID: 'deepseek' };
/* All benchmark logs live under tests/bench. */
const LOG = path.join(__dirname, 'proxy.log');

function log(line) {
  fs.appendFileSync(LOG, line + '\n');
}

function generate(prompt) {
  const data = JSON.stringify({ prompt, model: MODEL });
  const out = execFileSync(OPENCODE, [
    'api', 'post', '/api/experimental/generate/text',
    '--param', 'location.directory=' + LOCATION,
    '--data', data
  ], { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024, timeout: 240000 });
  const j = JSON.parse(out);
  if (!j.data || typeof j.data.text !== 'string') {
    throw new Error('bad generate response: ' + out);
  }
  return j.data.text;
}

function clip(s, n) {
  s = String(s == null ? '' : s);
  return s.length > n ? s.slice(0, n) + '\n...[truncated]' : s;
}

/* OpenAI transports tool arguments as a JSON *string* inside function.arguments.
 * The old bridge only accepted objects and silently turned those into {}, which
 * produced bogus read_file({}) / run_command({}) calls and wasted rounds. */
function parseArgs(a) {
  if (a && typeof a === 'object' && !Array.isArray(a)) return a;
  if (typeof a === 'string') {
    try {
      const o = JSON.parse(a);
      return o && typeof o === 'object' && !Array.isArray(o) ? o : {};
    } catch (_) { return {}; }
  }
  return {};
}

const ALLOWED = new Set([
  'read_file', 'write_file', 'append_file', 'edit', 'apply_patch', 'replace_across',
  'run_command', 'build', 'list_dir', 'tree', 'grep', 'glob', 'search_files',
  'diff', 'head', 'tail', 'which', 'session_info', 'todo_read', 'todo_write',
  'snapshot', 'restore'
]);

const WRITE_TOOLS = new Set([
  'write_file', 'edit', 'append_file', 'apply_patch', 'replace_across'
]);
const READ_TOOLS = new Set(['read_file', 'head', 'tail', 'list_dir', 'tree']);
const TEST_TOOLS = new Set(['run_command', 'build']);

function summarizeState(nonSys) {
  const readPaths = [];
  let lastWrite = null;
  let lastTest = null;
  for (let i = 0; i < nonSys.length; i++) {
    const m = nonSys[i];
    if (m.role === 'assistant' && Array.isArray(m.tool_calls)) {
      for (const c of m.tool_calls) {
        const f = c.function || {};
        const nm = f.name || '';
        const a = parseArgs(f.arguments);
        const p = a.path || a.filePath || a.file || '';
        if ((nm === 'read_file' || nm === 'head' || nm === 'tail') && p &&
            readPaths.indexOf(p) < 0) readPaths.push(p);
        if (WRITE_TOOLS.has(nm)) {
          lastWrite = { path: p || '(unknown)', index: i };
        }
      }
    }
    if (m.role === 'tool' && TEST_TOOLS.has(m.name)) {
      const c = String(m.content || '');
      if (/all visible tests passed/i.test(c) && /exit code:\s*0/.test(c)) {
        lastTest = { ok: true, index: i };
      } else if (/\bFAIL:|exit code:\s*[1-9]/.test(c)) {
        lastTest = { ok: false, index: i };
      }
    }
  }
  return { readPaths, lastWrite, lastTest };
}

function compactCalls(m) {
  if (!Array.isArray(m.tool_calls)) return '';
  const compact = m.tool_calls.map(c => {
    const f = c.function || {};
    const nm = f.name || '';
    const a = parseArgs(f.arguments);
    const p = a.path || a.filePath || a.file || '';
    if (WRITE_TOOLS.has(nm)) {
      return { name: nm, path: p, note: 'content omitted (already applied)' };
    }
    if (nm === 'run_command') {
      return { name: nm, command: clip(a.command || '', 160) };
    }
    return { name: nm, args: a };
  });
  return '\ntool_calls: ' + clip(JSON.stringify(compact), 1200);
}

function buildPrompt(body) {
  const msgs = Array.isArray(body.messages) ? body.messages : [];
  const tools = (Array.isArray(body.tools) ? body.tools : [])
    .filter(t => ALLOWED.has(t.function && t.function.name));
  const sys = msgs.filter(m => m.role === 'system').map(m => m.content || '').join('\n');
  const nonSys = msgs.filter(m => m.role !== 'system');
  const state = summarizeState(nonSys);
  const recent = nonSys.slice(-8);
  const conv = recent.map((m, i) => {
    let line = '[' + (i + 1) + '] ' + m.role;
    if (m.role === 'tool') {
      line += ' (name=' + (m.name || '?') + '):\n' + clip(m.content, 2000);
    } else if (m.role === 'assistant') {
      line += ':\n' + clip(m.content, 1200) + compactCalls(m);
    } else {
      line += ':\n' + clip(m.content, 2500);
    }
    return line;
  }).join('\n\n');

  const compactTools = tools.map(t => {
    const f = t.function || {};
    return { name: f.name, description: clip(f.description, 240) };
  });

  const passedAfterWrite = state.lastTest && state.lastTest.ok &&
    state.lastWrite && state.lastTest.index > state.lastWrite.index;
  const stateLines = [
    '=== CURRENT STATE ===',
    'Files already read: ' + (state.readPaths.length ? state.readPaths.join(', ') : 'none'),
    'Last write: ' + (state.lastWrite ? state.lastWrite.path : 'none'),
    'Last test: ' + (!state.lastTest ? 'none'
      : state.lastTest.ok ? 'PASSED' : 'FAILED'),
    passedAfterWrite
      ? 'The acceptance tests PASSED after the last write. Output ANSWER: now; do not re-read files.'
      : ''
  ].filter(Boolean);

  return [
    'You are the reasoning engine inside an agent program called AIagent98.',
    'You are given the agent system prompt, the available tools and the conversation so far.',
    'Reply with EXACTLY ONE of these two formats, with no markdown fence and no extra text.',
    '',
    'To call tools, output:',
    'TOOLS:',
    '[{"name":"<tool name>","arguments":{...}}]',
    '',
    'When the task is finished, output:',
    'ANSWER:',
    '<concise final answer>',
    '',
    'Rules:',
    '- Use only tool names from the tool list.',
    '- "arguments" must be a JSON object matching that tool schema.',
    '- Never output both formats. Never wrap the JSON in a code fence.',
    '- Never call a tool with missing required arguments: read_file needs "path", run_command needs "command".',
    '- Reading files is preparation, not progress. Once you have the spec and the test command, write the implementation.',
    '- Do not call read_file on a path listed in "Files already read" unless you changed that file or need a specific unseen line range; use head/tail with line numbers instead.',
    '- If the latest test output shows all tests passed after the last write, answer immediately with ANSWER:.',
    '- Prefer tools while work remains; answer only when the task is actually complete.',
    '',
    '=== AGENT SYSTEM PROMPT ===',
    clip(sys, 3000),
    '',
    stateLines.join('\n'),
    '',
    '=== AVAILABLE TOOLS ===',
    JSON.stringify(compactTools),
    '',
    '=== CONVERSATION ===',
    conv
  ].join('\n');
}

function callsFromArray(arr, validNames) {
  if (!Array.isArray(arr)) throw new Error('tool payload is not an array');
  return arr.map(c => {
    const name = c.name || (c.function && c.function.name) || '';
    if (!validNames.has(name)) throw new Error('unknown tool: ' + name);
    let raw = c.arguments !== undefined ? c.arguments
      : c.args !== undefined ? c.args
      : (c.function && c.function.arguments);
    if (raw === undefined) {
      /* Some replies flatten arguments next to "name": {"name":"read_file","path":"..."} */
      raw = {};
      for (const k of Object.keys(c)) {
        if (['name', 'function', 'id', 'type'].indexOf(k) < 0) raw[k] = c[k];
      }
    }
    return { name, arguments: parseArgs(raw) };
  });
}

function callsFromDsml(t, validNames) {
  if (!/DSML/.test(t)) return null;
  const parts = t.split(/<[|｜]{1,2}DSML[|｜]{1,2}\s*invoke/);
  const calls = [];
  for (let i = 1; i < parts.length; i++) {
    const p = parts[i];
    const nm = /name="([^"]+)"/.exec(p);
    if (!nm || !validNames.has(nm[1])) continue;
    const a = p.indexOf('{');
    const b = p.lastIndexOf('}');
    let args;
    if (a >= 0 && b > a) {
      args = parseArgs(p.slice(a, b + 1));
    } else {
      /* DeepSeek also emits DSML as named parameter elements instead of one
       * JSON "arguments" blob.  Parse those into the same args object. */
      args = {};
      const re = /<[^>]*parameter[^>]*name="([^"]+)"[^>]*>([\s\S]*?)<\/[^>]*parameter[^>]*>/gi;
      let m;
      while ((m = re.exec(p)) !== null) {
        let v = m[2].trim();
        if (v === 'true') v = true;
        else if (v === 'false') v = false;
        else if (/^-?\d+$/.test(v)) v = Number(v);
        args[m[1]] = v;
      }
    }
    calls.push({ name: nm[1], arguments: args });
  }
  return calls.length ? calls : null;
}

function callsFromToolCallBlocks(t, validNames) {
  if (!/<tool_?calls?>/i.test(t)) return null;
  const body = t.replace(/<\/?tool_?calls?>/gi, '').trim();
  const calls = [];
  for (const line of body.split(/\r?\n/)) {
    const s = line.trim();
    if (!s) continue;
    try {
      const o = JSON.parse(s);
      const name = o.name || (o.function && o.function.name) || '';
      if (!validNames.has(name)) continue;
      const raw = o.arguments !== undefined ? o.arguments
        : (o.function && o.function.arguments);
      calls.push({ name, arguments: parseArgs(raw) });
    } catch (_) {}
  }
  return calls.length ? calls : null;
}

function callsFromNamedBlocks(t, validNames) {
  const calls = [];
  const re = /(?:^|\n)\s*([a-z_][a-z0-9_]*)\s*:\s*(\{[\s\S]*?\})\s*(?=\n|$)/gi;
  let m;
  while ((m = re.exec(t)) !== null) {
    if (!validNames.has(m[1])) continue;
    calls.push({ name: m[1], arguments: parseArgs(m[2]) });
  }
  return calls.length ? calls : null;
}

function callsFromFunctionStyle(t, validNames) {
  const calls = [];
  const re = /\b([a-z_][a-z0-9_]*)\s*\(\s*(\{[\s\S]*?\})\s*\)/gi;
  let m;
  while ((m = re.exec(t)) !== null) {
    if (!validNames.has(m[1])) continue;
    calls.push({ name: m[1], arguments: parseArgs(m[2]) });
  }
  return calls.length ? calls : null;
}

function parseReply(text, validNames) {
  const t = String(text || '').trim().replace(/^tool_calls\s*:/i, 'TOOLS:');
  const dsml = callsFromDsml(t, validNames);
  if (dsml) return { content: '', calls: dsml };
  const blocks = callsFromToolCallBlocks(t, validNames);
  if (blocks) return { content: '', calls: blocks };
  const iTools = t.lastIndexOf('TOOLS:');
  const iAnswer = t.lastIndexOf('ANSWER:');

  if (iAnswer > iTools) {
    return { content: t.slice(iAnswer + 7).trim(), calls: [] };
  }
  if (iTools >= 0) {
    const payload = t.slice(iTools + 6).trim();
    const a = payload.indexOf('[');
    const b = payload.lastIndexOf(']');
    if (a < 0 || b <= a) throw new Error('no JSON array after TOOLS:');
    return { content: '', calls: callsFromArray(JSON.parse(payload.slice(a, b + 1)), validNames) };
  }
  /* Some models emit a bare JSON array without the marker. */
  const a = t.indexOf('[');
  const b = t.lastIndexOf(']');
  if (a >= 0 && b > a) {
    try {
      const arr = JSON.parse(t.slice(a, b + 1));
      if (Array.isArray(arr)) return { content: '', calls: callsFromArray(arr, validNames) };
    } catch (_) {}
  }
  const named = callsFromNamedBlocks(t, validNames);
  if (named) return { content: '', calls: named };
  const fnStyle = callsFromFunctionStyle(t, validNames);
  if (fnStyle) return { content: '', calls: fnStyle };
  return { content: t, calls: [] };
}

/* ------------------------------------------------------------------ mock mode
 * Canned, correct four-step sequences. No model / CLI / API key is involved.
 * The state machine looks at the tool calls the agent has already emitted:
 *   no reads -> read the fixture files
 *   reads but no writes -> write the fixed source(s)
 *   writes but no test -> run run_tests.bat
 *   test already run -> ANSWER
 * This mirrors the live benchmark flow and keeps the efficiency score honest.
 */
const CSV_FIXED_SOURCE = [
  '#include <limits.h>',
  '#include <ctype.h>',
  '#include "strutil.h"',
  '',
  'static int is_space_char(char c)',
  '{',
  '    return isspace((unsigned char)c) != 0;',
  '}',
  '',
  'int csv_parse_int_list(const char *text, int *out, int max)',
  '{',
  '    const char *p;',
  '    int count = 0;',
  '',
  '    if (text == NULL || out == NULL || max < 0)',
  '        return -1;',
  '',
  '    p = text;',
  '    while (*p != \'\\0\' && is_space_char(*p))',
  '        p++;',
  '    if (*p == \'\\0\')',
  '        return 0;',
  '',
  '    for (;;) {',
  '        int negative = 0;',
  '        unsigned int mag = 0;',
  '        unsigned int limit;',
  '        int value;',
  '',
  '        while (*p != \'\\0\' && is_space_char(*p))',
  '            p++;',
  '',
  '        if (*p == \'+\' || *p == \'-\') {',
  '            if (*p == \'-\')',
  '                negative = 1;',
  '            p++;',
  '        }',
  '',
  '        if (!(*p >= \'0\' && *p <= \'9\'))',
  '            return -1;',
  '',
  '        limit = negative ? (unsigned int)INT_MAX + 1u : (unsigned int)INT_MAX;',
  '',
  '        while (*p >= \'0\' && *p <= \'9\') {',
  '            unsigned int d = (unsigned int)(*p - \'0\');',
  '            if (mag > (limit - d) / 10u)',
  '                return -1;',
  '            mag = mag * 10u + d;',
  '            p++;',
  '        }',
  '',
  '        if (negative) {',
  '            if (mag == (unsigned int)INT_MAX + 1u)',
  '                value = INT_MIN;',
  '            else',
  '                value = -(int)mag;',
  '        } else {',
  '            value = (int)mag;',
  '        }',
  '',
  '        if (count >= max)',
  '            return -1;',
  '        out[count++] = value;',
  '',
  '        while (*p != \'\\0\' && is_space_char(*p))',
  '            p++;',
  '',
  '        if (*p == \',\') {',
  '            p++;',
  '            continue;',
  '        }',
  '        if (*p == \'\\0\')',
  '            break;',
  '        return -1;',
  '    }',
  '',
  '    return count;',
  '}',
  ''
].join('\n');

const BUGFIX_FIXED_SOURCE = [
  '#include "counter.h"',
  '',
  'int counter_add(int *counter, int delta)',
  '{',
  '    long long next;',
  '',
  '    if (counter == NULL)',
  '        return -1;',
  '    next = (long long)*counter + (long long)delta;',
  '    if (next < 0 || next > 100)',
  '        return -1;',
  '    *counter = (int)next;',
  '    return 0;',
  '}',
  ''
].join('\n');

const IDSET_FIXED_SOURCE = [
  '#include <stddef.h>',
  '#include "idset.h"',
  '',
  'int idset_add(int *ids, int *count, int capacity, int id)',
  '{',
  '    int i;',
  '',
  '    if (ids == NULL || count == NULL || capacity < 0 || *count < 0 ||',
  '        *count > capacity || id < 0)',
  '        return -1;',
  '    for (i = 0; i < *count; i++) {',
  '        if (ids[i] == id)',
  '            return -1;',
  '    }',
  '    if (*count == capacity)',
  '        return -1;',
  '    ids[*count] = id;',
  '    (*count)++;',
  '    return 0;',
  '}',
  ''
].join('\n');

const IDFMT_FIXED_SOURCE = [
  '#include <stdio.h>',
  '#include <string.h>',
  '#include "idset.h"',
  '',
  'int idfmt_write(const int *ids, int count, char *out, size_t out_size)',
  '{',
  '    int i, used = 0;',
  '',
  '    if (out == NULL || out_size == 0 || count < 0 || (count > 0 && ids == NULL))',
  '        return -1;',
  '    for (i = 0; i < count; i++) {',
  '        char part[32];',
  '        int n;',
  '',
  '        if (ids[i] < 0)',
  '            return -1;',
  '        n = snprintf(part, sizeof(part), "%s%d", i == 0 ? "[" : ",", ids[i]);',
  '        if (n < 0 || (size_t)(used + n) >= out_size)',
  '            return -1;',
  '        memcpy(out + used, part, (size_t)n);',
  '        used += n;',
  '    }',
  '    if ((size_t)(used + 2) > out_size)',
  '        return -1;',
  '    if (count == 0) {',
  '        out[0] = \'[\';',
  '        used = 1;',
  '    }',
  '    out[used++] = \']\';',
  '    out[used] = \'\\0\';',
  '    return used;',
  '}',
  ''
].join('\n');

const MOCKS = {
  csv_parse: {
    reads: ['include/strutil.h', 'src/strutil.c', 'tests/test_visible.c'],
    writes: [{ path: 'src/strutil.c', content: CSV_FIXED_SOURCE }],
    test: 'run_tests.bat',
    answer: 'Implemented csv_parse_int_list() with whitespace/sign/overflow handling; all visible tests pass.'
  },
  bugfix: {
    reads: ['include/counter.h', 'src/counter.c', 'tests/test_visible.c'],
    writes: [{ path: 'src/counter.c', content: BUGFIX_FIXED_SOURCE }],
    test: 'run_tests.bat',
    answer: 'Fixed counter_add() to reject NULL, below-zero, above-100 and overflowing updates; all visible tests pass.'
  },
  multifile: {
    reads: ['include/idset.h', 'src/idset.c', 'src/idfmt.c', 'tests/test_visible.c'],
    writes: [
      { path: 'src/idset.c', content: IDSET_FIXED_SOURCE },
      { path: 'src/idfmt.c', content: IDFMT_FIXED_SOURCE }
    ],
    test: 'run_tests.bat',
    answer: 'Implemented idset_add() and idfmt_write() across the two source files; all visible tests pass.'
  }
};

function getMockTask() {
  const a = argv.find(x => x === '--mock' || x.startsWith('--mock='));
  if (a) return a.startsWith('--mock=') ? (a.slice('--mock='.length) || 'csv_parse') : 'csv_parse';
  const e = process.env.BENCH_MOCK;
  if (e && e !== '0' && e !== 'false') return e === '1' ? 'csv_parse' : e;
  return null;
}

let MOCK_TASK = getMockTask();
if (MOCK_TASK && !MOCKS[MOCK_TASK]) {
  log('WARNING: unknown mock task "' + MOCK_TASK + '", using csv_parse');
  MOCK_TASK = 'csv_parse';
}

/* Non-streaming and SSE responses use the same shape as tests/mock_openai.js
 * so a98.ini can switch `stream` without changing the bridge. */
function sse(res, obj) {
  res.write('data: ' + JSON.stringify(obj) + '\n\n');
}

function chunk(delta, finish) {
  return {
    id: 'chatcmpl-bench-' + Date.now(),
    object: 'chat.completion.chunk',
    created: Math.floor(Date.now() / 1000),
    model: 'bench-model',
    choices: [{ index: 0, delta: delta, finish_reason: finish || null }]
  };
}

function sendCompletion(res, body, parsed, calls) {
  if (body.stream) {
    res.writeHead(200, {
      'content-type': 'text/event-stream',
      'cache-control': 'no-cache',
      'connection': 'close'
    });
    sse(res, chunk({ role: 'assistant' }));
    if (calls.length) {
      calls.forEach((c, i) => sse(res, chunk({ tool_calls: [{
        index: i, id: c.id, type: 'function', function: c.function
      }] })));
      sse(res, chunk({}, 'tool_calls'));
    } else {
      const words = String(parsed.content || '').split(' ');
      for (const word of words) sse(res, chunk({ content: word + ' ' }));
      sse(res, chunk({}, 'stop'));
    }
    res.write('data: [DONE]\n\n');
    res.end();
    return;
  }
  res.writeHead(200, { 'content-type': 'application/json' });
  res.end(JSON.stringify({
    id: 'chatcmpl-bench-' + Date.now(),
    object: 'chat.completion',
    created: Math.floor(Date.now() / 1000),
    model: 'bench-model',
    choices: [{
      index: 0,
      message: {
        role: 'assistant',
        content: parsed.content || null,
        tool_calls: calls.length ? calls : undefined
      },
      finish_reason: calls.length ? 'tool_calls' : 'stop'
    }],
    usage: { prompt_tokens: 0, completion_tokens: 0, total_tokens: 0 }
  }));
}

function mockReply(body) {
  const msgs = Array.isArray(body.messages) ? body.messages : [];
  const names = [];
  for (const m of msgs) {
    if (m.role === 'assistant' && Array.isArray(m.tool_calls)) {
      for (const c of m.tool_calls) {
        names.push((c.function && c.function.name) || c.name || '');
      }
    }
  }
  const spec = MOCKS[MOCK_TASK] || MOCKS.csv_parse;
  let parsed;
  if (!names.some(n => READ_TOOLS.has(n))) {
    parsed = { content: '', calls: spec.reads.map(p => ({ name: 'read_file', arguments: { path: p } })) };
  } else if (!names.some(n => WRITE_TOOLS.has(n))) {
    parsed = { content: '', calls: spec.writes.map(w => ({ name: 'write_file', arguments: { path: w.path, content: w.content } })) };
  } else if (!names.some(n => TEST_TOOLS.has(n))) {
    parsed = { content: '', calls: [{ name: 'run_command', arguments: { command: spec.test } }] };
  } else {
    parsed = { content: spec.answer, calls: [] };
  }
  return parsed;
}

const server = http.createServer((req, res) => {
  if (req.method === 'GET' && String(req.url).indexOf('/models') >= 0) {
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(JSON.stringify({
      object: 'list',
      data: [{ id: 'bench-model', object: 'model', owned_by: 'bench' }]
    }));
    return;
  }
  let raw = '';
  req.on('data', c => { raw += c; });
  req.on('end', () => {
    try {
      const body = JSON.parse(raw);
      const validNames = new Set((body.tools || [])
        .map(t => t.function && t.function.name)
        .filter(n => n && ALLOWED.has(n)));
      let text = '';
      let parsed;
      if (MOCK_TASK) {
        parsed = mockReply(body);
      } else {
        const prompt = buildPrompt(body);
        log('PROMPT BYTES: ' + Buffer.byteLength(prompt, 'utf8'));
        try {
          text = generate(prompt);
          parsed = parseReply(text, validNames);
        } catch (e) {
          log('RETRY after parse/generate error: ' + e.message);
          text = generate(prompt + '\n\nYour previous reply was invalid (' + e.message + '). Reply again using exactly TOOLS: or ANSWER:.');
          parsed = parseReply(text, validNames);
        }
      }
      const tag = MOCK_TASK ? 'MOCK[' + MOCK_TASK + ']' : 'LIVE';
      log('REQ ' + tag + ' tools=' + validNames.size + ' messages=' + (body.messages || []).length +
          ' -> ' + (parsed.calls.length ? 'TOOLS ' + parsed.calls.map(c => c.name).join(',') : 'ANSWER'));
      if (!MOCK_TASK) log('MODEL RAW: ' + clip(text, 1200));

      const calls = parsed.calls.map((c, i) => ({
        id: 'call_' + Date.now().toString(36) + '_' + i,
        type: 'function',
        function: { name: c.name, arguments: JSON.stringify(c.arguments) }
      }));
      sendCompletion(res, body, parsed, calls);
    } catch (e) {
      log('FATAL: ' + (e && e.stack || e));
      res.writeHead(500, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ error: { message: String(e && e.message || e) } }));
    }
  });
});

server.listen(PORT, '127.0.0.1', () =>
  log((MOCK_TASK ? 'mock mode (' + MOCK_TASK + ')' : 'live mode') + ' listening on ' + PORT));
