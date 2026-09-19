"""The HTTP contract scorecard must never turn missing evidence into a pass."""

import unittest

from driver import evaluate


class VerdictTests(unittest.TestCase):
    def test_skipped_stronger_case_is_not_a_pass(self):
        verdict, _, _ = evaluate({"skip_run": True, "force_pass_stronger": True}, 200, {}, b"{}")
        self.assertEqual(verdict, "N/A")

    def test_transport_failure_fails_without_expected_status(self):
        verdict, _, _ = evaluate({"id": "openapi_json"}, 0, {}, b"connection failed")
        self.assertEqual(verdict, "FAIL")

    def test_redirect_special_case_keeps_header_failure(self):
        verdict, _, failures = evaluate(
            {"id": "health_trailing_slash", "expect_status": 307, "expect_header_present": "Location"},
            307,
            {},
            b"",
        )
        self.assertEqual(verdict, "FAIL")
        self.assertTrue(failures)

    def test_head_special_case_keeps_body_failure(self):
        verdict, _, _ = evaluate(
            {"id": "explicit_head", "method": "HEAD", "expect_body_empty": True}, 200, {}, b"unexpected"
        )
        self.assertEqual(verdict, "FAIL")


if __name__ == "__main__":
    unittest.main()
