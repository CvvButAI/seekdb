import json
import threading
import unittest
from urllib.request import Request, urlopen
from urllib.error import HTTPError
from mock_ai import make_server, vector

class MockProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = make_server(port=0)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.url = 'http://127.0.0.1:' + str(cls.server.server_port)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.thread.join()
        cls.server.server_close()

    def post(self, path, body):
        with urlopen(Request(self.url + path, json.dumps(body).encode(), {'Content-Type': 'application/json'})) as response:
            return json.load(response)

    def setUp(self):
        self.post('/_test/control', {'mode': 'normal'})

    def test_vectors_distinguish_inputs_and_honor_dimension(self):
        data = self.post('/compatible-mode/v1/embeddings', {'input': ['a', 'b'], 'dimensions': 3})['data']
        self.assertEqual([len(item['embedding']) for item in data], [3, 3])
        self.assertNotEqual(data[0]['embedding'], data[1]['embedding'])

    def test_reverse_preserves_indices(self):
        self.post('/_test/control', {'mode': 'reverse'})
        data = self.post('/compatible-mode/v1/embeddings', {'input': ['a', 'b'], 'dimensions': 7})['data']
        self.assertEqual([item['index'] for item in data], [1, 0])
        self.assertEqual(data[0]['embedding'], vector('b', 7))

    def test_ollama_uses_native_shape(self):
        data = self.post('/api/embed', {'input': ['a', 'b'], 'dimensions': 3})
        self.assertEqual(data['embeddings'], [vector('a', 3), vector('b', 3)])
        self.assertNotIn('data', data)

    def test_ollama_requires_non_streaming(self):
        with self.assertRaises(HTTPError):
            self.post('/api/generate', {'prompt': 'hello'})
        self.assertEqual(self.post('/api/generate', {'prompt': 'hello', 'stream': False})['response'], 'mock: hello')

    def test_request_metadata_is_recorded(self):
        self.post('/api/embed', {'input': ['a'], 'dimensions': 3})
        with urlopen(self.url + '/_test/state') as response:
            requests = json.load(response)['requests']
        self.assertEqual(requests[0]['body']['input'], ['a'])
        self.assertFalse(requests[0]['has_authorization'])

    def test_rerank_ties_return_unsorted_indices(self):
        self.post('/_test/control', {'mode': 'ties'})
        results = self.post('/v1/rerank', {'query': 'a', 'documents': ['a', 'b', 'c']})['results']
        self.assertEqual([item['index'] for item in results], [2, 1, 0])
        self.assertEqual([item['relevance_score'] for item in results], [0.5] * 3)

if __name__ == '__main__':
    unittest.main()
