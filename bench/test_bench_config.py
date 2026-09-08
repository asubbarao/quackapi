"""Pure configuration regression checks; no database connections are opened."""

import os
import unittest
from unittest.mock import patch

from bench_config import pg_dsn


class ConnectionConfigurationTests(unittest.TestCase):
    def test_defaults(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(
                pg_dsn(), "postgresql://admin:password@127.0.0.1:6432/quackbench"
            )

    def test_explicit_dsn_wins_over_component_settings(self):
        expected = "host=/tmp/isolated-db dbname=bench user=test"
        with patch.dict(os.environ, {"PG_DSN": expected, "PG_PORT": "1234"}, clear=True):
            self.assertEqual(pg_dsn(), expected)

    def test_components_escape_url_reserved_characters_and_ipv6(self):
        with patch.dict(os.environ, {
            "PG_HOST": "::1", "PG_PORT": "6543", "PG_USER": "test@user",
            "PGPASSWORD": "pass:/?#'", "PG_DB": "bench/name",
        }, clear=True):
            self.assertEqual(
                pg_dsn(),
                "postgresql://test%40user:pass%3A%2F%3F%23%27@[::1]:6543/bench%2Fname",
            )


if __name__ == "__main__":
    unittest.main()
