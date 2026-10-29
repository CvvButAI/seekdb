#!/usr/bin/env python3
"""Start a compiled seekdb plus local mock and run the SQL tests in a fresh directory."""
import argparse
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time
import unittest
from mock_ai import make_server


def free_port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--work-dir', required=True, type=Path,
                        help='A NEW directory for disposable database files and retained logs')
    parser.add_argument('--timeout', default=300, type=int)
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error('binary does not exist')
    try:
        import pymysql
    except ImportError:
        parser.error('PyMySQL is required for SQL integration')
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    mock = make_server(port=0)
    thread = threading.Thread(target=mock.serve_forever, daemon=True)
    thread.start()
    port = free_port()
    os.environ['SEEKDB_TEST_HOST'] = '127.0.0.1'
    os.environ['SEEKDB_TEST_PORT'] = str(port)
    os.environ['SEEKDB_TEST_MOCK'] = 'http://127.0.0.1:' + str(mock.server_port)
    environment = os.environ.copy()
    environment['NO_PROXY'] = '127.0.0.1,localhost,0.0.0.0'
    environment['no_proxy'] = environment['NO_PROXY']
    process = None
    try:
        with (work / 'seekdb.log').open('wb') as log:
            process = subprocess.Popen([str(binary), '--nodaemon', '--base-dir=' + str(work / 'database'),
                                        '--port=' + str(port)], stdout=log, stderr=subprocess.STDOUT, env=environment)
            deadline = time.monotonic() + args.timeout
            while True:
                if process.poll() is not None:
                    raise RuntimeError('seekdb exited; inspect ' + str(work / 'seekdb.log'))
                try:
                    connection = pymysql.connect(host='127.0.0.1', port=port,
                        user=os.environ.get('SEEKDB_TEST_USER', 'root'),
                        password=os.environ.get('SEEKDB_TEST_PASSWORD', ''), connect_timeout=2)
                    with connection:
                        with connection.cursor() as cursor:
                            cursor.execute('SELECT 1')
                    break
                except pymysql.Error:
                    if time.monotonic() >= deadline:
                        raise RuntimeError('seekdb did not become ready; inspect ' + str(work / 'seekdb.log'))
                    time.sleep(0.5)
            suite = unittest.defaultTestLoader.discover(str(Path(__file__).parent), pattern='test_sql.py')
            result = unittest.TextTestRunner(verbosity=2).run(suite)
            return 0 if result.wasSuccessful() and not result.skipped else 1
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        mock.shutdown()
        thread.join()
        mock.server_close()

if __name__ == '__main__':
    sys.exit(main())
