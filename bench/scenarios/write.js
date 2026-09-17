// Concurrent append: POST /write with unique id per iteration (__VU / __ITER).
import http from 'k6/http';
import { check } from 'k6';
import exec from 'k6/execution';
import { Counter } from 'k6/metrics';

const BASE_URL = __ENV.BASE_URL;
if (!BASE_URL) {
  throw new Error('BASE_URL is required (e.g. http://127.0.0.1:8000)');
}

const VUS = Number(__ENV.VUS || 32);
const WARMUP = __ENV.WARMUP_DURATION || '5s';
const MEASURE = __ENV.MEASURE_DURATION || '20s';
const WARMUP_DRAIN = __ENV.WRITE_WARMUP_DRAIN_DURATION || '2s';
const MEASURE_GRACEFUL_STOP =
  __ENV.WRITE_MEASURE_GRACEFUL_STOP_DURATION || '30s';
const successfulMeasuredWrites = new Counter('write_successful_measure');

function durationMillis(value) {
  const text = String(value).trim();
  const pattern = /(\d+(?:\.\d+)?)(ms|s|m|h)/g;
  let total = 0;
  let consumed = 0;
  let match;
  while ((match = pattern.exec(text)) !== null) {
    if (match.index !== consumed) {
      throw new Error(`invalid duration: ${value}`);
    }
    const factor = { ms: 1, s: 1000, m: 60000, h: 3600000 }[match[2]];
    total += Number(match[1]) * factor;
    consumed += match[0].length;
  }
  if (consumed !== text.length || total <= 0) {
    throw new Error(`invalid duration: ${value}`);
  }
  return total;
}

// The warmup executor must drain before measurement VUs start. Otherwise the
// two scenarios compete for the same VU pool, which can leave the measured
// phase with no requests under a saturated concurrent write workload.
const MEASURE_START = `${durationMillis(WARMUP) + durationMillis(WARMUP_DRAIN)}ms`;

export const options = {
  discardResponseBodies: false,
  summaryTrendStats: ['avg', 'min', 'med', 'max', 'p(90)', 'p(95)', 'p(99)'],
  scenarios: {
    warmup: {
      executor: 'ramping-vus',
      startVUs: 0,
      stages: [{ duration: WARMUP, target: VUS }],
      // A POST can commit after k6 abandons its socket. Let warmup requests
      // finish so the run-scoped committed-row check compares acknowledgements
      // to writes rather than counting client-aborted commits as data loss.
      gracefulRampDown: WARMUP_DRAIN,
      gracefulStop: '0s',
      startTime: '0s',
      tags: { stage: 'warmup' },
      exec: 'write',
    },
    measure: {
      executor: 'constant-vus',
      vus: VUS,
      duration: MEASURE,
      startTime: MEASURE_START,
      // Do not cancel a POST while its server-side transaction can still
      // commit. A long grace period normally costs nothing (idle VUs exit
      // immediately) and turns an overloaded shutdown into a timeout instead
      // of a false commit/ack mismatch.
      gracefulStop: MEASURE_GRACEFUL_STOP,
      tags: { stage: 'measure' },
      exec: 'write',
    },
  },
  thresholds: {
    'http_req_duration{stage:measure}': ['p(99)>=0'],
    'http_reqs{stage:measure}': ['count>=0'],
    'checks{stage:measure}': ['rate>=0'],
  },
};

export function write() {
  // Unique across VUs and iterations within a run (fresh DB per server boot).
  // __ITER is scoped to a k6 scenario, so include warmup/measure in the key;
  // otherwise a measure iteration can collide with a warmup insert.
  const phase = exec.scenario.name === 'measure' ? 1 : 0;
  const id = phase * 1_000_000_000_000 + __VU * 1_000_000_000 + __ITER;
  const prefix = __ENV.WRITE_PREFIX || 'unscoped';
  const note = `${prefix}-${id}`;
  const res = http.post(`${BASE_URL}/write`, JSON.stringify({ id, note }), {
    headers: { 'Content-Type': 'application/json' },
    tags: { name: 'POST /write' },
  });
  let shapeOk = false;
  try {
    const body = res.json();
    shapeOk =
      Array.isArray(body) &&
      body.length === 1 &&
      body[0] &&
      Number(body[0].id) === id;
  } catch (_) {
    shapeOk = false;
  }
  const statusOk = res.status >= 200 && res.status < 300;
  if (phase === 1 && statusOk && shapeOk) {
    successfulMeasuredWrites.add(1);
  }
  check(res, {
    'status 2xx': () => statusOk,
    'body shape [{id}]': () => shapeOk,
    'request contract': () => statusOk && shapeOk,
  });
}
