-- DuckLake + Quack on one host (no tailnet). Requires DuckDB 1.5+ with quack + ducklake from core_nightly.
--
-- Server session (terminal 1):
--   duckdb server.duckdb
--
-- Client session (terminal 2):
--   duckdb

-- === Server ===
INSTALL quack FROM core_nightly;
INSTALL ducklake FROM core_nightly;
LOAD quack;
LOAD ducklake;

ATTACH 'ducklake:./lake/metadata/inventory.ducklake' AS lake (DATA_PATH './lake/data/');
USE lake;

CREATE TABLE IF NOT EXISTS inventory (item_id INT, quantity INT);
INSERT INTO inventory VALUES (101, 50), (102, 120);

CALL quack_serve(
    'quack:127.0.0.1:9494',
    allow_other_hostname => true,
    token => 'quackscale-demo-token'
);

-- === Client (new duckdb process) ===
-- INSTALL quack FROM core_nightly;
-- INSTALL ducklake FROM core_nightly;
-- LOAD ducklake;
-- LOAD quack;
--
-- CREATE SECRET (TYPE quack, TOKEN 'quackscale-demo-token');
--
-- ATTACH 'ducklake:quack:127.0.0.1:9494' AS lake (DATA_PATH './lake/data/');
-- USE lake;
--
-- SELECT * FROM inventory ORDER BY item_id;
-- INSERT INTO inventory VALUES (103, 75);
--
-- Time travel: snapshot after the server seed (version 2) before the client INSERT above.
-- SELECT * FROM inventory AT (VERSION => 2);
