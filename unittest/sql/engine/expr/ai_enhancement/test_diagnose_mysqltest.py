"""Diagnostic-report tests only. These do not run seekdb or validate its SQL."""
import json
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

from diagnose_mysqltest import compare_selects, transcript_selects, write_comparison, main


EXPECTED = (Path(__file__).resolve().parents[5] /
    'tools/deploy/mysql_test/test_suite/ai_function/r/mysql/ai_function_enhancement.result')


class DiagnosticReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.expected = EXPECTED.read_text(encoding='utf-8')

    def test_original_fixture_has_13_selects_and_structured_prompt(self):
        rows = transcript_selects(self.expected)
        self.assertEqual(len(rows), 13)
        self.assertEqual(json.loads(rows[0]['output'][1]),
                         {'args': [[1, True], {'k': 2}], 'template': '{0}/{0}/{1}'})
        self.assertEqual(rows[6]['output'], ['id\tdimension', '1\t768', '2\tNULL', '3\t768'])
        self.assertEqual(rows[8]['output'], ['first_id\trerank_count', 'a\t2'])

    def test_wrong_ollama_case_is_isolated_to_select_11(self):
        actual = self.expected.replace('provider\nOllama\n', 'provider\nollama\n', 1)
        rows = compare_selects(self.expected, actual)
        self.assertEqual([row['select'] for row in rows if row['status'] != 'MATCH'], [11])
        self.assertEqual(rows[10]['actual']['output'], ['provider', 'ollama'])

    def test_earlier_embedding_mismatch_survives_correct_rerank(self):
        actual = self.expected.replace('embed_dimension\n768\n', 'embed_dimension\n384\n', 1)
        rows = compare_selects(self.expected, actual)
        self.assertEqual(rows[4]['status'], 'DIFFERENT')
        self.assertEqual(rows[7]['status'], 'MATCH')
        self.assertEqual(rows[8]['status'], 'MATCH')
        self.assertEqual(sum(row['status'] == 'DIFFERENT' for row in rows), 1)

    def test_truncated_output_never_marks_unexecuted_selects_as_passed(self):
        actual = self.expected.split("call DBMS_AI_SERVICE.DROP_AI_MODEL('enhance_ollama_complete');")[0]
        rows = compare_selects(self.expected, actual)
        self.assertEqual([row['status'] for row in rows[:9]], ['MATCH'] * 9)
        self.assertEqual([row['status'] for row in rows[9:]], ['NOT_CAPTURED'] * 4)

    def test_no_actual_output_is_not_a_test_pass(self):
        rows = compare_selects(self.expected, None)
        self.assertEqual(len(rows), 13)
        self.assertEqual({row['status'] for row in rows}, {'NOT_CAPTURED'})

    def test_report_preserves_complete_rows_diff_and_original_expected(self):
        original = EXPECTED.read_bytes()
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            actual = work / 'actual.result'
            actual.write_text(self.expected.replace('2\tNULL\n', '2\t768\n', 1), encoding='utf-8')
            metadata = {'status': 'REPORT_UNIT_TEST'}
            write_comparison(work, EXPECTED, actual, metadata)
            rows = json.loads((work / 'select-results.json').read_text())
            self.assertEqual(rows[6]['actual']['output'], ['id\tdimension', '1\t768', '2\t768', '3\t768'])
            self.assertIn('+2\t768', (work / 'full.diff').read_text())
            self.assertEqual(metadata['first_difference']['expected'], '2\tNULL')
            self.assertEqual(metadata['first_difference']['actual'], '2\t768')
            self.assertIn('SELECT 7: DIFFERENT', (work / 'report.md').read_text())
        self.assertEqual(EXPECTED.read_bytes(), original)

    def test_failed_native_comparison_uses_its_reject_and_retains_full_stdout(self):
        # This is a runner fixture, not a simulated database validation result.
        class FixtureServer:
            def __init__(self):
                self.done = threading.Event()
                self.state = type('State', (), {'lock': threading.Lock(), 'requests': []})()

            def serve_forever(self):
                self.done.wait()

            def shutdown(self):
                self.done.set()

            def server_close(self):
                pass

        expected_bytes = EXPECTED.read_bytes()
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder) / 'new'
            repo = EXPECTED.parents[7]
            command_line = ['diagnose_mysqltest.py', '--repo', str(repo), '--existing-server',
                            '--mysqltest', str(EXPECTED), '--work-dir', str(work)]

            def fixture_client(command, **kwargs):
                if command[0] == 'git':
                    return subprocess.CompletedProcess(command, 0, stdout='fixture\n')
                if '--record' in command:
                    target = Path(next(item.split('=', 1)[1] for item in command if item.startswith('--result-file=')))
                    target.write_bytes(expected_bytes)
                    return subprocess.CompletedProcess(command, 0)
                logdir = Path(next(item.split('=', 1)[1] for item in command if item.startswith('--logdir=')))
                (logdir / 'ai_function_enhancement.reject').write_text(
                    self.expected.replace('provider\nOllama\n', 'provider\nollama\n', 1), encoding='utf-8')
                kwargs['stdout'].write(b'x' * 50000 + b'FULL_LOG_END\n')
                return subprocess.CompletedProcess(command, 1)

            with patch.object(sys, 'argv', command_line), \
                 patch('diagnose_mysqltest.make_server', return_value=FixtureServer()), \
                 patch('diagnose_mysqltest.subprocess.run', side_effect=fixture_client), \
                 patch('sys.stdout', new_callable=io.StringIO):
                self.assertEqual(main(), 1)
            summary = json.loads((work / 'summary.json').read_text())
            self.assertEqual(summary['actual_origin'], 'native comparison reject')
            self.assertEqual(summary['select_counts'], {'DIFFERENT': 1, 'MATCH': 12})
            self.assertEqual(summary['first_difference']['actual'], 'ollama')
            self.assertTrue((work / 'compare/mysqltest.stdout.log').read_bytes().endswith(b'FULL_LOG_END\n'))
            self.assertEqual(EXPECTED.read_bytes(), expected_bytes)


if __name__ == '__main__':
    unittest.main()
