"""
pyodbc integration tests for GizmoSQL ODBC driver.

Tests that pyodbc can connect (which requires SQL_ATTR_AUTOCOMMIT=OFF to work)
and exercise basic transaction operations.

Requires: pyodbc, unixODBC, GizmoSQL server on localhost:31337
"""

import os
import sys
import traceback
import pyodbc

# Disable unixODBC connection pooling — pooled connections retain stale
# transaction state and session handles, causing UTF-16 decode errors
# when pyodbc reads diagnostic messages from a reused connection.
pyodbc.pooling = False

DRIVER_PATH = os.environ.get("GIZMOSQL_DRIVER", "/tmp/libgizmosql-odbc.so")
HOST = os.environ.get("GIZMOSQL_HOST", "localhost")
PORT = os.environ.get("GIZMOSQL_PORT", "31337")
UID = os.environ.get("GIZMOSQL_UID", "gizmosql_user")
PWD = os.environ.get("GIZMOSQL_PWD", "gizmosql_password")
USE_ENCRYPTION = os.environ.get("GIZMOSQL_USE_ENCRYPTION", "false")
DISABLE_CERT_VERIFY = os.environ.get("GIZMOSQL_DISABLE_CERT_VERIFICATION", "")
TRUSTED_CERTS = os.environ.get("GIZMOSQL_TRUSTED_CERTS", "")

CONN_STR = (
    f"Driver={DRIVER_PATH};"
    f"host={HOST};port={PORT};"
    f"uid={UID};pwd={PWD};"
    f"useEncryption={USE_ENCRYPTION};"
    "defaultCatalog=test"
)
if DISABLE_CERT_VERIFY:
    CONN_STR += f";disableCertificateVerification={DISABLE_CERT_VERIFY}"
if TRUSTED_CERTS:
    CONN_STR += f";trustedCerts={TRUSTED_CERTS}"

print(f"Connection string: Driver={DRIVER_PATH};host={HOST};port={PORT};useEncryption={USE_ENCRYPTION}")

passed = 0
failed = 0


def test(name):
    """Decorator to register and run a test function."""
    def decorator(func):
        global passed, failed
        print(f"\n--- Test: {name} ---")
        try:
            func()
            print("  PASS")
            passed += 1
        except Exception as e:
            print(f"  FAIL: {e}")
            traceback.print_exc()
            failed += 1
        return func
    return decorator


@test("pyodbc connect with autocommit=True (default)")
def test_connect_autocommit_on():
    conn = pyodbc.connect(CONN_STR, autocommit=True)
    cursor = conn.cursor()
    cursor.execute("SELECT 42 AS answer")
    row = cursor.fetchone()
    assert row[0] == 42, f"Expected 42, got {row[0]}"
    conn.close()


@test("pyodbc connect with autocommit=False (the fix)")
def test_connect_autocommit_off():
    # This is what pyodbc does by default — it was broken before (HYC00)
    conn = pyodbc.connect(CONN_STR, autocommit=False)
    cursor = conn.cursor()
    cursor.execute("SELECT 1 AS test")
    row = cursor.fetchone()
    assert row[0] == 1, f"Expected 1, got {row[0]}"
    conn.close()


@test("pyodbc default connect (autocommit=False)")
def test_default_connect():
    # pyodbc.connect() without autocommit= defaults to autocommit=False
    conn = pyodbc.connect(CONN_STR)
    assert conn.autocommit is False, f"Expected autocommit=False, got {conn.autocommit}"
    conn.close()


@test("pyodbc commit transaction")
def test_commit():
    print("    step 1: connect autocommit=True")
    conn = pyodbc.connect(CONN_STR, autocommit=True)
    cursor = conn.cursor()
    print("    step 2: DROP TABLE (autocommit=True)")
    cursor.execute("DROP TABLE IF EXISTS pyodbc_commit_test")
    print("    step 3: close")
    conn.close()

    print("    step 4: connect autocommit=False")
    conn = pyodbc.connect(CONN_STR, autocommit=False)
    cursor = conn.cursor()
    print("    step 5: CREATE TABLE (autocommit=False)")
    cursor.execute("CREATE TABLE pyodbc_commit_test (id INTEGER, name VARCHAR)")
    print("    step 6: INSERT")
    cursor.execute("INSERT INTO pyodbc_commit_test VALUES (1, 'alice')")
    print("    step 7: COMMIT")
    conn.commit()

    print("    step 8: SELECT COUNT(*)")
    cursor.execute("SELECT COUNT(*) FROM pyodbc_commit_test")
    print("    step 9: fetchone()")
    count = cursor.fetchone()[0]
    assert count == 1, f"Expected 1 row after commit, got {count}"

    # Cleanup
    print("    step 10: DROP TABLE cleanup")
    cursor.execute("DROP TABLE IF EXISTS pyodbc_commit_test")
    print("    step 11: COMMIT cleanup")
    conn.commit()
    print("    step 12: close")
    conn.close()


@test("pyodbc rollback transaction")
def test_rollback():
    # Create the table with explicit commit — GizmoSQL reuses server sessions via
    # cached bearer tokens, so DDL must be committed via Flight SQL Commit RPC to
    # ensure it persists across connections on the same session.
    print("    step 1: connect autocommit=False")
    conn = pyodbc.connect(CONN_STR, autocommit=False)
    cursor = conn.cursor()
    print("    step 2: DROP TABLE")
    cursor.execute("DROP TABLE IF EXISTS pyodbc_rollback_test")
    print("    step 3: CREATE TABLE")
    cursor.execute("CREATE TABLE pyodbc_rollback_test (id INTEGER)")
    print("    step 4: COMMIT")
    conn.commit()
    print("    step 5: close")
    conn.close()

    # Now insert in a transaction and rollback
    print("    step 6: connect autocommit=False")
    conn = pyodbc.connect(CONN_STR, autocommit=False)
    cursor = conn.cursor()
    print("    step 7: INSERT")
    cursor.execute("INSERT INTO pyodbc_rollback_test VALUES (999)")
    print("    step 8: ROLLBACK")
    conn.rollback()

    print("    step 9: SELECT COUNT(*)")
    cursor.execute("SELECT COUNT(*) FROM pyodbc_rollback_test")
    print("    step 10: fetchone()")
    count = cursor.fetchone()[0]
    print("    step 11: COMMIT")
    conn.commit()
    assert count == 0, f"Expected 0 rows after rollback, got {count}"

    # Cleanup
    print("    step 12: set autocommit=True")
    conn.autocommit = True
    print("    step 13: DROP TABLE cleanup")
    cursor.execute("DROP TABLE IF EXISTS pyodbc_rollback_test")
    print("    step 14: close")
    conn.close()


@test("pyodbc toggle autocommit")
def test_toggle_autocommit():
    print("    step 1: connect autocommit=False")
    conn = pyodbc.connect(CONN_STR, autocommit=False)
    cursor = conn.cursor()

    print("    step 2: DROP TABLE")
    cursor.execute("DROP TABLE IF EXISTS pyodbc_toggle_test")
    print("    step 3: COMMIT")
    conn.commit()

    print("    step 4: CREATE TABLE")
    cursor.execute("CREATE TABLE pyodbc_toggle_test (id INTEGER)")
    print("    step 5: INSERT")
    cursor.execute("INSERT INTO pyodbc_toggle_test VALUES (1)")

    # Switch to autocommit=True — should implicitly commit
    print("    step 6: set autocommit=True")
    conn.autocommit = True

    print("    step 7: SELECT COUNT(*)")
    cursor.execute("SELECT COUNT(*) FROM pyodbc_toggle_test")
    print("    step 8: fetchone()")
    count = cursor.fetchone()[0]
    assert count == 1, f"Expected 1 row after implicit commit, got {count}"

    print("    step 9: DROP TABLE cleanup")
    cursor.execute("DROP TABLE IF EXISTS pyodbc_toggle_test")
    print("    step 10: close")
    conn.close()


# --- Summary ---
print(f"\n=== pyodbc Results: {passed} passed, {failed} failed ===")
if failed > 0:
    print(f"FAIL: {failed} test(s) failed", file=sys.stderr)
    sys.exit(1)
print("All pyodbc tests passed.")
