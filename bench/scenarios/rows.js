// Serialization: GET /rows?n=<N> — body must be fully read (discardResponseBodies OFF).
import http from 'k6/http';
import { check } from 'k6';

const BASE_URL = __ENV.BASE_URL;
if (!BASE_URL) {
  throw new Error('BASE_URL is required (e.g. http://127.0.0.1:8000)');
}

const VUS = Number(__ENV.VUS || 32);
const ROWS_N = Number(__ENV.ROWS_N || 1000);
const WARMUP = __ENV.WARMUP_DURATION || '5s';
const MEASURE = __ENV.MEASURE_DURATION || '20s';

export const options = {
  // Measuring serialization: k6 must read and retain the full response body.
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
      exec: 'rows',
    },
    measure: {
      executor: 'constant-vus',
      vus: VUS,
      duration: MEASURE,
      startTime: WARMUP,
      gracefulStop: '2s',
      tags: { stage: 'measure' },
      exec: 'rows',
    },
  },
  thresholds: {
    'http_req_duration{stage:measure}': ['p(99)>=0'],
    'http_reqs{stage:measure}': ['count>=0'],
    'checks{stage:measure}': ['rate>=0'],
  },
};

export function rows() {
  const res = http.get(`${BASE_URL}/rows?n=${ROWS_N}`, {
    tags: { name: 'GET /rows' },
  });
  // Shape only (keys + id integer). Timestamp *format* differs across stacks
  // ("2026-01-01 00:00:00" vs "2026-01-01T00:00:00") — never byte-compare ts.
  let shapeOk = false;
  try {
    const body = res.json();
    const row0 = body && body[0];
    shapeOk =
      Array.isArray(body) &&
      body.length === ROWS_N &&
      row0 !== null &&
      typeof row0 === 'object' &&
      Number.isInteger(Number(row0.id)) &&
      row0.name !== undefined &&
      row0.value !== undefined &&
      row0.ts !== undefined &&
      row0.ts !== null;
  } catch (_) {
    shapeOk = false;
  }
  check(res, {
    'status 2xx': (r) => r.status >= 200 && r.status < 300,
    'body is n row objects': () => shapeOk,
  });
}
