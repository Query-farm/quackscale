-- Fleet client. Paste a wbkey-… from the server's quackscale.preauth_keys.
-- ../../build/release/duckdb -unsigned
-- Another client: change hostname and state_dir (e.g. job-etl).

LOAD quack;
LOAD quackscale;

CALL tailscale_up(
    hostname    => 'analyst-1',
    control_url => 'http://127.0.0.1:18080',
    authkey     => 'wbkey-REPLACE-ME',
    state_dir   => '/tmp/quackscale-hub-analyst-1',
    ephemeral   => true
);

FROM tailscale_status();

CREATE SECRET (TYPE quack, TOKEN 'your-shared-token', SCOPE 'quack:analytics-hub.quackscale.local:9494');
ATTACH 'quack:analytics-hub.quackscale.local:9494' AS hub (TYPE quack, DISABLE_SSL true);

FROM hub.query('SELECT 42');

DETACH hub;
CALL tailscale_down();
