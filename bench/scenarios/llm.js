// LLM-gateway scenario: inbound request -> ollama -> durable verbatim log -> respond.
//
// Both stacks expose the same two endpoints, so BASE is the only thing that changes.
//
// The metric that actually isolates the framework is `overhead_ms` = end-to-end
// latency minus ollama's OWN reported service time (`total_duration`, which ollama
// returns in its body). Raw end-to-end latency on a generate workload is dominated
// by the model and mostly measures ollama, not the gateway.
import http from 'k6/http';
import { check } from 'k6';
import { Trend, Rate, Counter } from 'k6/metrics';

const BASE = __ENV.BASE || 'http://127.0.0.1:8000';
const API = __ENV.API || 'embed'; // 'embed' | 'ask'
const MODEL = __ENV.MODEL || (API === 'embed' ? 'nomic-embed-text:latest' : 'llama3.2:3b');
const VUS = parseInt(__ENV.VUS || '8', 10);
const DUR = __ENV.DUR || '30s';
const NUM_PREDICT = parseInt(__ENV.NUM_PREDICT || '16', 10);

const overhead = new Trend('overhead_ms');       // gateway+DB cost, model excluded
const ollamaTime = new Trend('ollama_total_ms'); // upstream service time
const outTokens = new Counter('out_tokens');
const ok = new Rate('logical_success');
const timingInvalid = new Rate('timing_invalid');

// Without these the checks below are decoration: k6 exits 0 with every check
// failed. Latency is deliberately absent -- ollama is the shared bottleneck and
// a slow response is the model, not a failure -- but correctness is not.
const thresholds = {
  checks: ['rate==1'],
  logical_success: ['rate==1'],
  http_req_failed: ['rate==0'],
};
// timing_invalid only takes samples on the generation route.
if (API === 'ask') { thresholds.timing_invalid = ['rate==0']; }

export const options = {
  scenarios: {
    load: {
      executor: 'constant-vus',
      vus: VUS,
      duration: DUR,
      gracefulStop: '60s',
    },
  },
  thresholds,
};

const PROMPTS = [
  'Summarize the role of a write-ahead log in one sentence.',
  'What is the difference between a B-tree and an LSM tree?',
  'Explain vectorized query execution briefly.',
  'Why does Nagle interact badly with delayed ACK?',
  'Describe multi-master replication in one sentence.',
];

export default function () {
  const prompt = PROMPTS[(__VU + __ITER) % PROMPTS.length];
  const qs =
    `model=${encodeURIComponent(MODEL)}&prompt=${encodeURIComponent(prompt)}` +
    (API === 'ask' ? `&num_predict=${NUM_PREDICT}` : '');
  const url = `${BASE}/llm/${API}?${qs}`;

  const started = Date.now();
  const res = http.post(url, null, { timeout: '600s' });
  const e2e = Date.now() - started;

  const statusOk = res.status === 200;
  const good = check(res, { 'status 200': () => statusOk });

  let logical = false;
  if (good) {
    try {
      let body = res.json();
      // quackapi returns a row array; FastAPI returns a bare object.
      if (Array.isArray(body)) { body = body[0]; }
      if (API === 'embed') {
        logical = body && Number.isInteger(Number(body.dims)) && body.dims > 0;
      } else {
        logical = body && typeof body.response === 'string';
        const t = Number(body && body.ollama_total_ms);
        if (Number.isFinite(t) && t > 0) {
          timingInvalid.add(0);
          ollamaTime.add(t);
          // Keep the signed residual. Clamping a negative value hides clock,
          // serialization, or upstream timing inconsistencies.
          overhead.add(e2e - t);
        } else {
          timingInvalid.add(1);
          logical = false;
        }
        if (body && Number.isInteger(Number(body.out_tokens)) && Number(body.out_tokens) >= 0) {
          outTokens.add(Number(body.out_tokens));
        }
      }
    } catch (_e) {
      logical = false;
    }
  }
  ok.add(logical);
}
