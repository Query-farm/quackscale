-- Hub: control plane + client on one process. Leave this session open.
-- ../../build/release/duckdb -unsigned
--
-- Peers need a reachable server_url. 127.0.0.1 is for two processes on this host.

LOAD quack;
LOAD quackscale;

SELECT linked FROM quackscale_status();

CALL quackscale_hub(
    hostname   => 'coord',
    listen     => '127.0.0.1:18080',
    server_url => 'http://127.0.0.1:18080',
    dns_listen => '',
    state_dir  => '/tmp/quackscale-hub-coord'
);

SELECT * FROM quackscale_status();
SELECT * FROM quackscale.preauth_keys;
SELECT * FROM quackscale.nodes;

-- Optional: serve Quack on the mesh (needs LOAD quack + QUACK_TAILNET_TOKEN).
CALL quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true, token => quack_token());
CALL tailscale_serve_local(port => 9494);
FROM quack_discover();
