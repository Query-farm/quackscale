-- Client only. Paste the hub's wbkey-… into authkey.
-- ../../build/release/duckdb -unsigned

LOAD quack;
LOAD quackscale;

CALL tailscale_up(
    hostname    => 'peer',
    control_url => 'http://127.0.0.1:18080',
    authkey     => 'wbkey-REPLACE-ME',
    state_dir   => '/tmp/quackscale-hub-peer',
    ephemeral   => true
);

FROM tailscale_status();

-- Optional: attach the hub's Quack listener (same QUACK_TAILNET_TOKEN).
CREATE SECRET (TYPE quack, TOKEN 'your-shared-token', SCOPE 'quack:coord.quackscale.local:9494');
ATTACH 'quack:coord.quackscale.local:9494' AS remote (TYPE quack, DISABLE_SSL true);

FROM remote.query('SELECT 42');

DETACH remote;
CALL tailscale_down();
