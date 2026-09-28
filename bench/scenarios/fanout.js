import http from 'k6/http';
import { check } from 'k6';
const BASE_URL = __ENV.BASE_URL;
if (!BASE_URL) {
  throw new Error('BASE_URL is required (e.g. http://127.0.0.1:8000)');
}
const VUS = Number(__ENV.VUS || 16);
const N = __ENV.N || '10';
const MS = __ENV.MS || '50';
const WARMUP = __ENV.WARMUP_DURATION || '2s';
const MEASURE = __ENV.MEASURE_DURATION || '8s';
export const options = {
  discardResponseBodies: false,
  scenarios: {
    warmup:  { executor: 'constant-vus', vus: VUS, duration: WARMUP, startTime: '0s', tags: { stage: 'warmup' }, exec: 'fan' },
    measure: { executor: 'constant-vus', vus: VUS, duration: MEASURE, startTime: WARMUP, tags: { stage: 'measure' }, exec: 'fan' },
  },
};
export function fan() {
  const res = http.get(`${BASE_URL}/fanout?n=${N}&ms=${MS}`);
  let body;
  try { body = res.json(); } catch (_) { body = null; }
  const statusOk = res.status === 200;
  const shapeOk = body && body.n === Number(N) && body.sum_ids === (Number(N) * (Number(N) - 1)) / 2;
  check(res, {
    'status 200': () => statusOk,
    'fanout count and aggregate': () => Boolean(shapeOk),
    'request contract': () => statusOk && Boolean(shapeOk),
  });
}
