/*
 * Integration test for ODBC metadata functions (SQLTables / SQLColumns).
 *
 * Exercises all four Flight SQL metadata routing paths in the driver:
 *   1. GetCatalogs       — SQLTables(SQL_ALL_CATALOGS, "", "", NULL)
 *   2. GetDbSchemas      — SQLTables("", SQL_ALL_SCHEMAS, "", NULL)
 *   3. GetTableTypes     — SQLTables("", "", "", SQL_ALL_TABLE_TYPES)
 *   4. GetTables (generic) — SQLTables(NULL, NULL, NULL, NULL)
 * Plus filtered SQLTables variants and SQLColumns.
 *
 * Compile: gcc -o test_metadata test_metadata.c -lodbc
 * Requires: unixODBC, GizmoSQL server on localhost:31337
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
static int g_skipped = 0;

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

/* Execute a SQL statement, ignoring errors (for setup that may fail). */
static void exec_ignore(SQLHSTMT hStmt, const char *sql) {
  SQLExecDirect(hStmt, (SQLCHAR *)sql, SQL_NTS);
  reset_stmt(hStmt);
}

/* Execute a SQL statement, fail hard on error. */
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

/* ---------------------------------------------------------------------------
 * Test: Enumerate catalogs
 *   SQLTables(catalog="%", schema="", table="", type=NULL)
 *   Routes to GetTablesForSQLAllCatalogs -> Flight SQL GetCatalogs
 * ------------------------------------------------------------------------ */
static int test_enumerate_catalogs(SQLHSTMT hStmt) {
  printf("\n--- Test: Enumerate catalogs ---\n");

  SQLRETURN rc = SQLTables(hStmt,
                           (SQLCHAR *)"%", SQL_NTS,  /* catalog = SQL_ALL_CATALOGS */
                           (SQLCHAR *)"", SQL_NTS,   /* schema = "" */
                           (SQLCHAR *)"", SQL_NTS,   /* table = "" */
                           NULL, 0);                 /* type = NULL */
  CHECK(rc, "SQLTables(SQL_ALL_CATALOGS)");

  int count = 0;
  SQLCHAR catalog[MAX_COL_LEN];
  SQLLEN ind_cat, ind_sch, ind_tbl, ind_typ;
  SQLCHAR schema[MAX_COL_LEN], table[MAX_COL_LEN], type[MAX_COL_LEN];

  SQLBindCol(hStmt, 1, SQL_C_CHAR, catalog, sizeof(catalog), &ind_cat);
  SQLBindCol(hStmt, 2, SQL_C_CHAR, schema, sizeof(schema), &ind_sch);
  SQLBindCol(hStmt, 3, SQL_C_CHAR, table, sizeof(table), &ind_tbl);
  SQLBindCol(hStmt, 4, SQL_C_CHAR, type, sizeof(type), &ind_typ);

  int has_default = 0, has_test_catalog = 0;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  catalog='%s' schema=%s table=%s type=%s\n",
           catalog,
           ind_sch == SQL_NULL_DATA ? "NULL" : (char *)schema,
           ind_tbl == SQL_NULL_DATA ? "NULL" : (char *)table,
           ind_typ == SQL_NULL_DATA ? "NULL" : (char *)type);
    count++;

    /* Check schema/table/type are NULL for catalog enumeration */
    if (ind_sch != SQL_NULL_DATA || ind_tbl != SQL_NULL_DATA ||
        ind_typ != SQL_NULL_DATA) {
      fprintf(stderr, "FAIL: non-catalog columns should be NULL\n");
      reset_stmt(hStmt);
      return 1;
    }

    if (ind_cat != SQL_NULL_DATA) {
      if (strcasecmp((char *)catalog, "memory") == 0 ||
          strcasecmp((char *)catalog, "system") == 0 ||
          strcasecmp((char *)catalog, "temp") == 0 ||
          ind_cat != SQL_NULL_DATA) {
        /* Default catalog has various possible names */
        has_default = 1;
      }
      if (strcasecmp((char *)catalog, "test_catalog") == 0) {
        has_test_catalog = 1;
      }
    }
  }

  printf("  Total catalogs: %d\n", count);

  if (count < 2) {
    fprintf(stderr, "FAIL: expected >= 2 catalogs, got %d\n", count);
    reset_stmt(hStmt);
    return 1;
  }
  if (!has_test_catalog) {
    fprintf(stderr, "FAIL: 'test_catalog' not found in catalog list\n");
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test: Enumerate schemas
 *   SQLTables(catalog="", schema="%", table="", type=NULL)
 *   Routes to GetTablesForSQLAllDbSchemas -> Flight SQL GetDbSchemas
 * ------------------------------------------------------------------------ */
static int test_enumerate_schemas(SQLHSTMT hStmt) {
  printf("\n--- Test: Enumerate schemas ---\n");

  SQLRETURN rc = SQLTables(hStmt,
                           (SQLCHAR *)"", SQL_NTS,   /* catalog = "" */
                           (SQLCHAR *)"%", SQL_NTS,  /* schema = SQL_ALL_SCHEMAS */
                           (SQLCHAR *)"", SQL_NTS,   /* table = "" */
                           NULL, 0);                 /* type = NULL */
  CHECK(rc, "SQLTables(SQL_ALL_SCHEMAS)");

  int count = 0;
  SQLCHAR schema[MAX_COL_LEN];
  SQLLEN ind_sch;

  SQLBindCol(hStmt, 2, SQL_C_CHAR, schema, sizeof(schema), &ind_sch);

  int has_main = 0;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  schema='%s'\n", ind_sch == SQL_NULL_DATA ? "NULL" : (char *)schema);
    count++;
    if (ind_sch != SQL_NULL_DATA && strcasecmp((char *)schema, "main") == 0) {
      has_main = 1;
    }
  }

  printf("  Total schemas: %d\n", count);

  if (count < 1) {
    fprintf(stderr, "FAIL: expected >= 1 schema, got %d\n", count);
    reset_stmt(hStmt);
    return 1;
  }
  if (!has_main) {
    fprintf(stderr, "FAIL: 'main' schema not found\n");
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test: Enumerate table types
 *   SQLTables(catalog="", schema="", table="", type="%")
 *   Routes to GetTablesForSQLAllTableTypes -> Flight SQL GetTableTypes
 * ------------------------------------------------------------------------ */
static int test_enumerate_table_types(SQLHSTMT hStmt) {
  printf("\n--- Test: Enumerate table types ---\n");

  SQLRETURN rc = SQLTables(hStmt,
                           (SQLCHAR *)"", SQL_NTS,   /* catalog = "" */
                           (SQLCHAR *)"", SQL_NTS,   /* schema = "" */
                           (SQLCHAR *)"", SQL_NTS,   /* table = "" */
                           (SQLCHAR *)"%", SQL_NTS); /* type = SQL_ALL_TABLE_TYPES */
  CHECK(rc, "SQLTables(SQL_ALL_TABLE_TYPES)");

  int count = 0;
  SQLCHAR type[MAX_COL_LEN];
  SQLLEN ind_typ;

  SQLBindCol(hStmt, 4, SQL_C_CHAR, type, sizeof(type), &ind_typ);

  int has_base_table = 0, has_view = 0;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  type='%s'\n", ind_typ == SQL_NULL_DATA ? "NULL" : (char *)type);
    count++;
    if (ind_typ != SQL_NULL_DATA) {
      if (strcasecmp((char *)type, "BASE TABLE") == 0) has_base_table = 1;
      if (strcasecmp((char *)type, "VIEW") == 0) has_view = 1;
    }
  }

  printf("  Total table types: %d\n", count);

  if (count < 2) {
    fprintf(stderr, "FAIL: expected >= 2 table types, got %d\n", count);
    reset_stmt(hStmt);
    return 1;
  }
  if (!has_base_table) {
    fprintf(stderr, "FAIL: 'BASE TABLE' type not found\n");
    reset_stmt(hStmt);
    return 1;
  }
  if (!has_view) {
    fprintf(stderr, "FAIL: 'VIEW' type not found\n");
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test: List all tables (generic)
 *   SQLTables(NULL, NULL, NULL, NULL)
 *   Routes to GetTablesForGenericUse -> Flight SQL GetTables
 * ------------------------------------------------------------------------ */
static int test_list_all_tables(SQLHSTMT hStmt) {
  printf("\n--- Test: List all tables ---\n");

  SQLRETURN rc = SQLTables(hStmt, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
  CHECK(rc, "SQLTables(NULL,NULL,NULL,NULL)");

  int count = 0;
  SQLCHAR catalog[MAX_COL_LEN], schema[MAX_COL_LEN], table[MAX_COL_LEN];
  SQLCHAR type[MAX_COL_LEN], remarks[MAX_COL_LEN];
  SQLLEN ind_cat, ind_sch, ind_tbl, ind_typ, ind_rem;

  SQLBindCol(hStmt, 1, SQL_C_CHAR, catalog, sizeof(catalog), &ind_cat);
  SQLBindCol(hStmt, 2, SQL_C_CHAR, schema, sizeof(schema), &ind_sch);
  SQLBindCol(hStmt, 3, SQL_C_CHAR, table, sizeof(table), &ind_tbl);
  SQLBindCol(hStmt, 4, SQL_C_CHAR, type, sizeof(type), &ind_typ);
  SQLBindCol(hStmt, 5, SQL_C_CHAR, remarks, sizeof(remarks), &ind_rem);

  /* Track which expected tables we find (test_extra is optional — may not
   * appear if cross-catalog tables are excluded from unfiltered listing) */
  static const char *required_tables[] = {
    "customer", "lineitem", "nation", "orders", "part",
    "partsupp", "region", "supplier", "v_customer_summary"
  };
  int num_required = sizeof(required_tables) / sizeof(required_tables[0]);
  int found[9] = {0};

  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  cat='%s' sch='%s' tbl='%s' typ='%s'\n",
           ind_cat == SQL_NULL_DATA ? "NULL" : (char *)catalog,
           ind_sch == SQL_NULL_DATA ? "NULL" : (char *)schema,
           ind_tbl == SQL_NULL_DATA ? "NULL" : (char *)table,
           ind_typ == SQL_NULL_DATA ? "NULL" : (char *)type);
    count++;

    if (ind_tbl != SQL_NULL_DATA) {
      for (int i = 0; i < num_required; i++) {
        if (strcasecmp((char *)table, required_tables[i]) == 0) {
          found[i] = 1;
        }
      }
    }
  }

  printf("  Total tables: %d\n", count);

  if (count < 9) {
    fprintf(stderr, "FAIL: expected >= 9 tables, got %d\n", count);
    reset_stmt(hStmt);
    return 1;
  }

  int all_found = 1;
  for (int i = 0; i < num_required; i++) {
    if (!found[i]) {
      fprintf(stderr, "FAIL: expected table '%s' not found\n", required_tables[i]);
      all_found = 0;
    }
  }
  if (!all_found) {
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test: Filter by table type (VIEW only)
 *   SQLTables(NULL, NULL, NULL, "'VIEW'")
 *   Routes to GetTablesForGenericUse with type filter
 * ------------------------------------------------------------------------ */
static int test_filter_by_type(SQLHSTMT hStmt) {
  printf("\n--- Test: Filter by table type (VIEW) ---\n");

  SQLRETURN rc = SQLTables(hStmt, NULL, 0, NULL, 0, NULL, 0,
                           (SQLCHAR *)"'VIEW'", SQL_NTS);
  CHECK(rc, "SQLTables(type='VIEW')");

  int count = 0;
  SQLCHAR table[MAX_COL_LEN], type[MAX_COL_LEN];
  SQLLEN ind_tbl, ind_typ;

  SQLBindCol(hStmt, 3, SQL_C_CHAR, table, sizeof(table), &ind_tbl);
  SQLBindCol(hStmt, 4, SQL_C_CHAR, type, sizeof(type), &ind_typ);

  int found_view = 0;
  int non_view = 0;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  tbl='%s' typ='%s'\n",
           ind_tbl == SQL_NULL_DATA ? "NULL" : (char *)table,
           ind_typ == SQL_NULL_DATA ? "NULL" : (char *)type);
    count++;
    if (ind_typ != SQL_NULL_DATA && strcasecmp((char *)type, "VIEW") != 0) {
      non_view = 1;
    }
    if (ind_tbl != SQL_NULL_DATA &&
        strcasecmp((char *)table, "v_customer_summary") == 0) {
      found_view = 1;
    }
  }

  printf("  Total rows: %d\n", count);

  if (non_view) {
    fprintf(stderr, "FAIL: non-VIEW rows returned when filtering by VIEW\n");
    reset_stmt(hStmt);
    return 1;
  }
  if (!found_view) {
    fprintf(stderr, "FAIL: 'v_customer_summary' not found in VIEW listing\n");
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test: Filter by table name pattern
 *   SQLTables(NULL, NULL, "cust%", NULL)
 *   Routes to GetTablesForGenericUse with name filter
 * ------------------------------------------------------------------------ */
static int test_filter_by_name(SQLHSTMT hStmt) {
  printf("\n--- Test: Filter by table name pattern ---\n");

  SQLRETURN rc = SQLTables(hStmt, NULL, 0, NULL, 0,
                           (SQLCHAR *)"cust%", SQL_NTS, NULL, 0);
  CHECK(rc, "SQLTables(table='cust%')");

  int count = 0;
  SQLCHAR table[MAX_COL_LEN];
  SQLLEN ind_tbl;

  SQLBindCol(hStmt, 3, SQL_C_CHAR, table, sizeof(table), &ind_tbl);

  int found_customer = 0;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  tbl='%s'\n", ind_tbl == SQL_NULL_DATA ? "NULL" : (char *)table);
    count++;
    if (ind_tbl != SQL_NULL_DATA &&
        strcasecmp((char *)table, "customer") == 0) {
      found_customer = 1;
    }
  }

  printf("  Total rows: %d\n", count);

  if (count < 1) {
    fprintf(stderr, "FAIL: no tables matched 'cust%%'\n");
    reset_stmt(hStmt);
    return 1;
  }
  if (!found_customer) {
    fprintf(stderr, "FAIL: 'customer' not found with pattern 'cust%%'\n");
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test: SQLColumns for 'customer' table
 *   SQLColumns(NULL, NULL, "customer", NULL)
 *   Routes to GetColumns_V3 -> Flight SQL GetTables(include_schema=true)
 * ------------------------------------------------------------------------ */
static int test_columns_customer(SQLHSTMT hStmt) {
  printf("\n--- Test: SQLColumns for 'customer' ---\n");

  SQLRETURN rc = SQLColumns(hStmt, NULL, 0, NULL, 0,
                            (SQLCHAR *)"customer", SQL_NTS, NULL, 0);
  CHECK(rc, "SQLColumns(customer)");

  int count = 0;
  SQLCHAR table[MAX_COL_LEN], column[MAX_COL_LEN];
  SQLSMALLINT data_type;
  SQLSMALLINT ordinal;
  SQLLEN ind_tbl, ind_col, ind_dt, ind_ord;

  SQLBindCol(hStmt, 3, SQL_C_CHAR, table, sizeof(table), &ind_tbl);
  SQLBindCol(hStmt, 4, SQL_C_CHAR, column, sizeof(column), &ind_col);
  SQLBindCol(hStmt, 5, SQL_C_SSHORT, &data_type, sizeof(data_type), &ind_dt);
  SQLBindCol(hStmt, 17, SQL_C_SSHORT, &ordinal, sizeof(ordinal), &ind_ord);

  int first_ordinal_ok = 0;
  int any_zero_type = 0;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  tbl='%s' col='%s' data_type=%d ordinal=%d\n",
           ind_tbl == SQL_NULL_DATA ? "NULL" : (char *)table,
           ind_col == SQL_NULL_DATA ? "NULL" : (char *)column,
           ind_dt == SQL_NULL_DATA ? -1 : (int)data_type,
           ind_ord == SQL_NULL_DATA ? -1 : (int)ordinal);
    count++;

    if (count == 1 && ind_ord != SQL_NULL_DATA && ordinal == 1) {
      first_ordinal_ok = 1;
    }
    if (ind_dt != SQL_NULL_DATA && data_type == 0) {
      any_zero_type = 1;
    }
  }

  printf("  Total columns: %d\n", count);

  if (count != 8) {
    fprintf(stderr, "FAIL: expected 8 TPC-H customer columns, got %d\n", count);
    reset_stmt(hStmt);
    return 1;
  }
  if (!first_ordinal_ok) {
    fprintf(stderr, "FAIL: ORDINAL_POSITION should start at 1\n");
    reset_stmt(hStmt);
    return 1;
  }
  if (any_zero_type) {
    fprintf(stderr, "FAIL: DATA_TYPE = 0 (SQL_UNKNOWN_TYPE) for some column\n");
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test: SQLColumns for test_catalog table
 *   SQLColumns("test_catalog", "main", "test_extra", NULL)
 *   Verifies cross-catalog column listing
 * ------------------------------------------------------------------------ */
static int test_columns_cross_catalog(SQLHSTMT hStmt) {
  printf("\n--- Test: SQLColumns for test_catalog.main.test_extra ---\n");

  SQLRETURN rc = SQLColumns(hStmt,
                            (SQLCHAR *)"test_catalog", SQL_NTS,
                            (SQLCHAR *)"main", SQL_NTS,
                            (SQLCHAR *)"test_extra", SQL_NTS,
                            NULL, 0);
  CHECK(rc, "SQLColumns(test_catalog.main.test_extra)");

  int count = 0;
  SQLCHAR catalog[MAX_COL_LEN], table[MAX_COL_LEN], column[MAX_COL_LEN];
  SQLLEN ind_cat, ind_tbl, ind_col;

  SQLBindCol(hStmt, 1, SQL_C_CHAR, catalog, sizeof(catalog), &ind_cat);
  SQLBindCol(hStmt, 3, SQL_C_CHAR, table, sizeof(table), &ind_tbl);
  SQLBindCol(hStmt, 4, SQL_C_CHAR, column, sizeof(column), &ind_col);

  int cat_ok = 1;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  cat='%s' tbl='%s' col='%s'\n",
           ind_cat == SQL_NULL_DATA ? "NULL" : (char *)catalog,
           ind_tbl == SQL_NULL_DATA ? "NULL" : (char *)table,
           ind_col == SQL_NULL_DATA ? "NULL" : (char *)column);
    count++;

    if (ind_cat == SQL_NULL_DATA ||
        strcasecmp((char *)catalog, "test_catalog") != 0) {
      cat_ok = 0;
    }
  }

  printf("  Total columns: %d\n", count);

  if (count != 4) {
    fprintf(stderr, "FAIL: expected 4 columns for test_extra, got %d\n", count);
    reset_stmt(hStmt);
    return 1;
  }
  if (!cat_ok) {
    fprintf(stderr, "FAIL: TABLE_CAT should be 'test_catalog' for all rows\n");
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
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

  printf("=== Metadata Integration Tests (SQLTables / SQLColumns) ===\n");

  /* --- Connect --- */
  rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv);
  if (rc != SQL_SUCCESS) { fprintf(stderr, "FAIL: AllocEnv\n"); return 1; }

  rc = SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
  if (rc != SQL_SUCCESS) { fprintf(stderr, "FAIL: SetEnvAttr\n"); return 1; }

  rc = SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc);
  if (rc != SQL_SUCCESS) { fprintf(stderr, "FAIL: AllocDbc\n"); return 1; }

  const char *driver_path = getenv("GIZMOSQL_DRIVER");
  if (!driver_path) driver_path = DEFAULT_DRIVER_PATH;
  char connBuf[MAX_CONN_STR];
  snprintf(connBuf, sizeof(connBuf), CONN_STR_FMT, driver_path);
  printf("Driver: %s\n", driver_path);

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

  /* --- Setup: generate TPC-H data --- */
  printf("\nSetup: generating TPC-H data (sf=0.01)...\n");
  if (exec_required(hStmt, "CALL dbgen(sf=0.01)")) return 1;

  /* --- Setup: create a view --- */
  printf("Setup: creating view v_customer_summary...\n");
  exec_ignore(hStmt, "DROP VIEW IF EXISTS v_customer_summary");
  if (exec_required(hStmt,
        "CREATE VIEW v_customer_summary AS "
        "SELECT c_custkey, c_name, c_nationkey FROM customer")) return 1;

  /* --- Setup: attach second catalog and create table there --- */
  int multi_catalog = 1;
  printf("Setup: attaching test_catalog...\n");
  rc = SQLExecDirect(hStmt,
        (SQLCHAR *)"ATTACH ':memory:' AS test_catalog", SQL_NTS);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    printf("  ATTACH not supported — multi-catalog tests will be skipped\n");
    multi_catalog = 0;
  }
  reset_stmt(hStmt);

  if (multi_catalog) {
    printf("Setup: creating test_catalog.main.test_extra...\n");
    if (exec_required(hStmt,
          "CREATE TABLE test_catalog.main.test_extra "
          "(id INTEGER, name VARCHAR, value DOUBLE, ts TIMESTAMP)")) {
      multi_catalog = 0;
      printf("  CREATE TABLE in test_catalog failed — skipping multi-catalog tests\n");
    }
  }

  /* --- Run tests --- */

  /* Test 1: Enumerate catalogs */
  if (!multi_catalog) {
    printf("\n--- Test: Enumerate catalogs --- SKIP (ATTACH not supported)\n");
    g_skipped++;
  } else if (test_enumerate_catalogs(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* Test 2: Enumerate schemas */
  if (test_enumerate_schemas(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* Test 3: Enumerate table types */
  if (test_enumerate_table_types(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* Test 4: List all tables */
  if (test_list_all_tables(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* Test 5: Filter by table type */
  if (test_filter_by_type(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* Test 6: Filter by table name pattern */
  if (test_filter_by_name(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* Test 7: SQLColumns for customer */
  if (test_columns_customer(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* Test 8: SQLColumns for cross-catalog table */
  if (!multi_catalog) {
    printf("\n--- Test: SQLColumns for test_catalog.main.test_extra --- "
           "SKIP (ATTACH not supported)\n");
    g_skipped++;
  } else if (test_columns_cross_catalog(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* --- Cleanup --- */
  exec_ignore(hStmt, "DROP VIEW IF EXISTS v_customer_summary");
  if (multi_catalog) {
    exec_ignore(hStmt, "DROP TABLE IF EXISTS test_catalog.main.test_extra");
    exec_ignore(hStmt, "DETACH test_catalog");
  }

  SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
  SQLDisconnect(hDbc);
  SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
  SQLFreeHandle(SQL_HANDLE_ENV, hEnv);

  /* --- Summary --- */
  printf("\n=== Results: %d passed, %d failed, %d skipped ===\n",
         g_passed, g_failed, g_skipped);

  if (g_failed > 0) {
    fprintf(stderr, "FAIL: %d test(s) failed\n", g_failed);
    return 1;
  }

  printf("All metadata tests passed.\n");
  return 0;
}
