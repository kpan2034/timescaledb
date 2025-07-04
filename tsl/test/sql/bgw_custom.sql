-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

CREATE TABLE custom_log(job_id int, args jsonb, extra text, runner NAME DEFAULT CURRENT_ROLE);

CREATE OR REPLACE FUNCTION custom_func(jobid int, args jsonb) RETURNS VOID LANGUAGE SQL AS
$$
  INSERT INTO custom_log VALUES($1, $2, 'custom_func');
$$;

CREATE OR REPLACE FUNCTION custom_func_definer(jobid int, args jsonb) RETURNS VOID LANGUAGE SQL AS
$$
  INSERT INTO custom_log VALUES($1, $2, 'security definer');
$$ SECURITY DEFINER;

CREATE OR REPLACE PROCEDURE custom_proc(job_id int, args jsonb) LANGUAGE SQL AS
$$
  INSERT INTO custom_log VALUES($1, $2, 'custom_proc');
$$;

-- procedure with transaction handling
CREATE OR REPLACE PROCEDURE custom_proc2(job_id int, args jsonb) LANGUAGE PLPGSQL AS
$$
BEGIN
  INSERT INTO custom_log VALUES($1, $2, 'custom_proc2 1 COMMIT ' || (args->>'type'));
  COMMIT;
  INSERT INTO custom_log VALUES($1, $2, 'custom_proc2 2 ROLLBACK ' || (args->>'type'));
  ROLLBACK;
  INSERT INTO custom_log VALUES($1, $2, 'custom_proc2 3 COMMIT ' || (args->>'type'));
  COMMIT;
END
$$;

\set ON_ERROR_STOP 0
-- test bad input
SELECT add_job(NULL, '1h');
SELECT add_job(0, '1h');
-- this will return an error about Oid 4294967295
-- while regproc is unsigned int postgres has an implicit cast from int to regproc
SELECT add_job(-1, '1h');
SELECT add_job('invalid_func', '1h');
SELECT add_job('custom_func', NULL);
SELECT add_job('custom_func', 'invalid interval');
SELECT add_job('custom_func', '1h', job_name := 'this_is_a_really_really_really_long_application_name_to_overflow');
\set ON_ERROR_STOP 1

select '2000-01-01 00:00:00+00' as time_zero \gset

SELECT add_job('custom_func','1h', config:='{"type":"function"}'::jsonb, initial_start => :'time_zero'::TIMESTAMPTZ);
SELECT add_job('custom_proc','1h', config:='{"type":"procedure"}'::jsonb, initial_start => :'time_zero'::TIMESTAMPTZ);
SELECT add_job('custom_proc2','1h', config:= '{"type":"procedure"}'::jsonb, initial_start => :'time_zero'::TIMESTAMPTZ);

SELECT add_job('custom_func', '1h', config:='{"type":"function"}'::jsonb, initial_start => :'time_zero'::TIMESTAMPTZ);
SELECT add_job('custom_func_definer', '1h', config:='{"type":"function"}'::jsonb, initial_start => :'time_zero'::TIMESTAMPTZ, job_name := 'custom_job_name');

-- exclude internal jobs
SELECT * FROM timescaledb_information.jobs WHERE job_id >= 1000 ORDER BY 1;

SELECT count(*) FROM _timescaledb_config.bgw_job WHERE config->>'type' IN ('procedure', 'function');

\set ON_ERROR_STOP 0
-- test bad input
CALL run_job(NULL);
CALL run_job(-1);
\set ON_ERROR_STOP 1

CALL run_job(1001);
CALL run_job(1002);
CALL run_job(1003);
CALL run_job(1004);
CALL run_job(1005);

SELECT * FROM custom_log ORDER BY job_id, extra;

\set ON_ERROR_STOP 0
-- test bad input
SELECT delete_job(NULL);
SELECT delete_job(-1);
\set ON_ERROR_STOP 1

-- We keep job 1001 for some additional checks.
SELECT delete_job(1002);
SELECT delete_job(1003);
SELECT delete_job(1004);
SELECT delete_job(1005);

-- check jobs got removed
SELECT count(*) FROM timescaledb_information.jobs WHERE job_id >= 1002;

\c :TEST_DBNAME :ROLE_SUPERUSER

-- create a new job with longer id
SELECT nextval('_timescaledb_config.bgw_job_id_seq') as nextval \gset
SELECT setval('_timescaledb_config.bgw_job_id_seq', 2147483647, false);
SELECT add_job('custom_func', '1h', config:='{"type":"function"}'::jsonb, job_name := 'custom_job_name');

\set ON_ERROR_STOP 0
-- test bad input
SELECT alter_job(NULL, if_exists => false);
SELECT alter_job(-1, if_exists => false);
SELECT alter_job(1001, job_name => 'this_is_a_really_really_really_long_application_name_to_overflow');
SELECT alter_job(2147483647, job_name => 'this_is_a_really_really_really_long_application_name_to_overflow');
\set ON_ERROR_STOP 1
-- test bad input but don't fail
SELECT alter_job(NULL, if_exists => true);
SELECT alter_job(-1, if_exists => true);


-- test altering job with NULL config
SELECT job_id FROM alter_job(1001,scheduled:=false);
SELECT scheduled, config FROM timescaledb_information.jobs WHERE job_id = 1001;

-- test updating job settings
SELECT job_id FROM alter_job(1001,config:='{"test":"test"}');
SELECT scheduled, config FROM timescaledb_information.jobs WHERE job_id = 1001;
SELECT job_id FROM alter_job(1001,scheduled:=true);
SELECT scheduled, config FROM timescaledb_information.jobs WHERE job_id = 1001;
SELECT job_id FROM alter_job(1001,scheduled:=false);
SELECT scheduled, config FROM timescaledb_information.jobs WHERE job_id = 1001;

-- test updating the job name
SELECT job_id, application_name FROM alter_job(1001,job_name:='custom_name_2');
SELECT job_id, application_name FROM alter_job(2147483647,job_name:='short_name_to_fit');
SELECT application_name FROM timescaledb_information.jobs WHERE job_id >= 1001;

-- Done with jobs now, so remove it.
SELECT delete_job(1001);
SELECT delete_job(2147483647);

-- reset the sequence to its previous value
SELECT setval('_timescaledb_config.bgw_job_id_seq', :nextval, false);

--test for #2793
\c :TEST_DBNAME :ROLE_DEFAULT_PERM_USER
-- background workers are disabled, so the job will not run --
SELECT add_job( proc=>'custom_func',
     schedule_interval=>'1h', initial_start =>'2018-01-01 10:00:00-05') AS job_id_1 \gset

SELECT job_id, next_start, scheduled, schedule_interval
FROM timescaledb_information.jobs WHERE job_id > 1001;
\x
SELECT * FROM timescaledb_information.job_stats WHERE job_id > 1001;
\x

SELECT delete_job(:job_id_1);

-- tests for #3545
TRUNCATE custom_log;

-- Nested procedure call
CREATE OR REPLACE PROCEDURE custom_proc_nested(job_id int, args jsonb) LANGUAGE PLPGSQL AS
$$
BEGIN
  INSERT INTO custom_log VALUES($1, $2, 'custom_proc_nested 1 COMMIT');
  COMMIT;
  INSERT INTO custom_log VALUES($1, $2, 'custom_proc_nested 2 ROLLBACK');
  ROLLBACK;
  INSERT INTO custom_log VALUES($1, $2, 'custom_proc_nested 3 COMMIT');
  COMMIT;
END
$$;

CREATE OR REPLACE PROCEDURE custom_proc3(job_id int, args jsonb) LANGUAGE PLPGSQL AS
$$
BEGIN
    CALL custom_proc_nested(job_id, args);
END
$$;

CREATE OR REPLACE PROCEDURE custom_proc4(job_id int, args jsonb) LANGUAGE PLPGSQL AS
$$
BEGIN
    INSERT INTO custom_log VALUES($1, $2, 'custom_proc4 1 COMMIT');
    COMMIT;
    INSERT INTO custom_log VALUES($1, $2, 'custom_proc4 2 ROLLBACK');
    ROLLBACK;
    RAISE EXCEPTION 'forced exception';
    INSERT INTO custom_log VALUES($1, $2, 'custom_proc4 3 ABORT');
    COMMIT;
END
$$;

CREATE OR REPLACE PROCEDURE custom_proc5(job_id int, args jsonb) LANGUAGE PLPGSQL AS
$$
BEGIN
    CALL refresh_continuous_aggregate('conditions_summary_daily', '2021-08-01 00:00', '2021-08-31 00:00');
END
$$;

-- Remove any default jobs, e.g., telemetry
\c :TEST_DBNAME :ROLE_SUPERUSER
TRUNCATE _timescaledb_config.bgw_job RESTART IDENTITY CASCADE;

\c :TEST_DBNAME :ROLE_DEFAULT_PERM_USER

SELECT add_job('custom_proc2', '1h', config := '{"type":"procedure"}'::jsonb, initial_start := now()) AS job_id_1 \gset
SELECT add_job('custom_proc3', '1h', config := '{"type":"procedure"}'::jsonb, initial_start := now()) AS job_id_2 \gset

\c :TEST_DBNAME :ROLE_SUPERUSER
-- Start Background Workers
SELECT _timescaledb_functions.start_background_workers();

-- Wait for jobs
SELECT test.wait_for_job_to_run(:job_id_1, 1);
SELECT test.wait_for_job_to_run(:job_id_2, 1);

-- Check results
SELECT * FROM custom_log ORDER BY job_id, extra;

-- Delete previous jobs
SELECT delete_job(:job_id_1);
SELECT delete_job(:job_id_2);
TRUNCATE custom_log;

-- Forced Exception
SELECT add_job('custom_proc4', '1h', config := '{"type":"procedure"}'::jsonb, initial_start := now()) AS job_id_3 \gset
SELECT _timescaledb_functions.restart_background_workers();
SELECT test.wait_for_job_to_run(:job_id_3, 1);

-- Check results
SELECT * FROM custom_log ORDER BY job_id, extra;

-- Delete previous jobs
SELECT delete_job(:job_id_3);

CREATE TABLE conditions (
  time TIMESTAMP NOT NULL,
  location TEXT NOT NULL,
  location2 char(10) NOT NULL,
  temperature DOUBLE PRECISION NULL,
  humidity DOUBLE PRECISION NULL
) WITH (autovacuum_enabled = FALSE);

SELECT create_hypertable('conditions', 'time', chunk_time_interval := '15 days'::interval);

ALTER TABLE conditions
  SET (
    timescaledb.compress,
    timescaledb.compress_segmentby = 'location',
    timescaledb.compress_orderby = 'time'
);
INSERT INTO conditions
SELECT generate_series('2021-08-01 00:00'::timestamp, '2021-08-31 00:00'::timestamp, '1 day'), 'POR', 'klick', 55, 75;

-- Chunk compress stats
SELECT * FROM _timescaledb_internal.compressed_chunk_stats ORDER BY chunk_name;

-- Compression policy
SELECT add_compression_policy('conditions', interval '1 day') AS job_id_4 \gset
SELECT _timescaledb_functions.restart_background_workers();
SELECT test.wait_for_job_to_run(:job_id_4, 1);

-- Chunk compress stats
SELECT * FROM _timescaledb_internal.compressed_chunk_stats ORDER BY chunk_name;

--TEST compression job after inserting data into previously compressed chunk
INSERT INTO conditions
SELECT generate_series('2021-08-01 00:00'::timestamp, '2021-08-31 00:00'::timestamp, '1 day'), 'NYC', 'nycity', 40, 40;

SELECT id, table_name, status from _timescaledb_catalog.chunk
where hypertable_id = (select id from _timescaledb_catalog.hypertable
                       where table_name = 'conditions')
order by id;

--running job second time, wait for it to complete
select t.schedule_interval FROM alter_job(:job_id_4, next_start=> now() ) t;
SELECT _timescaledb_functions.restart_background_workers();
SELECT test.wait_for_job_to_run(:job_id_4, 2);

SELECT id, table_name, status from _timescaledb_catalog.chunk
where hypertable_id = (select id from _timescaledb_catalog.hypertable
                       where table_name = 'conditions')
order by id;

-- Drop the compression job
SELECT delete_job(:job_id_4);

-- Decompress chunks before create the cagg
SELECT decompress_chunk(c) FROM show_chunks('conditions') c;

-- TEST Continuous Aggregate job
CREATE MATERIALIZED VIEW conditions_summary_daily
WITH (timescaledb.continuous, timescaledb.materialized_only=false) AS
SELECT location,
   time_bucket(INTERVAL '1 day', time) AS bucket,
   AVG(temperature),
   MAX(temperature),
   MIN(temperature)
FROM conditions
GROUP BY location, bucket
WITH NO DATA;

-- Refresh Continous Aggregate by Job
SELECT add_job('custom_proc5', '1h', config := '{"type":"procedure"}'::jsonb, initial_start := now()) AS job_id_5 \gset
SELECT _timescaledb_functions.restart_background_workers();
SELECT test.wait_for_job_to_run(:job_id_5, 1);
SELECT count(*) FROM conditions_summary_daily;

-- TESTs for alter_job_set_hypertable_id API

SELECT _timescaledb_functions.alter_job_set_hypertable_id( :job_id_5, NULL);
SELECT id, proc_name, hypertable_id
FROM _timescaledb_config.bgw_job WHERE id = :job_id_5;

-- error case, try to associate with a PG relation
\set ON_ERROR_STOP 0
SELECT _timescaledb_functions.alter_job_set_hypertable_id( :job_id_5, 'custom_log');
\set ON_ERROR_STOP 1

-- TEST associate the cagg with the job
SELECT _timescaledb_functions.alter_job_set_hypertable_id( :job_id_5, 'conditions_summary_daily'::regclass);

SELECT id, proc_name, hypertable_id
FROM _timescaledb_config.bgw_job WHERE id = :job_id_5;

--verify that job is dropped when cagg is dropped
DROP MATERIALIZED VIEW conditions_summary_daily;

SELECT id, proc_name, hypertable_id
FROM _timescaledb_config.bgw_job WHERE id = :job_id_5;

-- Cleanup
DROP TABLE conditions;
DROP TABLE custom_log;

-- Stop Background Workers
SELECT _timescaledb_functions.stop_background_workers();

SELECT _timescaledb_functions.restart_background_workers();

\set ON_ERROR_STOP 0
-- add test for custom jobs with custom check functions
-- create the functions/procedures to be used as checking functions
CREATE OR REPLACE PROCEDURE test_config_check_proc(config jsonb)
LANGUAGE PLPGSQL
AS $$
DECLARE
  drop_after interval;
BEGIN
    SELECT jsonb_object_field_text (config, 'drop_after')::interval INTO STRICT drop_after;
    IF drop_after IS NULL THEN
        RAISE EXCEPTION 'Config must be not NULL and have drop_after';
    END IF ;
END
$$;

CREATE OR REPLACE FUNCTION test_config_check_func(config jsonb) RETURNS VOID
AS $$
DECLARE
  drop_after interval;
BEGIN
    IF config IS NULL THEN
        RETURN;
    END IF;
    SELECT jsonb_object_field_text (config, 'drop_after')::interval INTO STRICT drop_after;
    IF drop_after IS NULL THEN
        RAISE EXCEPTION 'Config can be NULL but must have drop_after if not';
    END IF ;
END
$$ LANGUAGE PLPGSQL;

-- step 2, create a procedure to run as a custom job
CREATE OR REPLACE PROCEDURE test_proc_with_check(job_id int, config jsonb)
LANGUAGE PLPGSQL
AS $$
BEGIN
  RAISE NOTICE 'Will only print this if config passes checks, my config is %', config;
END
$$;

-- step 3, add the job with the config check function passed as argument
-- test procedures, should get an unsupported error
select add_job('test_proc_with_check', '5 secs', config => '{}', check_config => 'test_config_check_proc'::regproc);

-- test functions
select add_job('test_proc_with_check', '5 secs', config => '{}', check_config => 'test_config_check_func'::regproc);
select add_job('test_proc_with_check', '5 secs', config => NULL, check_config => 'test_config_check_func'::regproc);
select add_job('test_proc_with_check', '5 secs', config => '{"drop_after": "chicken"}', check_config => 'test_config_check_func'::regproc);
select add_job('test_proc_with_check', '5 secs', config => '{"drop_after": "2 weeks"}', check_config => 'test_config_check_func'::regproc)
as job_with_func_check_id \gset


--- test alter_job
select alter_job(:job_with_func_check_id, config => '{"drop_after":"chicken"}');
select config from alter_job(:job_with_func_check_id, config => '{"drop_after":"5 years"}');


-- test that jobs with an incorrect check function signature will not be registered
-- these are all incorrect function signatures

CREATE OR REPLACE FUNCTION test_config_check_func_0args() RETURNS VOID
AS $$
BEGIN
    RAISE NOTICE 'I take no arguments and will validate anything you give me!';
END
$$ LANGUAGE PLPGSQL;

CREATE OR REPLACE FUNCTION test_config_check_func_2args(config jsonb, intarg int) RETURNS VOID
AS $$
BEGIN
    RAISE NOTICE 'I take two arguments (jsonb, int) and I should fail to run!';
END
$$ LANGUAGE PLPGSQL;

CREATE OR REPLACE FUNCTION test_config_check_func_intarg(config int) RETURNS VOID
AS $$
BEGIN
    RAISE NOTICE 'I take one argument which is an integer and I should fail to run!';
END
$$ LANGUAGE PLPGSQL;

-- -- this should fail, it has an incorrect check function
select add_job('test_proc_with_check', '5 secs', config => '{}', check_config => 'test_config_check_func_0args'::regproc);
-- -- so should this
select add_job('test_proc_with_check', '5 secs', config => '{}', check_config => 'test_config_check_func_2args'::regproc);
-- and this
select add_job('test_proc_with_check', '5 secs', config => '{}', check_config => 'test_config_check_func_intarg'::regproc);
-- and this fails as it calls a nonexistent function
select add_job('test_proc_with_check', '5 secs', config => '{}', check_config => 'test_nonexistent_check_func'::regproc);

-- when called with a valid check function and a NULL config no check should occur
CREATE OR REPLACE FUNCTION test_config_check_func(config jsonb) RETURNS VOID
AS $$
BEGIN
    RAISE NOTICE 'This message will get printed for both NULL and not NULL config';
END
$$ LANGUAGE PLPGSQL;

SET client_min_messages = NOTICE;
-- check done for both NULL and non-NULL config
select add_job('test_proc_with_check', '5 secs', config => NULL, check_config => 'test_config_check_func'::regproc);
-- check done
select add_job('test_proc_with_check', '5 secs', config => '{}', check_config => 'test_config_check_func'::regproc) as job_id \gset

-- check function not returning void
CREATE OR REPLACE FUNCTION test_config_check_func_returns_int(config jsonb) RETURNS INT
AS $$
BEGIN
    raise notice 'I print a message, and then I return least(1,2)';
    RETURN LEAST(1, 2);
END
$$ LANGUAGE PLPGSQL;
select add_job('test_proc_with_check', '5 secs', config => '{}', check_config => 'test_config_check_func_returns_int'::regproc,
initial_start => :'time_zero'::timestamptz) as job_id_int \gset

-- rename the check function and then call alter_job to register the new name
ALTER FUNCTION test_config_check_func RENAME TO renamed_func;
select job_id, schedule_interval, config, check_config from alter_job(:job_id, check_config => 'renamed_func'::regproc, schedule_interval => '1 hour');
-- run alter again, should get a config check
select job_id, schedule_interval, config, check_config from alter_job(:job_id, config => '{}');

-- drop the registered check function, verify that alter_job will work and print a warning that
-- the check is being skipped due to the check function missing
DROP FUNCTION test_config_check_func_returns_int;
select job_id, schedule_interval, config, check_config from alter_job(:job_id_int, config => '{"field":"value"}');

-- do not drop the current check function but register a new one
CREATE OR REPLACE FUNCTION substitute_check_func(config jsonb) RETURNS VOID
AS $$
BEGIN
    RAISE NOTICE 'This message is a substitute of the previously printed one';
END
$$ LANGUAGE PLPGSQL;
-- register the new check
select job_id, schedule_interval, config, check_config from alter_job(:job_id, check_config => 'substitute_check_func');
select job_id, schedule_interval, config, check_config from alter_job(:job_id, config => '{}');

RESET client_min_messages;

-- test an oid that doesn't exist
select add_job('test_proc_with_check', '5 secs', config => '{}', check_config => 17424217::regproc);

\c :TEST_DBNAME :ROLE_SUPERUSER
-- test a function with insufficient privileges
create schema test_schema;
create role user_noexec with login;
grant usage on schema test_schema to user_noexec;

CREATE OR REPLACE FUNCTION test_schema.test_config_check_func_privileges(config jsonb) RETURNS VOID
AS $$
BEGIN
    RAISE NOTICE 'This message will only get printed if privileges suffice';
END
$$ LANGUAGE PLPGSQL;

revoke execute on function test_schema.test_config_check_func_privileges from public;
-- verify the user doesn't have execute permissions on the function
select has_function_privilege('user_noexec', 'test_schema.test_config_check_func_privileges(jsonb)', 'execute');

\c :TEST_DBNAME user_noexec
-- user_noexec should not have exec permissions on this function
select add_job('test_proc_with_check', '5 secs', config => '{}', check_config => 'test_schema.test_config_check_func_privileges'::regproc);

\c :TEST_DBNAME :ROLE_SUPERUSER

-- check that alter_job rejects a check function with invalid signature
select add_job('test_proc_with_check', '5 secs', config => '{}', check_config => 'renamed_func',
initial_start => :'time_zero'::timestamptz) as job_id_alter \gset
select job_id, schedule_interval, config, check_config from alter_job(:job_id_alter, check_config => 'test_config_check_func_0args');
select job_id, schedule_interval, config, check_config from alter_job(:job_id_alter);
-- test that we can unregister the check function
select job_id, schedule_interval, config, check_config from alter_job(:job_id_alter, check_config => 0);
-- no message printed now
select job_id, schedule_interval, config, check_config from alter_job(:job_id_alter, config => '{}');

-- test the case where we have a background job that registers jobs with a check fn
CREATE OR REPLACE PROCEDURE add_scheduled_jobs_with_check(job_id int, config jsonb) LANGUAGE PLPGSQL AS
$$
BEGIN
    perform add_job('test_proc_with_check', schedule_interval => '10 secs', config => '{}', check_config => 'renamed_func');
END
$$;

select add_job('add_scheduled_jobs_with_check', schedule_interval => '1 hour') as last_job_id \gset
-- wait for enough time
SELECT _timescaledb_functions.restart_background_workers();
SELECT test.wait_for_job_to_run(:last_job_id, 1);
select total_runs, total_successes, last_run_status from timescaledb_information.job_stats where job_id = :last_job_id;

-- test coverage for alter_job
-- registering an invalid oid
select alter_job(:job_id_alter, check_config => 123456789::regproc);
-- registering a function with insufficient privileges
\c :TEST_DBNAME user_noexec
select * from add_job('test_proc_with_check', '5 secs', config => '{}') as job_id_owner \gset
select * from alter_job(:job_id_owner, check_config => 'test_schema.test_config_check_func_privileges'::regproc);

\c :TEST_DBNAME :ROLE_SUPERUSER
DROP SCHEMA test_schema CASCADE;

-- Delete all jobs with that owner before we can drop the user.
DELETE FROM _timescaledb_config.bgw_job WHERE owner = 'user_noexec'::regrole;
DROP ROLE user_noexec;

-- test with aggregate check proc
create function jsonb_add (j1 jsonb, j2 jsonb) returns jsonb
AS $$
BEGIN
    RETURN j1 || j2;
END
$$ LANGUAGE PLPGSQL;

CREATE AGGREGATE sum_jsb (jsonb)
(
    sfunc = jsonb_add,
    stype = jsonb,
    initcond = '{}'
);

-- for test coverage, check unsupported aggregate type
select add_job('test_proc_with_check', '5 secs', config => '{}', check_config => 'sum_jsb'::regproc);

-- Cleanup jobs
TRUNCATE _timescaledb_config.bgw_job CASCADE;

-- github issue 4610
CREATE TABLE sensor_data
(
    time timestamptz not null,
    sensor_id integer not null,
    cpu double precision null,
    temperature double precision null
);

SELECT FROM create_hypertable('sensor_data','time');
SELECT '2022-10-06 00:00:00+00' as start_date_sd \gset
INSERT INTO sensor_data
	SELECT
		time + (INTERVAL '1 minute' * random()) AS time,
		sensor_id,
		random() AS cpu,
		random()* 100 AS temperature
	FROM
		generate_series(:'start_date_sd'::timestamptz - INTERVAL '1 months', :'start_date_sd'::timestamptz - INTERVAL '1 week', INTERVAL '30 minute') AS g1(time),
		generate_series(1, 50, 1 ) AS g2(sensor_id)
	ORDER BY
		time;

-- enable compression
ALTER TABLE sensor_data SET (timescaledb.compress, timescaledb.compress_orderby = 'time DESC');

-- create new chunks
INSERT INTO sensor_data
	SELECT
		time + (INTERVAL '1 minute' * random()) AS time,
		sensor_id,
		random() AS cpu,
		random()* 100 AS temperature
	FROM
		generate_series(:'start_date_sd'::timestamptz - INTERVAL '2 months', :'start_date_sd'::timestamptz - INTERVAL '2 week', INTERVAL '60 minute') AS g1(time),
		generate_series(1, 30, 1 ) AS g2(sensor_id)
	ORDER BY
		time;

-- get the name of a new uncompressed chunk
SELECT chunk_name AS new_uncompressed_chunk_name
  FROM timescaledb_information.chunks
  WHERE hypertable_name = 'sensor_data' AND NOT is_compressed LIMIT 1 \gset

-- change compression status so that this chunk is skipped when policy is run
update _timescaledb_catalog.chunk set status=3 where table_name = :'new_uncompressed_chunk_name';

-- add new compression policy job
SELECT add_compression_policy('sensor_data', INTERVAL '1' minute) AS compressjob_id \gset
-- set recompress to true
SELECT alter_job(id,config:=jsonb_set(config,'{recompress}', 'true')) FROM _timescaledb_config.bgw_job WHERE id = :compressjob_id;

-- verify that there are other uncompressed new chunks that need to be compressed
SELECT count(*) > 1
  FROM timescaledb_information.chunks
  WHERE hypertable_name = 'sensor_data' AND NOT is_compressed;

-- disable notice/warning as the new_uncompressed_chunk_name
-- is dynamic and it will be printed in those messages.
SET client_min_messages TO ERROR;
CALL run_job(:compressjob_id);
SET client_min_messages TO NOTICE;

-- check compression status is not changed for the chunk whose status was manually updated
SELECT status FROM _timescaledb_catalog.chunk where table_name = :'new_uncompressed_chunk_name';

-- confirm all the other new chunks are now compressed despite
-- facing an error when trying to compress :'new_uncompressed_chunk_name'
SELECT count(*) = 0
  FROM timescaledb_information.chunks
  WHERE hypertable_name = 'sensor_data' AND NOT is_compressed;

-- cleanup
SELECT _timescaledb_functions.stop_background_workers();
DROP TABLE sensor_data;
SELECT _timescaledb_functions.restart_background_workers();

-- Github issue #5537
-- Proc that waits until the given job enters the expected state
CREATE OR REPLACE PROCEDURE wait_for_job_status(job_param_id INTEGER, expected_status TEXT, spins INTEGER=:TEST_SPINWAIT_ITERS)
LANGUAGE PLPGSQL AS $$
DECLARE
  jobstatus TEXT;
BEGIN
  FOR i in 1..spins
  LOOP
    SELECT job_status FROM timescaledb_information.job_stats WHERE job_id = job_param_id INTO jobstatus;
    IF jobstatus = expected_status THEN
      RETURN;
    END IF;
    PERFORM pg_sleep(0.1);
    ROLLBACK;
  END LOOP;
  RAISE EXCEPTION 'wait_for_job_status(%): timeout after % tries', job_param_id, spins;
END;
$$;

-- Proc that sleeps for 1m - to keep the test jobs in running state
CREATE OR REPLACE PROCEDURE proc_that_sleeps(job_id INT, config JSONB)
LANGUAGE PLPGSQL AS
$$
BEGIN
    PERFORM pg_sleep(60);
END
$$;

-- create new jobs and ensure that the second one gets scheduled
-- before the first one by adjusting the initial_start values
SELECT add_job('proc_that_sleeps', '1h', initial_start => now()::timestamptz + interval '2s') AS job_id_1 \gset
SELECT add_job('proc_that_sleeps', '1h', initial_start => now()::timestamptz - interval '2s') AS job_id_2 \gset

SELECT _timescaledb_functions.restart_background_workers();
-- wait for the jobs to start running job_2 will start running first
CALL wait_for_job_status(:job_id_2, 'Running');
CALL wait_for_job_status(:job_id_1, 'Running');

-- add a new job and wait for it to start
SELECT add_job('proc_that_sleeps', '1h') AS job_id_3 \gset
CALL wait_for_job_status(:job_id_3, 'Running');

-- verify that none of the jobs crashed
SELECT job_id, job_status, next_start,
       total_runs, total_successes, total_failures
  FROM timescaledb_information.job_stats
  WHERE job_id IN (:job_id_1, :job_id_2, :job_id_3)
  ORDER BY job_id;
SELECT job_id, err_message
  FROM timescaledb_information.job_errors
  WHERE job_id IN (:job_id_1, :job_id_2, :job_id_3);

-- cleanup
SELECT _timescaledb_functions.stop_background_workers();
CALL wait_for_job_status(:job_id_1, 'Scheduled');
CALL wait_for_job_status(:job_id_2, 'Scheduled');
CALL wait_for_job_status(:job_id_3, 'Scheduled');

SELECT delete_job(:job_id_1);
SELECT delete_job(:job_id_2);
SELECT delete_job(:job_id_3);

CREATE OR REPLACE FUNCTION ts_test_bgw_job_function_call_string(job_id INTEGER) RETURNS text
AS :MODULE_PATHNAME LANGUAGE C STABLE STRICT;

\set ON_ERROR_STOP 0
SELECT ts_test_bgw_job_function_call_string(999999);
\set ON_ERROR_STOP 1

SELECT add_job('custom_func', '1h') AS job_func \gset
SELECT add_job('custom_proc', '1h') AS job_proc \gset

SELECT ts_test_bgw_job_function_call_string(:job_func);
SELECT ts_test_bgw_job_function_call_string(:job_proc);

SELECT delete_job(:job_func);
SELECT delete_job(:job_proc);

SELECT add_job('custom_func', '1h', config => '{"type":"function"}'::jsonb) AS job_func \gset
SELECT add_job('custom_proc', '1h', config => '{"type":"procedure"}'::jsonb) AS job_proc \gset

SELECT ts_test_bgw_job_function_call_string(:job_func);
SELECT ts_test_bgw_job_function_call_string(:job_proc);

-- Remove the procedure and let's check it fallingback to PROKIND_FUNCTION
DROP PROCEDURE custom_proc(jobid int, args jsonb);
SELECT ts_test_bgw_job_function_call_string(:job_proc);

\set ON_ERROR_STOP 0
-- Mess with pg catalog to don't identify the PROKIND
BEGIN;
UPDATE pg_catalog.pg_proc SET prokind = 'X' WHERE oid = 'custom_func(int,jsonb)'::regprocedure;
SELECT ts_test_bgw_job_function_call_string(:job_func);
ROLLBACK;
\set ON_ERROR_STOP 1

SELECT delete_job(:job_func);
SELECT delete_job(:job_proc);
