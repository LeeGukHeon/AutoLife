#!/usr/bin/env python3
import unittest
import urllib.request

from fetch_upbit_historical_candles import (
    bounded_exponential_backoff_ms,
    compute_sec_zero_throttle_sleep_ms,
    parse_remaining_req_sec,
    parse_retry_after_ms,
    strip_origin_header,
)


class FetchUpbitRatePolicyTest(unittest.TestCase):
    def test_parse_remaining_req_sec(self):
        header = "group=default; min=1800; sec=29"
        self.assertEqual(29, parse_remaining_req_sec(header))
        self.assertIsNone(parse_remaining_req_sec("group=default; min=1800"))
        self.assertIsNone(parse_remaining_req_sec("group=default; sec=abc"))

    def test_parse_retry_after_ms(self):
        self.assertEqual(0, parse_retry_after_ms(""))
        self.assertEqual(1500, parse_retry_after_ms("1.5"))
        self.assertEqual(0, parse_retry_after_ms("invalid"))

    def test_bounded_exponential_backoff_ms(self):
        self.assertEqual(600, bounded_exponential_backoff_ms(600, 0, 10000))
        self.assertEqual(1200, bounded_exponential_backoff_ms(600, 1, 10000))
        self.assertEqual(10000, bounded_exponential_backoff_ms(600, 10, 10000))

    def test_compute_sec_zero_throttle_sleep_ms(self):
        # next second boundary from 1700000000.250 => 750ms, plus jitter 17ms => 767ms
        sleep_ms = compute_sec_zero_throttle_sleep_ms(
            remaining_req_header="group=default; sec=0",
            jitter_max_ms=50,
            now_epoch_sec=1700000000.250,
            jitter_ms=17,
        )
        self.assertEqual(767, sleep_ms)
        self.assertEqual(
            0,
            compute_sec_zero_throttle_sleep_ms(
                remaining_req_header="group=default; sec=1",
                jitter_max_ms=50,
                now_epoch_sec=1700000000.250,
                jitter_ms=17,
            ),
        )

    def test_strip_origin_header(self):
        req = urllib.request.Request("https://api.upbit.com/v1/ticker?markets=KRW-BTC", method="GET")
        req.add_header("Origin", "https://example.com")
        removed = strip_origin_header(req)
        self.assertTrue(removed)
        for container in (req.headers, req.unredirected_hdrs):
            for key in list(container.keys()):
                self.assertNotEqual("origin", str(key).lower())


if __name__ == "__main__":
    unittest.main()
