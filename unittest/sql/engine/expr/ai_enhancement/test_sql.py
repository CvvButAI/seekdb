"""Run against the binary built from this patch; requires pymysql and a running mock.

SEEKDB_TEST_PORT is mandatory to opt into integration tests. The test creates only
its uniquely named database, model and endpoints and removes them in tearDownClass.
"""
import json
import os
import unittest
import uuid
from urllib.request import Request, urlopen
from mock_ai import vector

@unittest.skipUnless(os.environ.get('SEEKDB_TEST_PORT'), 'set SEEKDB_TEST_PORT for actual SQL integration')
class AIEnhancementSQLTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import pymysql
        cls.db = pymysql.connect(host=os.environ.get('SEEKDB_TEST_HOST', '127.0.0.1'),
            port=int(os.environ['SEEKDB_TEST_PORT']), user=os.environ.get('SEEKDB_TEST_USER', 'root'),
            password=os.environ.get('SEEKDB_TEST_PASSWORD', ''), charset='utf8mb4', autocommit=True)
        cls.prefix = 'ai_patch_' + uuid.uuid4().hex[:10]
        cls.mock = os.environ.get('SEEKDB_TEST_MOCK', 'http://127.0.0.1:8000')
        cls.created = []
        cls.query('CREATE DATABASE ' + cls.prefix)
        cls.query('USE ' + cls.prefix)
        try:
            for kind, model_type, provider, path in [
                ('embed', 'dense_embedding', 'openai', '/compatible-mode/v1/embeddings'),
                ('complete', 'completion', 'openai', '/compatible-mode/v1/chat/completions'),
                ('rerank', 'rerank', 'siliconflow', '/v1/rerank'),
                ('ollama', 'completion', 'oLlAmA', '/api/generate'),
                ('oembed', 'dense_embedding', 'OLLAMA', '/api/embed')]:
                name = cls.prefix + '_' + kind
                cls.query('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)', (name, json.dumps({'type': model_type, 'model_name': name})))
                cls.created.append((name, False))
                cls.query('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)', (name + '_ep', json.dumps({
                    'ai_model_name': name, 'scope': 'all', 'url': cls.mock + path,
                    'access_key': '' if kind in ('ollama', 'oembed') else 'test-only-key', 'provider': provider})))
                cls.created.append((name + '_ep', True))
        except Exception:
            cls.tearDownClass()
            raise

    @classmethod
    def query(cls, sql, args=None):
        with cls.db.cursor() as cursor:
            cursor.execute(sql, args)
            result = cursor.fetchall()
            while cursor.nextset():
                pass
            return result

    @classmethod
    def tearDownClass(cls):
        try:
            for name, endpoint in reversed(cls.created):
                try:
                    cls.query('CALL DBMS_AI_SERVICE.' + ('DROP_AI_MODEL_ENDPOINT' if endpoint else 'DROP_AI_MODEL') + '(%s)', (name,))
                except Exception:
                    pass
            cls.query('DROP DATABASE IF EXISTS ' + cls.prefix)
        finally:
            cls.db.close()

    def control(self, mode='normal'):
        request = Request(self.mock + '/_test/control', json.dumps({'mode': mode}).encode(), {'Content-Type': 'application/json'})
        with urlopen(request) as response:
            json.load(response)

    def requests(self):
        with urlopen(self.mock + '/_test/state') as response:
            return json.load(response)['requests']

    def setUp(self):
        self.control()

    def test_prompt_json_lob_and_repeated_long_argument(self):
        result = self.query("SELECT ai_prompt('{0}/{0}/{1}',json_array(1,true),json_object('k',2))")[0][0]
        self.assertEqual(json.loads(result), {'template': '{0}/{0}/{1}', 'args': [[1, True], {'k': 2}]})
        self.query("SELECT ai_complete(%s,ai_prompt('outer:{0}',ai_prompt('inner:{0}','你好')),NULL)", (self.prefix + '_complete',))
        self.assertEqual(self.requests()[-1]['body']['messages'][-1]['content'], 'outer:inner:你好')
        self.query('CREATE TEMPORARY TABLE lob_input(t LONGTEXT, a TEXT)')
        try:
            self.query('INSERT INTO lob_input VALUES(%s,%s)', ('{0}/{0}', '你好' * 10000))
            self.query('SELECT ai_complete(%s,ai_prompt(t,a),NULL) FROM lob_input', (self.prefix + '_complete',))
            self.assertEqual(self.requests()[-1]['body']['messages'][-1]['content'], '/'.join(['你好' * 10000] * 2))
        finally:
            self.query('DROP TABLE lob_input')

    def test_completion_null_config_and_native_ollama(self):
        self.query("SELECT ai_complete(%s, 'hello', NULL)", (self.prefix + '_complete',))
        self.query("SELECT ai_complete(%s, '你好', JSON_OBJECT('temperature',0.2,'max_tokens',16))", (self.prefix + '_complete',))
        body = self.requests()[-1]['body']
        self.assertEqual((body['temperature'], body['max_tokens']), (0.2, 16))
        self.query("SELECT ai_complete(%s,'hello')", (self.prefix + '_ollama',))
        self.assertFalse(self.requests()[-1]['has_authorization'])
        self.assertIs(self.requests()[-1]['body']['stream'], False)
        endpoint = self.prefix + '_ollama_ep'
        self.query('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)', (endpoint, '{"provider":"Ollama"}'))
        value = self.query('SELECT provider FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS WHERE endpoint_name=%s', (endpoint,))[0][0]
        self.assertEqual(value, 'Ollama')
        self.query("SELECT ai_complete(%s,'after alter')", (self.prefix + '_ollama',))
        self.assertFalse(self.requests()[-1]['has_authorization'])

    def test_endpoint_partial_updates_preserve_empty_and_nonempty_keys(self):
        endpoint = self.prefix + '_ollama_ep'
        for change in ({'provider': 'Ollama'}, {'access_key': ''},
                       {'provider': 'OLLAMA'}, {'provider': 'Ollama'}):
            with self.subTest(change=change):
                self.query('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)',
                           (endpoint, json.dumps(change)))
                null_key, key = self.query(
                    'SELECT access_key IS NULL,access_key FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS '
                    'WHERE endpoint_name=%s', (endpoint,))[0]
                self.assertEqual((null_key, key), (0, ''))
                self.query("SELECT ai_complete(%s,'after repeated alter')", (self.prefix + '_ollama',))
                request = self.requests()[-1]
                self.assertEqual(request['path'], '/api/generate')
                self.assertFalse(request['has_authorization'])

        endpoint = self.prefix + '_complete_ep'
        for change in ({'provider': 'OPENAI'},
                       {'url': self.mock + '/compatible-mode/v1/chat/completions'}):
            self.query('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)',
                       (endpoint, json.dumps(change)))
            key = self.query('SELECT access_key FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS '
                             'WHERE endpoint_name=%s', (endpoint,))[0][0]
            self.assertEqual(key, 'test-only-key')
            self.query("SELECT ai_complete(%s,'key preserved')", (self.prefix + '_complete',))
            self.assertTrue(self.requests()[-1]['has_authorization'])

    def test_embed_dimensions_and_native_shape(self):
        for kind in ('embed', 'oembed'):
            for dimension in (3, 7):
                value = self.query('SELECT ai_embed(%s,%s,%s)', (self.prefix + '_' + kind, 'alpha', dimension))[0][0]
                self.assertEqual(json.loads(value), vector('alpha', dimension))
        import pymysql
        for dimension in (0, -1, None, 1.5):
            with self.subTest(dimension=dimension), self.assertRaises(pymysql.Error):
                self.query('SELECT ai_embed(%s,%s,%s)', (self.prefix + '_embed', 'alpha', dimension))

    def test_embed_actual_batch_null_reverse_multibatch(self):
        self.query('CREATE TEMPORARY TABLE batch_input(id INT PRIMARY KEY, content TEXT)')
        try:
            for i in range(300):
                self.query('INSERT INTO batch_input VALUES(%s,%s)', (i, None if i % 7 == 0 else 'row-' + str(i)))
            self.control('reverse')
            rows = self.query('SELECT id,ai_embed(%s,content,3) FROM batch_input ORDER BY id', (self.prefix + '_embed',))
            for i, value in rows:
                self.assertEqual(None if value is None else json.loads(value), None if i % 7 == 0 else vector('row-' + str(i), 3))
            requests = self.requests()
            self.assertTrue(any(len(item['body']['input']) > 1 for item in requests), 'still sending single-row requests')
            self.assertEqual(sum(len(item['body']['input']) for item in requests), 300 - len(range(0, 300, 7)))
            self.control('reverse')
            rows = self.query('SELECT id,ai_embed(%s,content,3) FROM batch_input WHERE id>=270 ORDER BY id LIMIT 5', (self.prefix + '_embed',))
            self.assertEqual([row[0] for row in rows], list(range(270, 275)))
            for i, value in rows:
                self.assertEqual(None if value is None else json.loads(value), None if i % 7 == 0 else vector('row-' + str(i), 3))
            self.control()
            self.query('SELECT ai_embed(%s,content,3) FROM batch_input WHERE content IS NULL', (self.prefix + '_embed',))
            self.assertEqual(self.requests(), [])
        finally:
            self.query('DROP TABLE batch_input')

    def test_rerank_objects_41_rows_ties_use_original_order(self):
        docs = [{'id': str(i), 'text': 'document-' + str(i)} for i in range(41)]
        self.control('ties')
        value = self.query("SELECT ai_rerank(%s,'query',%s,'text')", (self.prefix + '_rerank', json.dumps(docs)))[0][0]
        self.assertEqual([item['id'] for item in json.loads(value)], [str(i) for i in range(41)])
        self.assertEqual([len(item['body']['documents']) for item in self.requests()], [20, 20, 1])
        self.control('ties')
        value = self.query("SELECT ai_rerank(%s,'query',%s,NULL)", (self.prefix + '_rerank', json.dumps([item['text'] for item in docs])))[0][0]
        self.assertEqual([item['index'] for item in json.loads(value)], list(range(41)))

    def test_malformed_responses_are_errors(self):
        import pymysql
        for mode in ('duplicate', 'short', 'bad_value', 'bad_dimension', 'wrong_root', 'http_error'):
            self.control(mode)
            with self.subTest(mode=mode), self.assertRaises(pymysql.Error):
                self.query("SELECT ai_embed(%s,'hello',3)", (self.prefix + '_embed',))
        for mode in ('duplicate', 'short'):
            self.control(mode)
            with self.subTest(rerank_mode=mode), self.assertRaises(pymysql.Error):
                self.query("SELECT ai_rerank(%s,'query','[\"a\",\"b\"]',NULL)", (self.prefix + '_rerank',))
        self.control()
        with self.assertRaises(pymysql.Error):
            self.query("SELECT ai_complete(%s,ai_prompt('{99999999999999999999999999999999}','x'))", (self.prefix + '_complete',))
        self.control('empty_complete')
        with self.assertRaises(pymysql.Error):
            self.query("SELECT ai_complete(%s,'hello')", (self.prefix + '_complete',))

if __name__ == '__main__':
    unittest.main()
