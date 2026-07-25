import http from 'k6/http';
import { check } from 'k6';
const BASE_URL = __ENV.BASE_URL;
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
  check(res, { 'status 200': (r) => r.status === 200, 'n ok': (r) => { try { return r.json().n == Number(N); } catch { return false; } } });
}
