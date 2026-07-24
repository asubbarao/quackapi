// Concurrent append: POST /write with unique id per iteration (__VU / __ITER).
import http from 'k6/http';
import { check } from 'k6';

const BASE_URL = __ENV.BASE_URL;
if (!BASE_URL) {
  throw new Error('BASE_URL is required (e.g. http://127.0.0.1:8000)');
}

const VUS = Number(__ENV.VUS || 32);
const WARMUP = __ENV.WARMUP_DURATION || '5s';
const MEASURE = __ENV.MEASURE_DURATION || '20s';

export const options = {
  discardResponseBodies: false,
  summaryTrendStats: ['avg', 'min', 'med', 'max', 'p(90)', 'p(95)', 'p(99)'],
  scenarios: {
    warmup: {
      executor: 'ramping-vus',
      startVUs: 0,
      stages: [{ duration: WARMUP, target: VUS }],
      gracefulRampDown: '0s',
      gracefulStop: '0s',
      startTime: '0s',
      tags: { stage: 'warmup' },
      exec: 'write',
    },
    measure: {
      executor: 'constant-vus',
      vus: VUS,
      duration: MEASURE,
      startTime: WARMUP,
      gracefulStop: '2s',
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
  const id = __VU * 1_000_000_000 + __ITER;
  const note = `note-${id}-${Math.random().toString(36).slice(2, 10)}`;
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
  check(res, {
    'status 2xx': (r) => r.status >= 200 && r.status < 300,
    'body shape [{id}]': () => shapeOk,
  });
}
