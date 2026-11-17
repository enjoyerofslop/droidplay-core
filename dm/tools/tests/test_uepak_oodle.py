"""Unit tests for uepak.inline_needs_decoder."""
import unittest
import uepak


class UepakOodleTest(unittest.TestCase):
    def test_inline_needs_decoder_exists(self):
        self.assertTrue(callable(getattr(uepak, 'inline_needs_decoder', None)))


if __name__ == '__main__':
    unittest.main()
