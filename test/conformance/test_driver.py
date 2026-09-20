"""The HTTP contract scorecard must never turn missing evidence into a pass."""

import unittest

from driver import classify, evaluate, parse_set_cookie, types_equivalent


def _resp(status, headers=None, body=b""):
    return {"status": status, "headers": headers or {}, "body": body}


class VerdictTests(unittest.TestCase):
    def test_skipped_stronger_case_is_not_a_pass(self):
        verdict, _, _ = evaluate(
            {"skip_run": True, "force_pass_stronger": True},
            _resp(200, {}, b"{}"),
            _resp(200, {}, b"{}"),
        )
        self.assertEqual(verdict, "N/A")

    def test_quackapi_transport_failure_fails(self):
        verdict, _, _ = evaluate(
            {"id": "x"}, _resp(0, {}, b"connection failed"), _resp(200)
        )
        self.assertEqual(verdict, "FAIL")

    def test_missing_reference_response_fails_not_matches(self):
        """A status==0 on the FastAPI side (reference unreachable for this
        request) must never be readable as equivalence."""
        verdict, notes, _ = evaluate(
            {"id": "x"}, _resp(200), _resp(0, {}, b"REQUEST_ERROR")
        )
        self.assertEqual(verdict, "FAIL")
        self.assertIn("FastAPI", notes)

    def test_status_divergence_fails_even_with_matching_bodies(self):
        verdict, _, failures = evaluate(
            {"id": "x"}, _resp(200, {}, b"null"), _resp(404, {}, b"null")
        )
        self.assertEqual(verdict, "FAIL")
        self.assertTrue(any("status" in f for f in failures))

    def test_body_empty_case_keeps_body_failure(self):
        verdict, _, failures = evaluate(
            {"id": "explicit_head", "expect_body_empty": True},
            _resp(200, {}, b"unexpected"),
            _resp(200, {}, b""),
        )
        self.assertEqual(verdict, "FAIL")
        self.assertTrue(failures)

    def test_json_body_mismatch_fails(self):
        verdict, _, failures = evaluate(
            {"id": "x"},
            _resp(200, {"Content-Type": "application/json"}, b'[{"a":1}]'),
            _resp(200, {"Content-Type": "application/json"}, b'[{"a":2}]'),
        )
        self.assertEqual(verdict, "FAIL")
        self.assertTrue(any("json body mismatch" in f for f in failures))

    def test_matching_json_bodies_pass(self):
        verdict, _, _ = evaluate(
            {"id": "x"},
            _resp(200, {"Content-Type": "application/json"}, b'[{"a":1}]'),
            _resp(200, {"Content-Type": "application/json"}, b'[{"a":1}]'),
        )
        self.assertEqual(verdict, "PASS")


class TypeEquivalenceTests(unittest.TestCase):
    def test_generic_type_error_matches_specific_pydantic_kinds(self):
        self.assertTrue(types_equivalent("type_error", "int_parsing"))
        self.assertTrue(types_equivalent("float_parsing", "type_error"))

    def test_generic_type_error_does_not_match_unrelated_kinds(self):
        self.assertFalse(types_equivalent("type_error", "missing"))
        self.assertFalse(types_equivalent("type_error", "less_than_equal"))

    def test_identical_types_match(self):
        self.assertTrue(types_equivalent("missing", "missing"))


class CookieParsingTests(unittest.TestCase):
    def test_extracts_name_value_and_attrs_without_regex(self):
        parsed = parse_set_cookie("session=sess-abc; Path=/; HttpOnly")
        self.assertEqual(parsed["name"], "session")
        self.assertEqual(parsed["value"], "sess-abc")
        self.assertEqual(parsed["attrs"], ["Path=/", "HttpOnly"])

    def test_missing_path_attribute_is_visible(self):
        parsed = parse_set_cookie("session=sess-abc")
        self.assertNotIn("Path=/", parsed["attrs"])


class ClassifyTests(unittest.TestCase):
    def test_fail_class_is_sourced_only_from_explicit_class_hint(self):
        """A real failure can only be recategorized by a human-set
        class_hint on the case — never inferred from notes text or the
        case id at runtime. That's the mechanism the old driver used to
        quietly recast a real FAIL as an accepted category; it must not
        exist here even implicitly."""
        self.assertEqual(classify({"class_hint": "STRONGER"}, "FAIL"), "STRONGER")
        self.assertEqual(
            classify(
                {"notes": "this text says STRONGER but sets no class_hint"}, "FAIL"
            ),
            "BUG",
        )
        self.assertEqual(
            classify({"id": "get_user_head_explicit", "notes": "not built"}, "FAIL"),
            "BUG",
        )

    def test_pass_is_match_unless_explicitly_stronger(self):
        self.assertEqual(classify({}, "PASS"), "MATCH")
        self.assertEqual(classify({"class_hint": "STRONGER"}, "PASS"), "STRONGER")

    def test_unrecognized_hint_falls_back_to_bug(self):
        self.assertEqual(classify({"class_hint": "NOT-A-REAL-CLASS"}, "FAIL"), "BUG")


if __name__ == "__main__":
    unittest.main()
