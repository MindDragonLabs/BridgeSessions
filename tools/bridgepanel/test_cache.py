"""Listing / mesh-tree cache."""
from __future__ import annotations

import os
import time
import unittest

from bridgepanel import cache


class TestListingCache(unittest.TestCase):
    def setUp(self):
        cache.clear_caches()
        os.environ["BRIDGEPANEL_LIST_CACHE_FRESH"] = "20"
        os.environ["BRIDGEPANEL_LIST_CACHE_STALE"] = "120"

    def tearDown(self):
        cache.clear_caches()
        os.environ.pop("BRIDGEPANEL_LIST_CACHE_FRESH", None)
        os.environ.pop("BRIDGEPANEL_LIST_CACHE_STALE", None)
        os.environ.pop("BRIDGEPANEL_TREE_CACHE_TTL", None)

    def test_fresh_then_stale(self):
        key = cache.cache_key("avir", "C", "")
        cache.listing_put(key, {"ok": True, "items": [{"name": "a"}], "count": 1})
        payload, state = cache.listing_get(key)
        self.assertEqual(state, "fresh")
        self.assertIsNotNone(payload)
        assert payload is not None
        self.assertEqual(payload["items"][0]["name"], "a")
        self.assertNotIn("cached", payload)

        os.environ["BRIDGEPANEL_LIST_CACHE_FRESH"] = "0.01"
        time.sleep(0.03)
        payload, state = cache.listing_get(key)
        self.assertEqual(state, "stale")
        self.assertIsNotNone(payload)
        assert payload is not None
        self.assertEqual(payload["count"], 1)

    def test_invalidate_one_root(self):
        cache.listing_put(cache.cache_key("h", "inbox", ""), {"ok": True, "items": []})
        cache.listing_put(cache.cache_key("h", "C", ""), {"ok": True, "items": []})
        cache.listing_invalidate("h", "inbox")
        _, inbox_state = cache.listing_get(cache.cache_key("h", "inbox", ""))
        _, c_state = cache.listing_get(cache.cache_key("h", "C", ""))
        self.assertEqual(inbox_state, "miss")
        self.assertEqual(c_state, "fresh")

    def test_tree_ttl(self):
        os.environ["BRIDGEPANEL_TREE_CACHE_TTL"] = "0.01"
        cache.tree_put({"node": "node-3", "peers": []})
        hit = cache.tree_get()
        self.assertIsNotNone(hit)
        assert hit is not None
        self.assertEqual(hit["node"], "node-3")
        time.sleep(0.03)
        self.assertIsNone(cache.tree_get())


if __name__ == "__main__":
    unittest.main()
