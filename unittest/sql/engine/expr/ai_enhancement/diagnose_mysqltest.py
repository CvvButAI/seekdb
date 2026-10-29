#!/usr/bin/env python3
"""Retain complete, native mysqltest output for ai_function_enhancement.

Run only on a disposable database: the original fixture drops fixed model names.
The recording target is a NEW local file, never an upstream expected result.
"""
import argparse
import array
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid

from mock_ai import make_server


CASE = 'ai_function_enhancement'
QUERY_START = re.compile(r'^(select|call|create|drop|insert|alter|update|delete|use|set)\b', re.I)


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def transcript_selects(text):
    """Read the SQL echoes and TSV results of this fixture, not arbitrary SQL.

    Its echoed statements end on a line ending in a semicolon; none contain
    multiline quoted literals. Keep output bytes/formatting significant.
    """
    records = []
    pending = []
    current = None
    for line in text.splitlines():
        if pending or QUERY_START.match(line.lstrip()):
            pending.append(line)
            if line.rstrip().endswith(';'):
                sql = '\n'.join(pending)
                current = {'sql': sql, 'output': []}
                records.append(current)
                pending = []
        elif current is not None:
            current['output'].append(line)
    return [item for item in records if re.match(r'^\s*select\b', item['sql'], re.I)]


def compare_selects(expected_text, actual_text):
    expected = transcript_selects(expected_text)
    actual = transcript_selects(actual_text) if actual_text is not None else []
    result = []
    for index in range(max(len(expected), len(actual))):
        exp = expected[index] if index < len(expected) else None
        act = actual[index] if index < len(actual) else None
        if exp is None:
            status = 'UNEXPECTED_SELECT'
        elif act is None:
            status = 'NOT_CAPTURED'
        elif exp['sql'] != act['sql']:
            status = 'QUERY_SEQUENCE_DIFFERS'
        elif exp['output'] != act['output']:
            status = 'DIFFERENT'
        else:
            status = 'MATCH'
        result.append({'select': index + 1, 'status': status,
                       'expected': exp, 'actual': act})
    return result


def write_comparison(work, expected, actual, metadata):
    expected_text = expected.read_text(encoding='utf-8')
    actual_text = actual.read_text(encoding='utf-8', errors='replace') if actual else None
    rows = compare_selects(expected_text, actual_text)
    write_json(work / 'select-results.json', rows)
    if actual:
        diff = ''.join(difflib.unified_diff(
            expected_text.splitlines(keepends=True), actual_text.splitlines(keepends=True),
            fromfile='expected.result', tofile=actual.name))
        (work / 'full.diff').write_text(diff, encoding='utf-8')
        metadata['byte_equal'] = expected.read_bytes() == actual.read_bytes()
        if not metadata['byte_equal']:
            exp_bytes, act_bytes = expected.read_bytes(), actual.read_bytes()
            offset = next((i for i, (a, b) in enumerate(zip(exp_bytes, act_bytes)) if a != b),
                          min(len(exp_bytes), len(act_bytes)))
            metadata['first_differing_byte'] = offset
        exp_lines, act_lines = expected_text.splitlines(), actual_text.splitlines()
        for index in range(max(len(exp_lines), len(act_lines))):
            exp = exp_lines[index] if index < len(exp_lines) else None
            act = act_lines[index] if index < len(act_lines) else None
            if exp != act:
                metadata['first_difference'] = {'line': index + 1, 'expected': exp, 'actual': act}
                break
    else:
        (work / 'full.diff').write_text('# NOT_CAPTURED: no actual SQL transcript is available.\n', encoding='utf-8')
    metadata['select_counts'] = {status: sum(row['status'] == status for row in rows)
                                 for status in sorted({row['status'] for row in rows})}
    write_json(work / 'summary.json', metadata)
    report = ['# AI 函数增强：完整本地诊断', '',
              '状态：' + metadata['status'], '',
              '原始 test 和预期 result 保持不变。实际值仅来自本地 mysqltest 输出；未执行的 SQL 标为 NOT_CAPTURED。', '',
              '使用包内协议 Mock；这份结果不等于平台正式评分。', '',
              'AI_PROMPT 的预期是 template/args JSON；模板在调用模型时渲染。', '',
              '完整命令、退出码、环境和文件指纹见 summary.json；完整差异见 full.diff。', '']
    if metadata.get('error'):
        report += ['```text', metadata['error'], '```', '']
    for row in rows:
        report += [f"## SELECT {row['select']}: {row['status']}", '']
        for label, key in [('预期', 'expected'), ('实际', 'actual')]:
            value = row[key]
            report += [label, '']
            if value is None:
                report += ['NOT_CAPTURED', '']
            else:
                report += ['```sql', value['sql'], '```', '', '```text',
                           '\n'.join(value['output']), '```', '']
    (work / 'report.md').write_text('\n'.join(report), encoding='utf-8')


def linux_interfaces():
    if not sys.platform.startswith('linux'):
        return None
    import fcntl
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as handle:
        buffer = array.array('B', b'\0' * 16384)
        config = struct.pack('iP', len(buffer), buffer.buffer_info()[0])
        length = struct.unpack('iP', fcntl.ioctl(handle, 0x8912, config))[0]
        stride = 40 if struct.calcsize('P') == 8 else 32
        interfaces = []
        for offset in range(0, length, stride):
            name = buffer.tobytes()[offset:offset + 16].split(b'\0', 1)[0]
            flags = fcntl.ioctl(handle, 0x8913, struct.pack('16sH14s', name, 0, b''))
            interfaces.append({'name': name.decode(),
                               'loopback': bool(struct.unpack('16sH14s', flags)[1] & 8)})
        return interfaces


def free_port():
    with socket.socket() as handle:
        handle.bind(('127.0.0.1', 0))
        return handle.getsockname()[1]


def git_value(repo, *args):
    completed = subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True)
    return completed.stdout.strip() if completed.returncode == 0 else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[5])
    parser.add_argument('--binary', type=Path, help='Default: REPO/build_release/src/observer/seekdb')
    parser.add_argument('--mysqltest', type=Path, help='Default: bundled obclient mysqltest')
    parser.add_argument('--work-dir', type=Path, required=True, help='Must be a NEW directory')
    parser.add_argument('--timeout', type=int, default=300)
    parser.add_argument('--existing-server', action='store_true',
                        help='Use a disposable local server; fixture drops fixed model names')
    parser.add_argument('--port', type=int, default=2881, help='For --existing-server only')
    parser.add_argument('--actual-file', type=Path,
                        help='Analyze an already captured mysqltest .result/.reject; do not start services')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    repo, work = args.repo.resolve(), args.work_dir.resolve()
    suite = repo / 'tools/deploy/mysql_test/test_suite/ai_function'
    source_test = suite / 't' / (CASE + '.test')
    source_result = suite / 'r/mysql' / (CASE + '.result')
    for source in (source_test, source_result):
        if not source.is_file():
            parser.error('Missing fixture: ' + str(source) + '; set --repo to the seekdb checkout')
    if args.actual_file and not args.actual_file.is_file():
        parser.error('--actual-file does not exist')
    work.mkdir(parents=True, exist_ok=False)
    fixture_dir = work / 'fixtures'
    fixture_dir.mkdir()
    test = fixture_dir / source_test.name
    expected = fixture_dir / source_result.name
    shutil.copyfile(source_test, test)
    shutil.copyfile(source_result, expected)
    metadata = {'status': 'NOT_RUN', 'repo_commit': git_value(repo, 'rev-parse', 'HEAD'),
                'branch': git_value(repo, 'branch', '--show-current'),
                'production_tree': git_value(repo, 'rev-parse', 'HEAD:src'),
                'fixture_sha256': {str(source): file_sha256(source)
                                   for source in (source_test, source_result)},
                'mock': 'bundled mock_ai.py on 0.0.0.0:8000',
                'record_exit': None, 'comparison_exit': None}
    actual = None
    process = mock = thread = server_log = None
    try:
        if args.actual_file:
            actual = work / 'actual.result'
            shutil.copyfile(args.actual_file, actual)
            metadata['status'] = 'ANALYZE_ONLY'
        else:
            mysqltest = (args.mysqltest or repo / 'deps/3rd/u01/obclient/bin/mysqltest').resolve()
            if not mysqltest.is_file():
                raise RuntimeError('mysqltest not found; supply --mysqltest /absolute/path/mysqltest')
            if not args.existing_server:
                metadata['interfaces'] = linux_interfaces()
                if metadata['interfaces'] is not None and not any(
                        not item['loopback'] for item in metadata['interfaces']):
                    metadata['status'] = 'BLOCKED_NETWORK'
                    raise RuntimeError('Only loopback is visible. seekdb UUID initialization requires a non-loopback interface. '
                                       'No SQL was run. Use a normal host/container network; this tool does not change interfaces.')
            binary = (args.binary or repo / 'build_release/src/observer/seekdb').resolve()
            if not args.existing_server and not binary.is_file():
                raise RuntimeError('seekdb binary does not exist: ' + str(binary))
            environment = os.environ.copy()
            environment['NO_PROXY'] = environment['no_proxy'] = '127.0.0.1,localhost,0.0.0.0'
            password = os.environ.get('SEEKDB_TEST_PASSWORD', '')
            environment['MYSQL_PWD'] = password
            mock = make_server('0.0.0.0', 8000)
            thread = threading.Thread(target=mock.serve_forever, daemon=True)
            thread.start()
            port = args.port if args.existing_server else free_port()
            if not args.existing_server:
                metadata['binary'] = str(binary)
                metadata['binary_sha256'] = file_sha256(binary)
                server_log = (work / 'seekdb-console.log').open('wb')
                process = subprocess.Popen([str(binary), '--nodaemon',
                    '--base-dir=' + str(work / 'database'), '--port=' + str(port)],
                    stdout=server_log, stderr=subprocess.STDOUT, env=environment)
            deadline = time.monotonic() + args.timeout
            user = os.environ.get('SEEKDB_TEST_USER', 'root')
            client = [str(mysqltest), '--no-defaults', '--protocol=tcp', '--host=127.0.0.1',
                      '--port=' + str(port), '--user=' + user, '--default-character-set=utf8mb4',
                      '--connect-timeout=2', '--max-connect-retries=1']
            ready_dir = work / 'readiness'
            ready_dir.mkdir()
            ready_test = ready_dir / 'ready.test'
            ready_test.write_text('SELECT 1;\n', encoding='utf-8')
            ready_command = client + ['--test-file=' + str(ready_test), '--record',
                '--result-file=' + str(ready_dir / 'ready.result'), '--logdir=' + str(ready_dir)]
            while True:
                if process is not None and process.poll() is not None:
                    raise RuntimeError('seekdb exited before SQL was ready; inspect seekdb-console.log and database/log/')
                with (ready_dir / 'mysqltest.stdout.log').open('ab') as output:
                    probe = subprocess.run(ready_command, cwd=repo / 'tools/deploy', env=environment,
                        stdout=output, stderr=subprocess.STDOUT, timeout=min(10, args.timeout))
                if probe.returncode == 0:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError('SQL readiness timed out; inspect readiness/mysqltest.stdout.log and database/log/')
                time.sleep(0.5)
            database = 'ai_diag_' + uuid.uuid4().hex[:10]
            prologue = work / 'setup.test'
            prologue.write_text('--disable_query_log\nCREATE DATABASE IF NOT EXISTS ' + database
                                + ';\nUSE ' + database + ';\n--enable_query_log\n', encoding='utf-8')
            metadata['database'] = database
            metadata['port'] = port
            for phase in ('record', 'compare'):
                logdir = work / phase
                logdir.mkdir()
                target = (work / 'actual.result') if phase == 'record' else expected
                command = client + ['--prologue=' + str(prologue),
                    '--tail-lines=1000000', '--logdir=' + str(logdir), '--test-file=' + str(test),
                    '--result-file=' + str(target)]
                if phase == 'record':
                    command.append('--record')
                metadata[phase + '_command'] = command
                with (logdir / 'mysqltest.stdout.log').open('wb') as output:
                    completed = subprocess.run(command, cwd=repo / 'tools/deploy', env=environment,
                        stdout=output, stderr=subprocess.STDOUT, timeout=args.timeout)
                metadata['record_exit' if phase == 'record' else 'comparison_exit'] = completed.returncode
                with mock.state.lock:
                    write_json(logdir / 'http-requests.json', list(mock.state.requests))
                    mock.state.requests.clear()
                if phase == 'record':
                    if target.is_file():
                        actual = target
                        metadata['actual_origin'] = 'recording pass'
                    else:
                        candidates = sorted(logdir.glob('*.reject')) + sorted(logdir.glob('*.log'))
                        candidates = [path for path in candidates if path.name != 'mysqltest.stdout.log']
                        if candidates:
                            actual = candidates[0]
                            metadata['actual_origin'] = 'partial recording transcript'
                elif completed.returncode:
                    # Prefer the output of the comparison that actually failed,
                    # rather than assuming the two executions were identical.
                    candidates = sorted(logdir.rglob('*.reject')) + sorted(fixture_dir.glob('*.reject'))
                    if candidates:
                        actual = candidates[0]
                        metadata['actual_origin'] = 'native comparison reject'
            metadata['status'] = ('PASS' if metadata['record_exit'] == metadata['comparison_exit'] == 0
                                  and actual is not None and actual.read_bytes() == expected.read_bytes()
                                  else 'FAIL')
    except Exception as error:
        if metadata['status'] == 'NOT_RUN':
            metadata['status'] = 'ERROR'
        metadata['error'] = type(error).__name__ + ': ' + str(error)
        if actual is None and (work / 'actual.result').is_file():
            actual = work / 'actual.result'
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if server_log is not None:
            server_log.close()
        if mock is not None:
            mock.shutdown()
            thread.join()
            mock.server_close()
        metadata['upstream_fixtures_unchanged'] = all(
            file_sha256(Path(source)) == digest for source, digest in metadata['fixture_sha256'].items())
        if not metadata['upstream_fixtures_unchanged']:
            metadata['status'] = 'FIXTURE_CHANGED'
        metadata['actual_file'] = str(actual) if actual is not None else None
        write_comparison(work, expected, actual, metadata)
    print(metadata['status'] + ': ' + str(work / 'report.md'))
    print('Full output/diff: ' + str(work))
    return 0 if metadata['status'] == 'PASS' or (
        metadata['status'] == 'ANALYZE_ONLY' and metadata.get('byte_equal')) else 1


if __name__ == '__main__':
    sys.exit(main())
