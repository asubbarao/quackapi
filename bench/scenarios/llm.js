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

export const options = {
  scenarios: {
    load: {
      executor: 'constant-vus',
      vus: VUS,
      duration: DUR,
      gracefulStop: '60s',
    },
  },
  // Ollama is the shared bottleneck; a slow response is the model, not a failure.
  thresholds: {},
};

const PROMPTS = [
  'Summarize the role of a write-ahead log in one sentence.',
  'What is the difference between a B-tree and an LSM tree?',
  'Explain vectorized query execution briefly.',
  'Why does Nagle interact badly with delayed ACK?',
  'Describe multi-master replication in one sentence.',
];

export default function () {
  const prompt = PROMPTS[Math.floor(Math.random() * PROMPTS.length)];
  const qs =
    `model=${encodeURIComponent(MODEL)}&prompt=${encodeURIComponent(prompt)}` +
    (API === 'ask' ? `&num_predict=${NUM_PREDICT}` : '');
  const url = `${BASE}/llm/${API}?${qs}`;

  const started = Date.now();
  const res = http.post(url, null, { timeout: '600s' });
  const e2e = Date.now() - started;

  const good = check(res, { 'status 200': (r) => r.status === 200 });

  let logical = false;
  if (good) {
    try {
      let body = res.json();
      // quackapi returns a row array; FastAPI returns a bare object.
      if (Array.isArray(body)) { body = body[0]; }
      if (API === 'embed') {
        logical = body && body.dims > 0;
      } else {
        logical = body && typeof body.response === 'string';
        const t = Number(body.ollama_total_ms || 0);
        if (t > 0) {
          ollamaTime.add(t);
          overhead.add(Math.max(0, e2e - t));
        }
        if (body.out_tokens) { outTokens.add(Number(body.out_tokens)); }
      }
    } catch (_e) {
      logical = false;
    }
  }
  ok.add(logical);
}
