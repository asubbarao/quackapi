.read /Users/aloksubbarao/quackapi/client_fanout.sql
.timer on
SELECT  1 AS threads, http_fanout(18099, 4096,  1) AS ok_200s;
SELECT  8 AS threads, http_fanout(18099, 4096,  8) AS ok_200s;
SELECT 16 AS threads, http_fanout(18099, 4096, 16) AS ok_200s;
SELECT 32 AS threads, http_fanout(18099, 4096, 32) AS ok_200s;
SELECT 64 AS threads, http_fanout(18099, 4096, 64) AS ok_200s;
