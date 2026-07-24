// Typed path param: GET /items/:id (random int each iteration)
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
      exec: 'item',
    },
    measure: {
      executor: 'constant-vus',
      vus: VUS,
      duration: MEASURE,
      startTime: WARMUP,
      gracefulStop: '2s',
      tags: { stage: 'measure' },
      exec: 'item',
    },
  },
  thresholds: {
    'http_req_duration{stage:measure}': ['p(99)>=0'],
    'http_reqs{stage:measure}': ['count>=0'],
    'checks{stage:measure}': ['rate>=0'],
  },
};

export function item() {
  // bench_rows has ids 0..99999 — stay in range so this is a real lookup hit,
  // not a near-universal miss on an id that was never seeded.
  const id = Math.floor(Math.random() * 100_000);
  // name tag collapses high-cardinality /items/<id> URLs into one series.
  const res = http.get(`${BASE_URL}/items/${id}`, {
    tags: { name: 'GET /items/:id' },
  });
  // Shape only: stacks share keys + typed id; names come from live pgEdge
  // bench_rows (not a synthetic "item-<id>" string).
  let shapeOk = false;
  try {
    const body = res.json();
    shapeOk =
      Array.isArray(body) &&
      body.length === 1 &&
      body[0] &&
      Number.isInteger(Number(body[0].id)) &&
      Number(body[0].id) === id &&
      body[0].name !== undefined &&
      body[0].name !== null;
  } catch (_) {
    shapeOk = false;
  }
  check(res, {
    'status 2xx': (r) => r.status >= 200 && r.status < 300,
    'body shape [{id,name}]': () => shapeOk,
  });
}
