# AI enhancement checks

Production changes target `2026-competition` commit
`5bce9e06d1281347ad61344e4b01fab9158c58a2`.
The fixture is a local test service and is not part of the SQL implementation.
No existing competition tests or expected-result files are modified.

## Isolated production helper checks

```bash
python3 unittest/sql/engine/expr/ai_enhancement/test_cpp_logic.py
```

Requires `g++`. The runner extracts the production helper bodies from
`ob_ai_func_utils.cpp`, compiles them with a small API shim, and runs 29
assertions under AddressSanitizer and UndefinedBehaviorSanitizer. This covers
repeated long placeholders, JSON values, nested prompts, Unicode, literal
braces, overflow, vector types/dimensions, and invalid/reordered response indices.
It does not compile the complete translation unit or verify engine integration.

Some container runtimes block LeakSanitizer's access to `/proc`. On those
runtimes run `ASAN_OPTIONS=detect_leaks=0 python3 .../test_cpp_logic.py` and record
that leak detection was unavailable. The address and undefined-behavior
instrumentation remains enabled.

## Native linked-library and HTTP checks

After a successful Linux CMake build, run:

```bash
python3 unittest/sql/engine/expr/ai_enhancement/test_native_helpers.py \
  --build-dir build_release
```

The runner reuses the generated compile and link commands to link a temporary
test executable against the actual project objects and libraries. It runs 37
helper assertions, then eight local HTTP scenarios through `ObAIFuncModel` and
the real curl client: normal and reversed embeddings, duplicate indices, missing
results, invalid values, invalid dimensions, a wrong JSON root and HTTP errors.
The successful scenarios check input-specific vectors, one multi-input request,
and keyless native Ollama completion. The fixture also checks the actual request
headers and body. Set `LD_LIBRARY_PATH` if the completed build needs task-local
runtime libraries.

This test does not start the database, test SQL execution or prove that the
executor invokes the embedding batch callback. It remains usable when an isolated
runtime cannot satisfy seekdb's existing non-loopback-interface UUID startup
requirement. Run SQL integration in a supported host environment as well.

## Fixture tests

```bash
python3 -m unittest discover -s unittest/sql/engine/expr/ai_enhancement -p 'test_mock.py' -v
```

These six tests exercise the fixture only and cannot establish that the database
implementation passes.

## SQL integration

First compile and start the patched seekdb binary using a fresh test data directory.
Install PyMySQL in the test Python environment, then run the fixture in a separate
terminal:

```bash
python3 unittest/sql/engine/expr/ai_enhancement/mock_ai.py --port 8000
```

In another terminal, run:

```bash
SEEKDB_TEST_PORT=2881 python3 -m unittest discover \
  -s unittest/sql/engine/expr/ai_enhancement -p 'test_sql.py' -v
```

Optional variables: `SEEKDB_TEST_HOST`, `SEEKDB_TEST_USER`,
`SEEKDB_TEST_PASSWORD`, `SEEKDB_TEST_MOCK`. Configure credentials in the local
environment; do not add them to source files or test output.

The six integration tests create a unique database/model/endpoint namespace,
exercise prompt JSON and LOB inputs, NULL completion config, native Ollama,
embedding dimensions, 300-row actual batching with NULLs and reversed responses,
filter/LIMIT behavior, 41-document reranking and malformed responses. The test
removes its own endpoints/models/database afterward. It requires an account with
permission to create these test resources. An unset `SEEKDB_TEST_PORT` skips all
integration tests; report them as skipped, not passed.

Run the existing `ai_function` mysqltest suite separately using the repository's
runner and read-only competition fixtures. Its `--error`/connection directives
must be interpreted by mysqltest; they are not ordinary SQL statements.

A single-command runner is also available after compiling the server:

```bash
python3 unittest/sql/engine/expr/ai_enhancement/run_integration.py \
  --binary build_release/src/observer/seekdb --work-dir /tmp/seekdb-ai-qa-new
```

The work directory must not already exist. The runner starts its own local mock
and database, waits for readiness, runs all six SQL tests, then stops both. It
retains the database files and server log for diagnosis; it does not delete an
existing directory or connect tests to a remote database.

## Diagnose the exact enhancement mysqltest comparison

On a host where the compiled database can start, run from the checkout root:

```bash
python3 unittest/sql/engine/expr/ai_enhancement/diagnose_mysqltest.py \
  --binary build_release/src/observer/seekdb --work-dir /tmp/seekdb-ai-diff-new
```

This requires Python 3, the bundled obclient `mysqltest`, and a free local port
8000; no Python package installation is needed. The runner starts the bundled
five-endpoint Mock on `0.0.0.0:8000` and a
fresh database. First it records the original `.test` into a separate local
`actual.result`; then it runs native mysqltest comparison against a byte-for-byte
copy of the original `.result`. The two passes use the same protocol fixture.
It never records over upstream expected results or edits upstream tests.

The output directory retains `report.md`, `select-results.json`, `full.diff`,
`summary.json`, both native mysqltest logs, HTTP requests and database logs.
All 13 SELECTs are listed, with their complete expected/actual output; missing
results are marked `NOT_CAPTURED`. Comparison includes exact formatting, so a
textual mismatch can be found even if all selected values match.

The first `AI_PROMPT` result is structured `template`/`args` JSON, not rendered
text. Its expected value is read from the original result file. A successful
run with this Mock is local validation, not a platform score.

Linux startup checks the process-visible interfaces before launching seekdb.
With only loopback, it writes a `BLOCKED_NETWORK` report without running SQL.
The runner does not modify interfaces, UUID code or networking permissions.
Use a normal host/container network where a non-loopback interface is visible.

For an **already disposable** local database, use `--existing-server --port 2881`.
The fixture drops fixed `enhance_*` model/endpoint names. Never point this mode
at a database that needs to retain resources with those names. Credentials come
from `SEEKDB_TEST_USER`/`SEEKDB_TEST_PASSWORD`; they are not added to commands.
An already captured full `.reject`/recording can be compared without starting
services using `--actual-file /absolute/path/to/actual.reject --work-dir /tmp/new`.

Diagnostic parser/report checks, which **do not execute database SQL**:

```bash
python3 -m unittest discover -s unittest/sql/engine/expr/ai_enhancement \
  -p 'test_diagnose_mysqltest.py' -v
```

## Empty-key endpoint ALTER regression

The native real-library helper now also exercises the production endpoint proxy
with in-memory SQL result/transaction fixtures. It checks empty keys through
copy/INSERT/read/provider-only UPDATE, repeated explicit clearing, preservation
of nonempty keys, recovery of legacy NULL keys for Ollama, and rejection of
missing columns or NULL keys for providers that require a key. The HTTP checks
load a legacy NULL-key Ollama endpoint before performing a real model call.

Run these after compiling the patched database:

```bash
python3 unittest/sql/engine/expr/ai_enhancement/test_native_helpers.py \
  --build-dir build_release
```

These checks exercise production SQL construction and row decoding, not database
execution. The separate SQL integration suite adds repeated ALTER checks with
`access_key IS NULL = 0`, successful completions without Authorization, and
unchanged nonempty OpenAI keys. Run it on a host where seekdb can start, followed
by the original enhancement mysqltest; do not modify its expected results.
