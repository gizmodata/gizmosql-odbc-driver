/*
 * Integration test for ODBC metadata and parameterized query functions.
 *
 * Exercises all four Flight SQL metadata routing paths in the driver:
 *   1. GetCatalogs       — SQLTables(SQL_ALL_CATALOGS, "", "", NULL)
 *   2. GetDbSchemas      — SQLTables("", SQL_ALL_SCHEMAS, "", NULL)
 *   3. GetTableTypes     — SQLTables("", "", "", SQL_ALL_TABLE_TYPES)
 *   4. GetTables (generic) — SQLTables(NULL, NULL, NULL, NULL)
 * Plus filtered SQLTables variants, SQLColumns, parameterized queries
 * (SQLPrepare/SQLBindParameter/SQLExecute), and primary/foreign key
 * discovery (SQLPrimaryKeys/SQLForeignKeys).
 *
 * Compile: gcc -o test_metadata test_metadata.c -lodbc
 * Requires: unixODBC, GizmoSQL server on localhost:31337
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif
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

  int has_table = 0, has_view = 0;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  type='%s'\n", ind_typ == SQL_NULL_DATA ? "NULL" : (char *)type);
    count++;
    if (ind_typ != SQL_NULL_DATA) {
      if (strcasecmp((char *)type, "TABLE") == 0) has_table = 1;
      if (strcasecmp((char *)type, "VIEW") == 0) has_view = 1;
    }
  }

  printf("  Total table types: %d\n", count);

  if (count < 2) {
    fprintf(stderr, "FAIL: expected >= 2 table types, got %d\n", count);
    reset_stmt(hStmt);
    return 1;
  }
  if (!has_table) {
    fprintf(stderr, "FAIL: 'TABLE' type not found\n");
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
 * Test: SQLNumParams and SQLDescribeParam after Prepare
 *   Verifies that after SQLPrepare, SQLNumParams returns the correct count
 *   and SQLDescribeParam returns parameter metadata.
 * ------------------------------------------------------------------------ */
static int test_num_params(SQLHSTMT hStmt) {
  printf("\n--- Test: SQLNumParams after Prepare ---\n");

  SQLRETURN rc = SQLPrepare(hStmt,
    (SQLCHAR *)"SELECT * FROM region WHERE r_regionkey = ?", SQL_NTS);
  CHECK(rc, "SQLPrepare with 1 parameter");

  SQLSMALLINT paramCount = -1;
  rc = SQLNumParams(hStmt, &paramCount);
  CHECK(rc, "SQLNumParams");

  printf("  Parameter count: %d\n", (int)paramCount);

  if (paramCount != 1) {
    fprintf(stderr, "FAIL: expected 1 parameter, got %d\n", (int)paramCount);
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test: Parameterized query with integer parameter
 *   SQLPrepare -> SQLBindParameter(int) -> SQLExecute -> SQLFetch
 *   Verifies that binding an integer parameter via prepared statement works
 *   end-to-end through the Flight SQL PreparedStatement::SetParameters path.
 * ------------------------------------------------------------------------ */
static int test_parameterized_query_int(SQLHSTMT hStmt) {
  printf("\n--- Test: Parameterized query (integer) ---\n");

  SQLRETURN rc = SQLPrepare(hStmt,
    (SQLCHAR *)"SELECT r_regionkey, r_name FROM region WHERE r_regionkey = ?",
    SQL_NTS);
  CHECK(rc, "SQLPrepare");

  /* Bind parameter: r_regionkey = 1 (AFRICA in TPC-H) */
  SQLINTEGER regionkey = 1;
  rc = SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
                         0, 0, &regionkey, 0, NULL);
  CHECK(rc, "SQLBindParameter(int)");

  rc = SQLExecute(hStmt);
  CHECK(rc, "SQLExecute");

  /* Fetch results */
  SQLINTEGER result_key;
  SQLCHAR result_name[MAX_COL_LEN];
  SQLLEN ind_key, ind_name;

  SQLBindCol(hStmt, 1, SQL_C_SLONG, &result_key, sizeof(result_key), &ind_key);
  SQLBindCol(hStmt, 2, SQL_C_CHAR, result_name, sizeof(result_name), &ind_name);

  int count = 0;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  r_regionkey=%d r_name='%s'\n",
           (int)result_key,
           ind_name == SQL_NULL_DATA ? "NULL" : (char *)result_name);
    count++;
  }

  printf("  Total rows: %d\n", count);

  if (count != 1) {
    fprintf(stderr, "FAIL: expected 1 row for regionkey=1, got %d\n", count);
    reset_stmt(hStmt);
    return 1;
  }

  if (result_key != 1) {
    fprintf(stderr, "FAIL: expected r_regionkey=1, got %d\n", (int)result_key);
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test: Parameterized query with string parameter
 *   SQLPrepare -> SQLBindParameter(varchar) -> SQLExecute -> SQLFetch
 *   Power BI DirectQuery often sends parameters as strings even for
 *   numeric columns; this test verifies string parameter binding.
 * ------------------------------------------------------------------------ */
static int test_parameterized_query_string(SQLHSTMT hStmt) {
  printf("\n--- Test: Parameterized query (string) ---\n");

  SQLRETURN rc = SQLPrepare(hStmt,
    (SQLCHAR *)"SELECT n_nationkey, n_name FROM nation WHERE n_name = ?",
    SQL_NTS);
  CHECK(rc, "SQLPrepare");

  /* Bind parameter: n_name = 'FRANCE' */
  SQLCHAR name_param[] = "FRANCE";
  SQLLEN name_ind = SQL_NTS;
  rc = SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                         64, 0, name_param, sizeof(name_param), &name_ind);
  CHECK(rc, "SQLBindParameter(varchar)");

  rc = SQLExecute(hStmt);
  CHECK(rc, "SQLExecute");

  /* Fetch results */
  SQLINTEGER result_key;
  SQLCHAR result_name[MAX_COL_LEN];
  SQLLEN ind_key, ind_name;

  SQLBindCol(hStmt, 1, SQL_C_SLONG, &result_key, sizeof(result_key), &ind_key);
  SQLBindCol(hStmt, 2, SQL_C_CHAR, result_name, sizeof(result_name), &ind_name);

  int count = 0;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  n_nationkey=%d n_name='%s'\n",
           (int)result_key,
           ind_name == SQL_NULL_DATA ? "NULL" : (char *)result_name);
    count++;
  }

  printf("  Total rows: %d\n", count);

  if (count != 1) {
    fprintf(stderr, "FAIL: expected 1 row for n_name='FRANCE', got %d\n", count);
    reset_stmt(hStmt);
    return 1;
  }

  if (ind_name != SQL_NULL_DATA && strcasecmp((char *)result_name, "FRANCE") != 0) {
    fprintf(stderr, "FAIL: expected n_name='FRANCE', got '%s'\n", (char *)result_name);
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test: Parameterized query with LIMIT ? (Power BI DirectQuery pattern)
 *   Power BI generates: SELECT "col" FROM "table" LIMIT ?
 *   This is the exact pattern that failed before SQLBindParameter support.
 * ------------------------------------------------------------------------ */
static int test_parameterized_query_limit(SQLHSTMT hStmt) {
  printf("\n--- Test: Parameterized query (LIMIT ?) ---\n");

  SQLRETURN rc = SQLPrepare(hStmt,
    (SQLCHAR *)"SELECT r_regionkey FROM region LIMIT ?", SQL_NTS);
  CHECK(rc, "SQLPrepare");

  /* Bind parameter: LIMIT 3 */
  SQLINTEGER limit_val = 3;
  rc = SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
                         0, 0, &limit_val, 0, NULL);
  CHECK(rc, "SQLBindParameter(LIMIT)");

  rc = SQLExecute(hStmt);
  CHECK(rc, "SQLExecute");

  int count = 0;
  SQLINTEGER result_key;
  SQLLEN ind_key;

  SQLBindCol(hStmt, 1, SQL_C_SLONG, &result_key, sizeof(result_key), &ind_key);

  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  r_regionkey=%d\n", (int)result_key);
    count++;
  }

  printf("  Total rows: %d\n", count);

  if (count != 3) {
    fprintf(stderr, "FAIL: expected 3 rows with LIMIT 3, got %d\n", count);
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test: SQLPrimaryKeys
 *   Verifies that SQLPrimaryKeys returns the correct primary key column
 *   for a table with a PRIMARY KEY constraint.
 * ------------------------------------------------------------------------ */
static int test_primary_keys(SQLHSTMT hStmt) {
  printf("\n--- Test: SQLPrimaryKeys for pk_test_customers ---\n");

  SQLRETURN rc = SQLPrimaryKeys(hStmt, NULL, 0, NULL, 0,
                                 (SQLCHAR *)"pk_test_customers", SQL_NTS);
  CHECK(rc, "SQLPrimaryKeys(pk_test_customers)");

  SQLCHAR table_cat[MAX_COL_LEN], table_schem[MAX_COL_LEN];
  SQLCHAR table_name[MAX_COL_LEN], column_name[MAX_COL_LEN];
  SQLCHAR pk_name[MAX_COL_LEN];
  SQLSMALLINT key_seq;
  SQLLEN ind[6];

  SQLBindCol(hStmt, 1, SQL_C_CHAR, table_cat, sizeof(table_cat), &ind[0]);
  SQLBindCol(hStmt, 2, SQL_C_CHAR, table_schem, sizeof(table_schem), &ind[1]);
  SQLBindCol(hStmt, 3, SQL_C_CHAR, table_name, sizeof(table_name), &ind[2]);
  SQLBindCol(hStmt, 4, SQL_C_CHAR, column_name, sizeof(column_name), &ind[3]);
  SQLBindCol(hStmt, 5, SQL_C_SSHORT, &key_seq, 0, &ind[4]);
  SQLBindCol(hStmt, 6, SQL_C_CHAR, pk_name, sizeof(pk_name), &ind[5]);

  int count = 0;
  int found_customer_id = 0;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  TABLE_CAT=%s TABLE_SCHEM=%s TABLE_NAME=%s COLUMN_NAME=%s KEY_SEQ=%d PK_NAME=%s\n",
           ind[0] == SQL_NULL_DATA ? "(null)" : (char *)table_cat,
           ind[1] == SQL_NULL_DATA ? "(null)" : (char *)table_schem,
           (char *)table_name, (char *)column_name, (int)key_seq,
           ind[5] == SQL_NULL_DATA ? "(null)" : (char *)pk_name);
    count++;

    if (strcasecmp((char *)column_name, "customer_id") == 0 && key_seq == 1) {
      found_customer_id = 1;
    }
  }

  printf("  Total rows: %d\n", count);

  if (count < 1) {
    fprintf(stderr, "FAIL: expected >= 1 primary key row, got %d\n", count);
    reset_stmt(hStmt);
    return 1;
  }
  if (!found_customer_id) {
    fprintf(stderr, "FAIL: customer_id with KEY_SEQ=1 not found\n");
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test: SQLForeignKeys (imported keys)
 *   Verifies that SQLForeignKeys returns the FK relationship when called
 *   with only the FK table specified (GetImportedKeys path).
 * ------------------------------------------------------------------------ */
static int test_foreign_keys_imported(SQLHSTMT hStmt) {
  printf("\n--- Test: SQLForeignKeys (imported keys for pk_test_orders) ---\n");

  SQLRETURN rc = SQLForeignKeys(hStmt,
                                 NULL, 0, NULL, 0, NULL, 0,     /* PK table: not specified */
                                 NULL, 0, NULL, 0,               /* FK cat/schema: not specified */
                                 (SQLCHAR *)"pk_test_orders", SQL_NTS);
  CHECK(rc, "SQLForeignKeys(imported keys)");

  SQLCHAR pktbl[MAX_COL_LEN], pkcol[MAX_COL_LEN];
  SQLCHAR fktbl[MAX_COL_LEN], fkcol[MAX_COL_LEN];
  SQLCHAR fk_name[MAX_COL_LEN], pk_name[MAX_COL_LEN];
  SQLSMALLINT key_seq, update_rule, delete_rule, deferrability;
  SQLLEN ind[14];

  SQLBindCol(hStmt, 1, SQL_C_CHAR, NULL, 0, &ind[0]);  /* PKTABLE_CAT */
  SQLBindCol(hStmt, 2, SQL_C_CHAR, NULL, 0, &ind[1]);  /* PKTABLE_SCHEM */
  SQLBindCol(hStmt, 3, SQL_C_CHAR, pktbl, sizeof(pktbl), &ind[2]);
  SQLBindCol(hStmt, 4, SQL_C_CHAR, pkcol, sizeof(pkcol), &ind[3]);
  SQLBindCol(hStmt, 5, SQL_C_CHAR, NULL, 0, &ind[4]);  /* FKTABLE_CAT */
  SQLBindCol(hStmt, 6, SQL_C_CHAR, NULL, 0, &ind[5]);  /* FKTABLE_SCHEM */
  SQLBindCol(hStmt, 7, SQL_C_CHAR, fktbl, sizeof(fktbl), &ind[6]);
  SQLBindCol(hStmt, 8, SQL_C_CHAR, fkcol, sizeof(fkcol), &ind[7]);
  SQLBindCol(hStmt, 9, SQL_C_SSHORT, &key_seq, 0, &ind[8]);
  SQLBindCol(hStmt, 10, SQL_C_SSHORT, &update_rule, 0, &ind[9]);
  SQLBindCol(hStmt, 11, SQL_C_SSHORT, &delete_rule, 0, &ind[10]);
  SQLBindCol(hStmt, 12, SQL_C_CHAR, fk_name, sizeof(fk_name), &ind[11]);
  SQLBindCol(hStmt, 13, SQL_C_CHAR, pk_name, sizeof(pk_name), &ind[12]);
  SQLBindCol(hStmt, 14, SQL_C_SSHORT, &deferrability, 0, &ind[13]);

  int count = 0;
  int found_fk = 0;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  PK: %s.%s -> FK: %s.%s  KEY_SEQ=%d UPDATE_RULE=%d DELETE_RULE=%d\n",
           (char *)pktbl, (char *)pkcol,
           (char *)fktbl, (char *)fkcol,
           (int)key_seq,
           ind[9] == SQL_NULL_DATA ? -1 : (int)update_rule,
           ind[10] == SQL_NULL_DATA ? -1 : (int)delete_rule);
    count++;

    /* Verify the expected FK relationship */
    if (strcasecmp((char *)pktbl, "pk_test_customers") == 0 &&
        strcasecmp((char *)pkcol, "customer_id") == 0 &&
        strcasecmp((char *)fktbl, "pk_test_orders") == 0 &&
        strcasecmp((char *)fkcol, "customer_id") == 0 &&
        key_seq == 1) {
      found_fk = 1;
    }
  }

  printf("  Total rows: %d\n", count);

  if (count < 1) {
    fprintf(stderr, "FAIL: expected >= 1 foreign key row, got %d\n", count);
    reset_stmt(hStmt);
    return 1;
  }
  if (!found_fk) {
    fprintf(stderr, "FAIL: expected FK pk_test_orders.customer_id -> pk_test_customers.customer_id not found\n");
    reset_stmt(hStmt);
    return 1;
  }

  printf("  PASS\n");
  reset_stmt(hStmt);
  return 0;
}

/* ---------------------------------------------------------------------------
 * Test: SQLForeignKeys (exported keys)
 *   Verifies that SQLForeignKeys returns the FK relationship when called
 *   with only the PK table specified (GetExportedKeys path).
 * ------------------------------------------------------------------------ */
static int test_foreign_keys_exported(SQLHSTMT hStmt) {
  printf("\n--- Test: SQLForeignKeys (exported keys for pk_test_customers) ---\n");

  SQLRETURN rc = SQLForeignKeys(hStmt,
                                 NULL, 0, NULL, 0,
                                 (SQLCHAR *)"pk_test_customers", SQL_NTS,
                                 NULL, 0, NULL, 0, NULL, 0);
  CHECK(rc, "SQLForeignKeys(exported keys)");

  SQLCHAR pktbl[MAX_COL_LEN], pkcol[MAX_COL_LEN];
  SQLCHAR fktbl[MAX_COL_LEN], fkcol[MAX_COL_LEN];
  SQLSMALLINT key_seq;
  SQLLEN ind[9];

  SQLBindCol(hStmt, 3, SQL_C_CHAR, pktbl, sizeof(pktbl), &ind[0]);
  SQLBindCol(hStmt, 4, SQL_C_CHAR, pkcol, sizeof(pkcol), &ind[1]);
  SQLBindCol(hStmt, 7, SQL_C_CHAR, fktbl, sizeof(fktbl), &ind[2]);
  SQLBindCol(hStmt, 8, SQL_C_CHAR, fkcol, sizeof(fkcol), &ind[3]);
  SQLBindCol(hStmt, 9, SQL_C_SSHORT, &key_seq, 0, &ind[4]);

  int count = 0;
  int found_fk = 0;
  while (SQLFetch(hStmt) == SQL_SUCCESS) {
    printf("  PK: %s.%s -> FK: %s.%s  KEY_SEQ=%d\n",
           (char *)pktbl, (char *)pkcol,
           (char *)fktbl, (char *)fkcol,
           (int)key_seq);
    count++;

    if (strcasecmp((char *)pktbl, "pk_test_customers") == 0 &&
        strcasecmp((char *)pkcol, "customer_id") == 0 &&
        strcasecmp((char *)fktbl, "pk_test_orders") == 0 &&
        strcasecmp((char *)fkcol, "customer_id") == 0 &&
        key_seq == 1) {
      found_fk = 1;
    }
  }

  printf("  Total rows: %d\n", count);

  if (count < 1) {
    fprintf(stderr, "FAIL: expected >= 1 exported key row, got %d\n", count);
    reset_stmt(hStmt);
    return 1;
  }
  if (!found_fk) {
    fprintf(stderr, "FAIL: expected exported FK pk_test_customers -> pk_test_orders not found\n");
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

  printf("=== Integration Tests (Metadata / Parameters) ===\n");

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

  /* --- Setup: create PK/FK test tables --- */
  printf("Setup: creating pk_test_customers and pk_test_orders...\n");
  exec_ignore(hStmt, "DROP TABLE IF EXISTS pk_test_orders");
  exec_ignore(hStmt, "DROP TABLE IF EXISTS pk_test_customers");
  if (exec_required(hStmt,
        "CREATE TABLE pk_test_customers "
        "(customer_id INTEGER PRIMARY KEY, name VARCHAR(100) NOT NULL)")) return 1;
  if (exec_required(hStmt,
        "CREATE TABLE pk_test_orders "
        "(order_id INTEGER PRIMARY KEY, "
        "customer_id INTEGER NOT NULL REFERENCES pk_test_customers(customer_id), "
        "amount DECIMAL(10,2))")) return 1;
  if (exec_required(hStmt,
        "INSERT INTO pk_test_customers VALUES (1, 'Alice'), (2, 'Bob')")) return 1;
  if (exec_required(hStmt,
        "INSERT INTO pk_test_orders VALUES (100, 1, 49.99), (101, 2, 19.99)")) return 1;

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

  /* Test 9: SQLNumParams after Prepare */
  if (test_num_params(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* Test 10: Parameterized query with integer parameter */
  if (test_parameterized_query_int(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* Test 11: Parameterized query with string parameter */
  if (test_parameterized_query_string(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* Test 12: Parameterized query with LIMIT ? (Power BI pattern) */
  if (test_parameterized_query_limit(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* Test 13: SQLPrimaryKeys */
  if (test_primary_keys(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* Test 14: SQLForeignKeys (imported keys) */
  if (test_foreign_keys_imported(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* Test 15: SQLForeignKeys (exported keys) */
  if (test_foreign_keys_exported(hStmt)) {
    g_failed++;
  } else {
    g_passed++;
  }

  /* --- Cleanup --- */
  exec_ignore(hStmt, "DROP TABLE IF EXISTS pk_test_orders");
  exec_ignore(hStmt, "DROP TABLE IF EXISTS pk_test_customers");
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

  printf("All tests passed.\n");
  return 0;
}
