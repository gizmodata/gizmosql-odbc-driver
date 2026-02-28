/*
 * Integration test for ODBC transaction support.
 *
 * Exercises transaction lifecycle via Flight SQL BeginTransaction/Commit/Rollback:
 *   1. Default autocommit state
 *   2. Set autocommit OFF (the pyodbc fix)
 *   3. Commit — data persists
 *   4. Rollback — data is gone (DDL is transactional in GizmoSQL)
 *   5. Autocommit OFF→ON implicitly commits
 *   6. Multiple statements in a transaction
 *   7. Disconnect without commit rolls back
 *   8. SQL_TXN_CAPABLE info
 *   9. SQL_ATTR_TXN_ISOLATION attribute
 *
 * Compile: gcc -o test_transactions test_transactions.c -lodbc
 * Requires: unixODBC, GizmoSQL server on localhost:31337
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sql.h>
#include <sqlext.h>

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------ */

/* Default driver path (overridable via GIZMOSQL_DRIVER env var) */
#define DEFAULT_DRIVER_PATH "/tmp/libgizmosql-odbc.so"
#define CONN_STR_FMT \
  "Driver=%s;host=localhost;port=31337;" \
  "uid=gizmosql_user;pwd=gizmosql_password;useEncryption=false"

#define MAX_COL_LEN 256
#define MAX_CONN_STR 512

static int g_passed = 0;
static int g_failed = 0;

static const char *g_driver_path = NULL;

#define CHECK(rc, msg) do { \
  if ((rc) != SQL_SUCCESS && (rc) != SQL_SUCCESS_WITH_INFO) { \
    fprintf(stderr, "FAIL: %s (rc=%d)\n", msg, rc); \
    print_diagnostics(SQL_HANDLE_STMT, hStmt); \
    return 1; \
  } \
} while(0)

#define CHECK_DBC(rc, msg) do { \
  if ((rc) != SQL_SUCCESS && (rc) != SQL_SUCCESS_WITH_INFO) { \
    fprintf(stderr, "FAIL: %s (rc=%d)\n", msg, rc); \
    print_diagnostics(SQL_HANDLE_DBC, hDbc); \
    return 1; \
  } \
} while(0)

#define CHECK_ENV(rc, msg) do { \
  if ((rc) != SQL_SUCCESS && (rc) != SQL_SUCCESS_WITH_INFO) { \
    fprintf(stderr, "FAIL: %s (rc=%d)\n", msg, rc); \
    print_diagnostics(SQL_HANDLE_ENV, hEnv); \
    return 1; \
  } \
} while(0)

static void print_diagnostics(SQLSMALLINT handleType, SQLHANDLE handle) {
  SQLCHAR state[8], msg[1024];
  SQLINTEGER native;
  SQLSMALLINT msgLen;
  SQLSMALLINT i = 1;
  while (SQLGetDiagRec(handleType, handle, i++, state, &native, msg,
                       sizeof(msg), &msgLen) == SQL_SUCCESS) {
    fprintf(stderr, "  [%s] %s (native=%d)\n", state, msg, (int)native);
  }
}

static void reset_stmt(SQLHSTMT hStmt) {
  SQLFreeStmt(hStmt, SQL_CLOSE);
  SQLFreeStmt(hStmt, SQL_UNBIND);
  SQLFreeStmt(hStmt, SQL_RESET_PARAMS);
}

static void exec_ignore(SQLHSTMT hStmt, const char *sql) {
  SQLExecDirect(hStmt, (SQLCHAR *)sql, SQL_NTS);
  reset_stmt(hStmt);
}

static int exec_required(SQLHSTMT hStmt, const char *sql) {
  SQLRETURN rc = SQLExecDirect(hStmt, (SQLCHAR *)sql, SQL_NTS);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    fprintf(stderr, "FAIL: exec '%s' (rc=%d)\n", sql, rc);
    print_diagnostics(SQL_HANDLE_STMT, hStmt);
    return 1;
  }
  reset_stmt(hStmt);
  return 0;
}

/* Helper: count rows from a SELECT query */
static int count_rows(SQLHSTMT hStmt, const char *sql) {
  SQLRETURN rc = SQLExecDirect(hStmt, (SQLCHAR *)sql, SQL_NTS);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    reset_stmt(hStmt);
    return -1; /* table doesn't exist or query error */
  }
  int count = 0;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    count++;
  }
  reset_stmt(hStmt);
  return count;
}

/* Helper: open a fresh connection */
static int open_connection(SQLHENV *hEnv, SQLHDBC *hDbc) {
  SQLRETURN rc;

  rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, hEnv);
  if (rc != SQL_SUCCESS) return 1;

  rc = SQLSetEnvAttr(*hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
  if (rc != SQL_SUCCESS) return 1;

  rc = SQLAllocHandle(SQL_HANDLE_DBC, *hEnv, hDbc);
  if (rc != SQL_SUCCESS) return 1;

  char connBuf[MAX_CONN_STR];
  snprintf(connBuf, sizeof(connBuf), CONN_STR_FMT, g_driver_path);

  SQLCHAR outConn[MAX_CONN_STR];
  SQLSMALLINT outLen;
  rc = SQLDriverConnect(*hDbc, NULL, (SQLCHAR *)connBuf, SQL_NTS,
                        outConn, sizeof(outConn), &outLen,
                        SQL_DRIVER_NOPROMPT);
  return (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) ? 0 : 1;
}

static void close_connection(SQLHENV hEnv, SQLHDBC hDbc) {
  SQLDisconnect(hDbc);
  SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
  SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
}

/* ---------------------------------------------------------------------------
 * Test 1: Default autocommit is ON
 * ------------------------------------------------------------------------ */
static int test_autocommit_default(SQLHDBC hDbc) {
  printf("\n--- Test: Default autocommit is ON ---\n");

  SQLUINTEGER autocommit = 0;
  SQLRETURN rc = SQLGetConnectAttr(hDbc, SQL_ATTR_AUTOCOMMIT,
                                    &autocommit, sizeof(autocommit), NULL);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    fprintf(stderr, "FAIL: SQLGetConnectAttr(SQL_ATTR_AUTOCOMMIT) rc=%d\n", rc);
    print_diagnostics(SQL_HANDLE_DBC, hDbc);
    return 1;
  }

  if (autocommit != SQL_AUTOCOMMIT_ON) {
    fprintf(stderr, "FAIL: expected SQL_AUTOCOMMIT_ON (%d), got %u\n",
            (int)SQL_AUTOCOMMIT_ON, (unsigned)autocommit);
    return 1;
  }

  printf("  autocommit=%u (SQL_AUTOCOMMIT_ON)\n", (unsigned)autocommit);
  printf("  PASS\n");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test 2: Set autocommit OFF succeeds (the pyodbc fix)
 * ------------------------------------------------------------------------ */
static int test_set_autocommit_off(SQLHDBC hDbc) {
  printf("\n--- Test: Set autocommit OFF ---\n");

  SQLRETURN rc = SQLSetConnectAttr(hDbc, SQL_ATTR_AUTOCOMMIT,
                                    (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    fprintf(stderr, "FAIL: SQLSetConnectAttr(SQL_AUTOCOMMIT_OFF) rc=%d\n", rc);
    print_diagnostics(SQL_HANDLE_DBC, hDbc);
    return 1;
  }

  /* Verify it took effect */
  SQLUINTEGER autocommit = 99;
  rc = SQLGetConnectAttr(hDbc, SQL_ATTR_AUTOCOMMIT,
                          &autocommit, sizeof(autocommit), NULL);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    fprintf(stderr, "FAIL: SQLGetConnectAttr after set rc=%d\n", rc);
    return 1;
  }

  if (autocommit != SQL_AUTOCOMMIT_OFF) {
    fprintf(stderr, "FAIL: expected SQL_AUTOCOMMIT_OFF, got %u\n",
            (unsigned)autocommit);
    return 1;
  }

  /* Restore autocommit ON for other tests */
  SQLSetConnectAttr(hDbc, SQL_ATTR_AUTOCOMMIT,
                    (SQLPOINTER)SQL_AUTOCOMMIT_ON, 0);

  printf("  PASS\n");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test 3: Commit — data persists
 * ------------------------------------------------------------------------ */
static int test_commit(SQLHSTMT hStmt, SQLHDBC hDbc) {
  printf("\n--- Test: Commit persists data ---\n");

  /* Cleanup from previous runs */
  exec_ignore(hStmt, "DROP TABLE IF EXISTS txn_commit_test");

  /* Switch to manual commit */
  SQLRETURN rc = SQLSetConnectAttr(hDbc, SQL_ATTR_AUTOCOMMIT,
                                    (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
  CHECK_DBC(rc, "Set autocommit OFF");

  /* Create table and insert data */
  if (exec_required(hStmt, "CREATE TABLE txn_commit_test (id INTEGER, name VARCHAR)"))
    return 1;
  if (exec_required(hStmt, "INSERT INTO txn_commit_test VALUES (1, 'alice')"))
    return 1;
  if (exec_required(hStmt, "INSERT INTO txn_commit_test VALUES (2, 'bob')"))
    return 1;

  /* Commit */
  rc = SQLEndTran(SQL_HANDLE_DBC, hDbc, SQL_COMMIT);
  CHECK_DBC(rc, "SQLEndTran(SQL_COMMIT)");

  /* Verify data persists */
  int rows = count_rows(hStmt, "SELECT * FROM txn_commit_test");
  printf("  Rows after commit: %d\n", rows);

  if (rows != 2) {
    fprintf(stderr, "FAIL: expected 2 rows after commit, got %d\n", rows);
    /* Cleanup */
    SQLEndTran(SQL_HANDLE_DBC, hDbc, SQL_COMMIT);
    exec_ignore(hStmt, "DROP TABLE IF EXISTS txn_commit_test");
    SQLEndTran(SQL_HANDLE_DBC, hDbc, SQL_COMMIT);
    SQLSetConnectAttr(hDbc, SQL_ATTR_AUTOCOMMIT,
                      (SQLPOINTER)SQL_AUTOCOMMIT_ON, 0);
    return 1;
  }

  /* Cleanup */
  exec_ignore(hStmt, "DROP TABLE IF EXISTS txn_commit_test");
  SQLEndTran(SQL_HANDLE_DBC, hDbc, SQL_COMMIT);

  /* Restore autocommit */
  SQLSetConnectAttr(hDbc, SQL_ATTR_AUTOCOMMIT,
                    (SQLPOINTER)SQL_AUTOCOMMIT_ON, 0);

  printf("  PASS\n");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test 4: Rollback — data is gone (DDL is transactional)
 * ------------------------------------------------------------------------ */
static int test_rollback(SQLHSTMT hStmt, SQLHDBC hDbc) {
  printf("\n--- Test: Rollback reverses changes ---\n");

  /* Cleanup from previous runs */
  exec_ignore(hStmt, "DROP TABLE IF EXISTS txn_rollback_test");

  /* Switch to manual commit */
  SQLRETURN rc = SQLSetConnectAttr(hDbc, SQL_ATTR_AUTOCOMMIT,
                                    (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
  CHECK_DBC(rc, "Set autocommit OFF");

  /* Create table and insert — then rollback */
  if (exec_required(hStmt, "CREATE TABLE txn_rollback_test (id INTEGER)"))
    return 1;
  if (exec_required(hStmt, "INSERT INTO txn_rollback_test VALUES (1)"))
    return 1;

  rc = SQLEndTran(SQL_HANDLE_DBC, hDbc, SQL_ROLLBACK);
  CHECK_DBC(rc, "SQLEndTran(SQL_ROLLBACK)");

  /* Verify table is gone (DDL is transactional in GizmoSQL) */
  int rows = count_rows(hStmt, "SELECT * FROM txn_rollback_test");
  printf("  Rows after rollback: %d (expect -1 for table not found)\n", rows);

  /* After rollback, we may be in a failed txn state, commit to clear it */
  SQLEndTran(SQL_HANDLE_DBC, hDbc, SQL_COMMIT);

  /* Restore autocommit */
  SQLSetConnectAttr(hDbc, SQL_ATTR_AUTOCOMMIT,
                    (SQLPOINTER)SQL_AUTOCOMMIT_ON, 0);

  if (rows != -1) {
    fprintf(stderr, "FAIL: table should not exist after DDL rollback, got %d rows\n", rows);
    exec_ignore(hStmt, "DROP TABLE IF EXISTS txn_rollback_test");
    return 1;
  }

  printf("  PASS\n");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test 5: Autocommit OFF→ON implicitly commits
 * ------------------------------------------------------------------------ */
static int test_autocommit_off_to_on_commits(SQLHSTMT hStmt, SQLHDBC hDbc) {
  printf("\n--- Test: Autocommit OFF→ON implicitly commits ---\n");

  exec_ignore(hStmt, "DROP TABLE IF EXISTS txn_auto_commit_test");

  SQLRETURN rc = SQLSetConnectAttr(hDbc, SQL_ATTR_AUTOCOMMIT,
                                    (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
  CHECK_DBC(rc, "Set autocommit OFF");

  if (exec_required(hStmt, "CREATE TABLE txn_auto_commit_test (id INTEGER)"))
    return 1;
  if (exec_required(hStmt, "INSERT INTO txn_auto_commit_test VALUES (42)"))
    return 1;

  /* Switch autocommit ON — should implicitly commit */
  rc = SQLSetConnectAttr(hDbc, SQL_ATTR_AUTOCOMMIT,
                          (SQLPOINTER)SQL_AUTOCOMMIT_ON, 0);
  CHECK_DBC(rc, "Set autocommit ON");

  /* Verify data persists */
  int rows = count_rows(hStmt, "SELECT * FROM txn_auto_commit_test");
  printf("  Rows after autocommit ON: %d\n", rows);

  exec_ignore(hStmt, "DROP TABLE IF EXISTS txn_auto_commit_test");

  if (rows != 1) {
    fprintf(stderr, "FAIL: expected 1 row after implicit commit, got %d\n", rows);
    return 1;
  }

  printf("  PASS\n");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test 6: Multiple statements in transaction
 * ------------------------------------------------------------------------ */
static int test_multiple_statements_in_transaction(SQLHSTMT hStmt, SQLHDBC hDbc) {
  printf("\n--- Test: Multiple statements in transaction ---\n");

  exec_ignore(hStmt, "DROP TABLE IF EXISTS txn_multi_test");

  SQLRETURN rc = SQLSetConnectAttr(hDbc, SQL_ATTR_AUTOCOMMIT,
                                    (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
  CHECK_DBC(rc, "Set autocommit OFF");

  if (exec_required(hStmt, "CREATE TABLE txn_multi_test (id INTEGER, val VARCHAR)"))
    return 1;
  if (exec_required(hStmt, "INSERT INTO txn_multi_test VALUES (1, 'first')"))
    return 1;
  if (exec_required(hStmt, "INSERT INTO txn_multi_test VALUES (2, 'second')"))
    return 1;
  if (exec_required(hStmt, "INSERT INTO txn_multi_test VALUES (3, 'third')"))
    return 1;

  rc = SQLEndTran(SQL_HANDLE_DBC, hDbc, SQL_COMMIT);
  CHECK_DBC(rc, "SQLEndTran(SQL_COMMIT)");

  int rows = count_rows(hStmt, "SELECT * FROM txn_multi_test");
  printf("  Rows after multi-insert commit: %d\n", rows);

  /* Also do some in-txn work: commit then clean up */
  SQLEndTran(SQL_HANDLE_DBC, hDbc, SQL_COMMIT);

  /* Cleanup */
  exec_ignore(hStmt, "DROP TABLE IF EXISTS txn_multi_test");
  SQLEndTran(SQL_HANDLE_DBC, hDbc, SQL_COMMIT);
  SQLSetConnectAttr(hDbc, SQL_ATTR_AUTOCOMMIT,
                    (SQLPOINTER)SQL_AUTOCOMMIT_ON, 0);

  if (rows != 3) {
    fprintf(stderr, "FAIL: expected 3 rows, got %d\n", rows);
    return 1;
  }

  printf("  PASS\n");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test 7: Disconnect without commit rolls back
 * ------------------------------------------------------------------------ */
static int test_disconnect_rollback(void) {
  printf("\n--- Test: Disconnect without commit rolls back ---\n");

  /* Open connection 1: create a persistent table, commit it */
  SQLHENV hEnv1; SQLHDBC hDbc1;
  if (open_connection(&hEnv1, &hDbc1)) {
    fprintf(stderr, "FAIL: could not open connection 1\n");
    return 1;
  }

  SQLHSTMT hStmt1;
  SQLAllocHandle(SQL_HANDLE_STMT, hDbc1, &hStmt1);
  exec_ignore(hStmt1, "DROP TABLE IF EXISTS txn_disconnect_test");

  /* First: create the table in autocommit mode so it definitely exists */
  if (exec_required(hStmt1, "CREATE TABLE txn_disconnect_test (id INTEGER)")) {
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt1);
    close_connection(hEnv1, hDbc1);
    return 1;
  }

  /* Now switch to manual commit and insert a row */
  SQLSetConnectAttr(hDbc1, SQL_ATTR_AUTOCOMMIT,
                    (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
  exec_required(hStmt1, "INSERT INTO txn_disconnect_test VALUES (999)");

  /* Disconnect WITHOUT committing — this should rollback */
  SQLFreeHandle(SQL_HANDLE_STMT, hStmt1);
  close_connection(hEnv1, hDbc1);

  /* Open connection 2: verify the INSERT was rolled back */
  SQLHENV hEnv2; SQLHDBC hDbc2;
  if (open_connection(&hEnv2, &hDbc2)) {
    fprintf(stderr, "FAIL: could not open connection 2\n");
    return 1;
  }

  SQLHSTMT hStmt2;
  SQLAllocHandle(SQL_HANDLE_STMT, hDbc2, &hStmt2);

  int rows = count_rows(hStmt2, "SELECT * FROM txn_disconnect_test");
  printf("  Rows after disconnect rollback: %d\n", rows);

  /* Cleanup */
  exec_ignore(hStmt2, "DROP TABLE IF EXISTS txn_disconnect_test");
  SQLFreeHandle(SQL_HANDLE_STMT, hStmt2);
  close_connection(hEnv2, hDbc2);

  if (rows != 0) {
    fprintf(stderr, "FAIL: expected 0 rows (INSERT rolled back), got %d\n", rows);
    return 1;
  }

  printf("  PASS\n");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test 8: SQL_TXN_CAPABLE returns SQL_TC_ALL
 * ------------------------------------------------------------------------ */
static int test_txn_capable_info(SQLHDBC hDbc) {
  printf("\n--- Test: SQL_TXN_CAPABLE info ---\n");

  SQLUSMALLINT txn_capable = 0;
  SQLSMALLINT out_len = 0;
  SQLRETURN rc = SQLGetInfo(hDbc, SQL_TXN_CAPABLE, &txn_capable,
                             sizeof(txn_capable), &out_len);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    fprintf(stderr, "FAIL: SQLGetInfo(SQL_TXN_CAPABLE) rc=%d\n", rc);
    print_diagnostics(SQL_HANDLE_DBC, hDbc);
    return 1;
  }

  printf("  SQL_TXN_CAPABLE=%u (expect SQL_TC_ALL=%d)\n",
         (unsigned)txn_capable, (int)SQL_TC_ALL);

  if (txn_capable != SQL_TC_ALL) {
    fprintf(stderr, "FAIL: expected SQL_TC_ALL (%d), got %u\n",
            (int)SQL_TC_ALL, (unsigned)txn_capable);
    return 1;
  }

  printf("  PASS\n");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test 9: SQL_ATTR_TXN_ISOLATION returns SQL_TXN_SERIALIZABLE
 * ------------------------------------------------------------------------ */
static int test_txn_isolation(SQLHDBC hDbc) {
  printf("\n--- Test: SQL_ATTR_TXN_ISOLATION ---\n");

  SQLUINTEGER isolation = 0;
  SQLRETURN rc = SQLGetConnectAttr(hDbc, SQL_ATTR_TXN_ISOLATION,
                                    &isolation, sizeof(isolation), NULL);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    fprintf(stderr, "FAIL: SQLGetConnectAttr(SQL_ATTR_TXN_ISOLATION) rc=%d\n", rc);
    print_diagnostics(SQL_HANDLE_DBC, hDbc);
    return 1;
  }

  printf("  SQL_ATTR_TXN_ISOLATION=%u (expect SQL_TXN_SERIALIZABLE=%d)\n",
         (unsigned)isolation, (int)SQL_TXN_SERIALIZABLE);

  if (isolation != SQL_TXN_SERIALIZABLE) {
    fprintf(stderr, "FAIL: expected SQL_TXN_SERIALIZABLE (%d), got %u\n",
            (int)SQL_TXN_SERIALIZABLE, (unsigned)isolation);
    return 1;
  }

  printf("  PASS\n");
  return 0;
}

/* ---------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------ */
int main(void) {
  SQLHENV hEnv = SQL_NULL_HENV;
  SQLHDBC hDbc = SQL_NULL_HDBC;
  SQLHSTMT hStmt = SQL_NULL_HSTMT;
  SQLRETURN rc;

  /* Ensure output is visible even if we crash */
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  printf("=== Integration Tests (Transactions) ===\n");

  g_driver_path = getenv("GIZMOSQL_DRIVER");
  if (!g_driver_path) g_driver_path = DEFAULT_DRIVER_PATH;
  printf("Driver: %s\n", g_driver_path);

  /* --- Connect --- */
  rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv);
  if (rc != SQL_SUCCESS) { fprintf(stderr, "FAIL: AllocEnv\n"); return 1; }

  rc = SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
  if (rc != SQL_SUCCESS) { fprintf(stderr, "FAIL: SetEnvAttr\n"); return 1; }

  rc = SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc);
  if (rc != SQL_SUCCESS) { fprintf(stderr, "FAIL: AllocDbc\n"); return 1; }

  char connBuf[MAX_CONN_STR];
  snprintf(connBuf, sizeof(connBuf), CONN_STR_FMT, g_driver_path);

  SQLCHAR outConn[MAX_CONN_STR];
  SQLSMALLINT outLen;
  rc = SQLDriverConnect(hDbc, NULL, (SQLCHAR *)connBuf, SQL_NTS,
                        outConn, sizeof(outConn), &outLen,
                        SQL_DRIVER_NOPROMPT);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    fprintf(stderr, "FAIL: DriverConnect (rc=%d)\n", rc);
    print_diagnostics(SQL_HANDLE_DBC, hDbc);
    return 1;
  }

  rc = SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);
  if (rc != SQL_SUCCESS) { fprintf(stderr, "FAIL: AllocStmt\n"); return 1; }

  /* --- Run tests --- */

  /* Test 1: Default autocommit */
  if (test_autocommit_default(hDbc)) g_failed++; else g_passed++;

  /* Test 2: Set autocommit OFF */
  if (test_set_autocommit_off(hDbc)) g_failed++; else g_passed++;

  /* Test 3: Commit persists data */
  if (test_commit(hStmt, hDbc)) g_failed++; else g_passed++;

  /* Test 4: Rollback reverses changes */
  if (test_rollback(hStmt, hDbc)) g_failed++; else g_passed++;

  /* Test 5: Autocommit OFF→ON implicitly commits */
  if (test_autocommit_off_to_on_commits(hStmt, hDbc)) g_failed++; else g_passed++;

  /* Test 6: Multiple statements in transaction */
  if (test_multiple_statements_in_transaction(hStmt, hDbc)) g_failed++; else g_passed++;

  /* Test 7: Disconnect without commit rolls back */
  /* (uses its own connections internally) */
  SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
  SQLDisconnect(hDbc);
  SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
  SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
  if (test_disconnect_rollback()) g_failed++; else g_passed++;

  /* Re-connect for remaining attribute tests */
  rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv);
  if (rc != SQL_SUCCESS) { fprintf(stderr, "FAIL: AllocEnv\n"); return 1; }
  rc = SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
  if (rc != SQL_SUCCESS) { fprintf(stderr, "FAIL: SetEnvAttr\n"); return 1; }
  rc = SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc);
  if (rc != SQL_SUCCESS) { fprintf(stderr, "FAIL: AllocDbc\n"); return 1; }
  rc = SQLDriverConnect(hDbc, NULL, (SQLCHAR *)connBuf, SQL_NTS,
                        outConn, sizeof(outConn), &outLen,
                        SQL_DRIVER_NOPROMPT);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    fprintf(stderr, "FAIL: Reconnect (rc=%d)\n", rc);
    print_diagnostics(SQL_HANDLE_DBC, hDbc);
    return 1;
  }

  /* Test 8: SQL_TXN_CAPABLE info */
  if (test_txn_capable_info(hDbc)) g_failed++; else g_passed++;

  /* Test 9: SQL_ATTR_TXN_ISOLATION */
  if (test_txn_isolation(hDbc)) g_failed++; else g_passed++;

  /* --- Cleanup --- */
  SQLDisconnect(hDbc);
  SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
  SQLFreeHandle(SQL_HANDLE_ENV, hEnv);

  /* --- Summary --- */
  printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);

  if (g_failed > 0) {
    fprintf(stderr, "FAIL: %d test(s) failed\n", g_failed);
    return 1;
  }

  printf("All transaction tests passed.\n");
  return 0;
}
