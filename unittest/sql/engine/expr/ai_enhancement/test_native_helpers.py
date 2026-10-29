#!/usr/bin/env python3
"""Link real project objects and test AI helpers/HTTP without starting a database."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
from urllib.request import Request, urlopen
from mock_ai import make_server, vector

ROOT = Path(__file__).resolve().parents[5]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path,
                        help='Completed CMake build with compile_commands.json and the seekdb link command')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    entries = json.loads((build / 'compile_commands.json').read_text())
    entry = next(item for item in entries if 'src/sql/CMakeFiles/ob_sql.dir/' in item['command'])
    env = os.environ.copy()
    env['NO_PROXY'] = env['no_proxy'] = '127.0.0.1,localhost,0.0.0.0'
    with tempfile.TemporaryDirectory(prefix='seekdb-ai-native-') as directory:
        work = Path(directory)
        obj, binary = work / 'native.o', work / 'native'
        command = shlex.split(entry['command'])
        command[command.index('-o') + 1] = str(obj)
        command[-1] = str(Path(__file__).with_name('native_ai_helpers.cpp'))
        command.append('-Wno-error')
        subprocess.run(command, cwd=entry['directory'], env=env, check=True)
        command = shlex.split((build / 'src/observer/CMakeFiles/seekdb.dir/link.txt').read_text())
        command = [arg for arg in command if arg not in
                   ('CMakeFiles/ob_main.dir/main.cpp.o', 'CMakeFiles/ob_main.dir/ob_command_line_parser.cpp.o')]
        command.insert(1, str(obj))
        command[command.index('-o') + 1] = str(binary)
        subprocess.run(command, cwd=build / 'src/observer', env=env, check=True)
        subprocess.run([str(binary)], cwd=ROOT, env=env, check=True)
        server = make_server(port=0)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base_url = 'http://127.0.0.1:' + str(server.server_port)
        expected = json.dumps({name: vector(name,3) for name in ('alpha','beta')})
        try:
            for mode in ('normal', 'reverse', 'duplicate', 'short', 'bad_value', 'bad_dimension', 'wrong_root', 'http_error'):
                request = Request(base_url + '/_test/control',json.dumps({'mode':mode}).encode(),
                                  {'Content-Type':'application/json'})
                with urlopen(request) as response:
                    json.load(response)
                subprocess.run([str(binary), base_url, mode, expected, 'http'], cwd=ROOT, env=env,
                               check=True, timeout=90)
                with urlopen(base_url + '/_test/state') as response:
                    requests = json.load(response)['requests']
                assert requests and requests[0]['body']['input'] == ['alpha','beta']
                if mode in ('normal','reverse'):
                    assert len(requests) == 2
                    assert requests[0]['has_authorization']
                    assert requests[1]['path'] == '/api/generate'
                    assert not requests[1]['has_authorization']
                    assert requests[1]['body']['stream'] is False
                print('PASS native HTTP fixture evidence: ' + mode, flush=True)
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

if __name__ == '__main__':
    main()
