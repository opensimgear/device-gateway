"""Private native-discovery paths cannot escape through namespace input."""
import io
from pathlib import Path
import unittest
from unittest.mock import patch

from acceptance import CDC, simulation_directory


class NamespaceTests(unittest.TestCase):
    @patch('acceptance.os.geteuid', return_value=123)
    def test_default_and_namespaced_paths(self, _uid):
        self.assertEqual(simulation_directory(), Path('/tmp/opensimgear-123/gateways'))
        self.assertEqual(simulation_directory('run_1.A-b'), Path('/tmp/opensimgear-123/gateways-run_1.A-b'))
        self.assertEqual(simulation_directory('..'), Path('/tmp/opensimgear-123/gateways-..'))
        client = CDC('00112233445566778899aabb', io.StringIO(), 'private')
        self.assertEqual(client.advertisement,
                         Path('/tmp/opensimgear-123/gateways-private/00112233445566778899aabb.json'))

    def test_invalid_and_boundary_names(self):
        for namespace in ('', 'a' * 32, '/tmp', '../escape', 'with space', 'ä', '\x00'):
            with self.subTest(namespace=namespace), self.assertRaises(ValueError):
                simulation_directory(namespace)
        self.assertTrue(str(simulation_directory('a' * 31)).endswith('gateways-' + 'a' * 31))


if __name__ == '__main__':
    unittest.main()
