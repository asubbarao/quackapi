-- probes/harbor_boot_9497.sql
-- Boot harbor loopback + keep the duckdb process alive (so listener thread lives).
-- After CALL, run a blocking UDF so the bg process does not exit.
INSTALL harbor FROM community; LOAD harbor;
INSTALL ducktinycc FROM community; LOAD ducktinycc;
SELECT ok, code FROM tcc_module(mode := 'quick_compile',
  source := 'int usleep(unsigned int); int block_forever(int x){ for(;;) usleep(1000000); return 0; }',
  symbol := 'block_forever', sql_name := 'block_forever',
  return_type := 'i32', arg_types := ['i32'], stability := 'volatile', library := 'c');
CALL harbor_serve(bind := '127.0.0.1', port := 9497, token := 'quackapi_edges_probe');
SELECT block_forever(0);
