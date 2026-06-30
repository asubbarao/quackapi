.read client_fanout.sql
.timer on
SELECT http_fanout(18099, 8, 8)      AS smoke_8x8;
SELECT http_fanout(18099, 4096, 1)   AS serial_1_thread;
SELECT http_fanout(18099, 4096, 64)  AS parallel_64_threads;
