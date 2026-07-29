// Routing ceiling: GET /hello
// Warmup (ramping) is tagged stage=warmup and excluded from reported measure metrics.
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
  // Tiny payload: discardResponseBodies reduces k6-side copy overhead on the ceiling test.
  discardResponseBodies: true,
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
      exec: 'hello',
    },
    measure: {
      executor: 'constant-vus',
      vus: VUS,
      duration: MEASURE,
      startTime: WARMUP,
      gracefulStop: '2s',
      tags: { stage: 'measure' },
      exec: 'hello',
    },
  },
  // Loose thresholds only to force measure-stage submetrics into --summary-export.
  // Always-true predicates so a slow/empty measure window does not abort the suite.
  thresholds: {
    'http_req_duration{stage:measure}': ['p(99)>=0'],
    'http_reqs{stage:measure}': ['count>=0'],
    'checks{stage:measure}': ['rate>=0'],
  },
};

export function hello() {
  // responseType text overrides discard so we can assert JSON shape on the ceiling path.
  const res = http.get(`${BASE_URL}/hello`, {
    responseType: 'text',
    tags: { name: 'GET /hello' },
  });
  let shapeOk = false;
  try {
    const body = res.json();
    shapeOk =
      Array.isArray(body) &&
      body.length === 1 &&
      body[0] &&
      body[0].msg === 'world';
  } catch (_) {
    shapeOk = false;
  }
  check(res, {
    'status 2xx': (r) => r.status >= 200 && r.status < 300,
    'body shape [{"msg":"world"}]': () => shapeOk,
  });
}
