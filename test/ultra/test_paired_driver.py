import unittest

from paired_driver import evaluate, fuzz_cases, normalize_json


class PairedDriverTests(unittest.TestCase):
    def test_quack_single_row_envelope_normalizes_to_object(self):
        self.assertEqual(normalize_json([{"id": 1}], "json_object"), {"id": 1})

    def test_validation_requires_fastapi_shape(self):
        case = {"expect_status": 422, "semantic": "validation", "loc0": "body", "loc1": "name"}
        result = {
            "status": 422,
            "headers": {},
            "body": b'{"detail":[{"loc":["body","name"],"msg":"required","type":"missing"}]}',
        }
        self.assertEqual(evaluate(case, result), [])

    def test_validation_rejects_missing_detail(self):
        case = {"expect_status": 422, "semantic": "validation", "loc0": "body"}
        result = {"status": 422, "headers": {}, "body": b'{"error":"bad"}'}
        self.assertTrue(evaluate(case, result))

    def test_compressed_payload_is_checked_after_decode(self):
        case = {
            "expect_status": 200,
            "semantic": "compressed",
            "content_encoding": "gzip",
            "expect": {"payload_prefix": "x"},
        }
        result = {"status": 200, "headers": {"Content-Encoding": "gzip"}, "body": b'{"payload":"xxx"}'}
        self.assertEqual(evaluate(case, result), [])

    def test_fuzz_cases_have_unique_ids(self):
        cases = fuzz_cases()
        self.assertEqual(len(cases), len({case["id"] for case in cases}))
        self.assertGreaterEqual(len(cases), 10)


if __name__ == "__main__":
    unittest.main()
