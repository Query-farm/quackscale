-- Fleet server: hub + Quack. Leave this session open.
-- ../../build/release/duckdb -unsigned
--
-- Clients need a reachable server_url. 127.0.0.1 is for processes on this host.

LOAD quack;
LOAD quackscale;

SELECT linked FROM quackscale_status();

CALL quackscale_hub(
    hostname   => 'analytics-hub',
    listen     => '127.0.0.1:18080',
    server_url => 'http://127.0.0.1:18080',
    dns_listen => '',
    state_dir  => '/tmp/quackscale-hub-coord'
);

SELECT * FROM quackscale_status();
SELECT * FROM quackscale.preauth_keys;
CALL quackscale_preauth(reusable => true);
SELECT * FROM quackscale.nodes;

CALL quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true, token => quack_token());
CALL tailscale_serve_local(port => 9494);
FROM quack_discover();
