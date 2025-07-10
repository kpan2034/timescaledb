# This file and its contents are licensed under the Timescale License.
# Please see the included NOTICE for copyright information and
# LICENSE-TIMESCALE for a copy of the license.

#
# Setup prior to every permutation.
#
# We define a function 'cagg_bucket_count' to get the number of
# buckets in a continuous aggregate.  We use it to verify that there
# aren't any duplicate buckets/rows inserted into the materialization
# hypertable after concurrent refreshes. Duplicate buckets are
# possible since there is no unique constraint on the GROUP BY keys in
# the materialized hypertable.
#
setup
{
-- create a base table for sensor data
CREATE TABLE IF NOT EXISTS sensor_data (
    time TIMESTAMPTZ NOT NULL,
    sensor_id INTEGER NOT NULL,
    temperature DOUBLE PRECISION,
    humidity DOUBLE PRECISION,
    pressure DOUBLE PRECISION
);

SELECT create_hypertable('sensor_data', 'time',
    chunk_time_interval => INTERVAL '1 day'
);

INSERT INTO sensor_data
SELECT
    timestamp '2025-01-01' + (i * INTERVAL '5 minutes') AS time,
    (i % 5) + 1 AS sensor_id,
    15 + 15 * random() AS temperature,
    30 + 60 * random() AS humidity,
    980 + 40 * random() AS pressure
FROM generate_series(0, 8640) AS i;

CREATE MATERIALIZED VIEW sensor_hourly_avg
WITH (timescaledb.continuous) AS
SELECT
    time_bucket('1 hour', time) AS bucket,
    sensor_id,
    AVG(temperature) AS avg_temperature,
    AVG(humidity) AS avg_humidity,
    AVG(pressure) AS avg_pressure,
    COUNT(*) AS reading_count
FROM sensor_data
GROUP BY bucket, sensor_id
WITH NO DATA;

    CREATE OR REPLACE FUNCTION lock_cagg(cagg name) RETURNS void AS $$
    DECLARE
      mattable text;
    BEGIN
      SELECT format('%I.%I', user_view_schema, user_view_name)
      FROM _timescaledb_catalog.continuous_agg
      WHERE user_view_name = cagg
      INTO mattable;
      EXECUTE format('LOCK table %s IN ROW EXCLUSIVE MODE', mattable);
    END
    $$ LANGUAGE plpgsql;
}

setup
{
CALL refresh_continuous_aggregate('sensor_hourly_avg', NULL, NULL);
}

setup
{
BEGIN;
INSERT INTO sensor_data (time, sensor_id, temperature, humidity,pressure ) VALUES (now()-'1 day'::interval, 2, 20,30,1000);
COMMIT;
BEGIN;
INSERT INTO sensor_data (time, sensor_id, temperature, humidity,pressure ) VALUES (now()-'1 month'::interval, 2, 20,30,1000);
COMMIT;
}

teardown {
    DROP TABLE sensor_data CASCADE;
}

session "R1"
setup
{
    SET SESSION client_min_messages = 'DEBUG1';
}
step "R1_refresh"
{
    CALL refresh_continuous_aggregate('sensor_hourly_avg', '2 months'::interval, '1 week'::interval);
}

session "R2"
setup
{
    SET SESSION client_min_messages = 'DEBUG1';
}
step "R2_refresh"
{

    CALL refresh_continuous_aggregate('sensor_hourly_avg', '1 week'::interval, '1 hour'::interval);
}

session "L"
step debug_waitpoint_enable
{
    SELECT debug_waitpoint_enable('invalidation_process_hypertable_log');
}

step debug_waitpoint_release
{
    SELECT debug_waitpoint_release('invalidation_process_hypertable_log');
}

session "S"
step count_bucket
{
    SELECT count(bucket) FROM sensor_hourly_avg;
}

permutation "debug_waitpoint_enable" "R1_refresh" "R2_refresh" "debug_waitpoint_release" "count_bucket" "R2_refresh" "count_bucket"
