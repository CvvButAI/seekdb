#!/usr/bin/env python3
"""Local-only AI protocol fixture. Never used by the SQL implementation."""
import argparse
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.mode = 'normal'
        self.requests = []


def vector(text, dimension):
    digest = hashlib.sha256(text.encode('utf-8')).digest()
    return [round((digest[i % len(digest)] + i % 17) / 300.0, 8) for i in range(dimension)]


def make_server(host='127.0.0.1', port=8000):
    state = State()
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, value, status=200):
            payload = json.dumps(value, ensure_ascii=False, allow_nan=False).encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self):
            if self.path != '/_test/state':
                return self.reply({'error': 'not found'}, 404)
            with state.lock:
                self.reply({'mode': state.mode, 'requests': list(state.requests)})

        def do_POST(self):
            try:
                body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))))
                if not isinstance(body, dict):
                    raise ValueError('object required')
                if self.path == '/_test/control':
                    with state.lock:
                        state.mode = body.get('mode', 'normal')
                        state.requests.clear()
                    return self.reply({'ok': True})
                with state.lock:
                    mode = state.mode
                    state.requests.append({'path': self.path, 'body': body,
                                           'has_authorization': 'Authorization' in self.headers})
                if mode == 'http_error':
                    return self.reply({'error': 'injected'}, 400)
                if mode == 'wrong_root':
                    return self.reply([])
                if self.path in ('/compatible-mode/v1/embeddings', '/api/embed'):
                    inputs = body['input']
                    if isinstance(inputs, str):
                        inputs = [inputs]
                    dimension = body.get('dimensions', 768)
                    if type(dimension) is not int or not 0 < dimension <= 8192:
                        raise ValueError('invalid dimensions')
                    data = [{'index': i, 'embedding': vector(text, dimension)}
                            for i, text in enumerate(inputs)]
                    if mode == 'bad_value':
                        data[0]['embedding'][0] = 'not numeric'
                    elif mode == 'bad_dimension':
                        data[0]['embedding'].pop()
                    elif mode == 'duplicate':
                        data[-1]['index'] = data[0]['index']
                        if len(data) == 1:
                            data[0]['index'] = 1
                    elif mode == 'short':
                        data.pop()
                    if self.path == '/api/embed':
                        return self.reply({'embeddings': [item['embedding'] for item in data]})
                    if mode == 'reverse':
                        data.reverse()
                    return self.reply({'data': data})
                if self.path == '/compatible-mode/v1/chat/completions':
                    text = '\n'.join(message['content'] for message in body['messages'])
                    content = '' if mode == 'empty_complete' else 'mock: ' + text
                    return self.reply({'choices': [{'message': {'content': content}}]})
                if self.path == '/api/generate':
                    if body.get('stream') is not False:
                        return self.reply({'error': 'non-streaming request required'}, 400)
                    return self.reply({'response': '' if mode == 'empty_complete' else 'mock: ' + body['prompt'], 'done': True})
                if self.path == '/v1/rerank':
                    docs = body['documents']
                    data = [{'index': i, 'relevance_score': 0.5 if mode == 'ties'
                             else (1.0 if body['query'] in text else 0.0)} for i, text in enumerate(docs)]
                    data.reverse()  # Clients must sort; fixture does not assume model order.
                    if mode == 'duplicate':
                        data[-1]['index'] = data[0]['index'] if len(data) > 1 else len(data)
                    elif mode == 'short':
                        data.pop()
                    return self.reply({'results': data})
                return self.reply({'error': 'unknown endpoint'}, 404)
            except (ValueError, TypeError, KeyError, IndexError) as error:
                return self.reply({'error': str(error)}, 400)
    server = ThreadingHTTPServer((host, port), Handler)
    server.state = state
    return server

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8000)
    args = parser.parse_args()
    make_server(args.host, args.port).serve_forever()
