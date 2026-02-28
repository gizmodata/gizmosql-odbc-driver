"""
pyodbc integration tests for GizmoSQL ODBC driver.

Tests that pyodbc can connect (which requires SQL_ATTR_AUTOCOMMIT=OFF to work)
and exercise basic transaction operations.

Requires: pyodbc, unixODBC, GizmoSQL server on localhost:31337
"""

import os
import sys
import pyodbc

DRIVER_PATH = os.environ.get("GIZMOSQL_DRIVER", "/tmp/libgizmosql-odbc.so")
CONN_STR = (
    f"Driver={DRIVER_PATH};"
    "host=localhost;port=31337;"
    "uid=gizmosql_user;pwd=gizmosql_password;"
    "useEncryption=false"
)

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
    conn = pyodbc.connect(CONN_STR, autocommit=True)
    cursor = conn.cursor()
    cursor.execute("DROP TABLE IF EXISTS pyodbc_commit_test")
    conn.close()

    conn = pyodbc.connect(CONN_STR, autocommit=False)
    cursor = conn.cursor()
    cursor.execute("CREATE TABLE pyodbc_commit_test (id INTEGER, name VARCHAR)")
    cursor.execute("INSERT INTO pyodbc_commit_test VALUES (1, 'alice')")
    conn.commit()

    cursor.execute("SELECT COUNT(*) FROM pyodbc_commit_test")
    count = cursor.fetchone()[0]
    assert count == 1, f"Expected 1 row after commit, got {count}"

    # Cleanup
    cursor.execute("DROP TABLE IF EXISTS pyodbc_commit_test")
    conn.commit()
    conn.close()


@test("pyodbc rollback transaction")
def test_rollback():
    # First create the table in autocommit mode — it MUST persist across connections
    conn = pyodbc.connect(CONN_STR, autocommit=True)
    cursor = conn.cursor()
    cursor.execute("DROP TABLE IF EXISTS pyodbc_rollback_test")
    cursor.execute("CREATE TABLE pyodbc_rollback_test (id INTEGER)")
    conn.close()

    # Now insert in a transaction and rollback
    conn = pyodbc.connect(CONN_STR, autocommit=False)
    cursor = conn.cursor()
    cursor.execute("INSERT INTO pyodbc_rollback_test VALUES (999)")
    conn.rollback()

    cursor.execute("SELECT COUNT(*) FROM pyodbc_rollback_test")
    count = cursor.fetchone()[0]
    conn.commit()
    assert count == 0, f"Expected 0 rows after rollback, got {count}"

    # Cleanup
    conn.autocommit = True
    cursor.execute("DROP TABLE IF EXISTS pyodbc_rollback_test")
    conn.close()


@test("pyodbc toggle autocommit")
def test_toggle_autocommit():
    conn = pyodbc.connect(CONN_STR, autocommit=False)
    cursor = conn.cursor()

    cursor.execute("DROP TABLE IF EXISTS pyodbc_toggle_test")
    conn.commit()

    cursor.execute("CREATE TABLE pyodbc_toggle_test (id INTEGER)")
    cursor.execute("INSERT INTO pyodbc_toggle_test VALUES (1)")

    # Switch to autocommit=True — should implicitly commit
    conn.autocommit = True

    cursor.execute("SELECT COUNT(*) FROM pyodbc_toggle_test")
    count = cursor.fetchone()[0]
    assert count == 1, f"Expected 1 row after implicit commit, got {count}"

    cursor.execute("DROP TABLE IF EXISTS pyodbc_toggle_test")
    conn.close()


# --- Summary ---
print(f"\n=== pyodbc Results: {passed} passed, {failed} failed ===")
if failed > 0:
    print(f"FAIL: {failed} test(s) failed", file=sys.stderr)
    sys.exit(1)
print("All pyodbc tests passed.")
