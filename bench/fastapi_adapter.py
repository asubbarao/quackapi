from __future__ import annotations

import os
from pathlib import Path
import sys
import time

from fastapi import BackgroundTasks
from pydantic import BaseModel


source_parent = Path(os.environ["UPSTREAM_SOURCE_PARENT"])
sys.path.insert(0, str(source_parent))

from app_an_py310.main import app  # noqa: E402


class DurableJob(BaseModel):
    id: int
    delay_ms: int


def finish_job(job_id: int, delay_ms: int) -> None:
    time.sleep(delay_ms / 1000)
    path = Path(os.environ["FASTAPI_SURVIVORS"])
    with path.open("a") as output:
        output.write(f"{job_id}\n")
        output.flush()
        os.fsync(output.fileno())


@app.post("/bench/jobs", status_code=202)
async def enqueue_job(job: DurableJob, background_tasks: BackgroundTasks) -> dict[str, int]:
    background_tasks.add_task(finish_job, job.id, job.delay_ms)
    return {"id": job.id, "job_id": job.id}
