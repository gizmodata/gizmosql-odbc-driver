/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 *
 * ODBC C Entry Points — thin shim that delegates to ODBCAbstraction classes.
 */

#include "driver_singleton.h"

#include <odbcabstraction/platform.h>
#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>
#include <sqlucode.h>

#include <odbcabstraction/odbc_impl/ODBCConnection.h>
#include <odbcabstraction/odbc_impl/ODBCDescriptor.h>
#include <odbcabstraction/odbc_impl/ODBCEnvironment.h>
#include <odbcabstraction/odbc_impl/ODBCHandle.h>
#include <odbcabstraction/odbc_impl/ODBCStatement.h>
#include <odbcabstraction/odbc_impl/AttributeUtils.h>
#include <odbcabstraction/odbc_impl/EncodingUtils.h>
#include <odbcabstraction/encoding.h>
#include <odbcabstraction/exceptions.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace ODBC;
using namespace driver::odbcabstraction;

// ============================================================================
// Unicode conversion helpers
// ============================================================================

static std::string SqlWCharToString(const SQLWCHAR *wstr, SQLSMALLINT len) {
  if (!wstr) return std::string();
  std::vector<uint8_t> utf8;
  if (len == SQL_NTS) {
    WcsToUtf8(wstr, &utf8);
  } else {
    WcsToUtf8(wstr, static_cast<size_t>(len) / GetSqlWCharSize(), &utf8);
  }
  return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
}

static void Utf8ToSqlWChar(const std::string &str, SQLWCHAR *buf,
                           SQLSMALLINT bufLen, SQLSMALLINT *outLen) {
  size_t bytesNeeded = ConvertToSqlWChar(str, buf, bufLen);
  if (outLen) {
    *outLen = static_cast<SQLSMALLINT>(bytesNeeded);
  }
}

static std::unique_ptr<std::string> ToOptionalString(const SQLCHAR *str,
                                                     SQLSMALLINT len) {
  if (!str) return nullptr;
  if (len == SQL_NTS) {
    return std::unique_ptr<std::string>(
        new std::string(reinterpret_cast<const char *>(str)));
  }
  return std::unique_ptr<std::string>(
      new std::string(reinterpret_cast<const char *>(str), len));
}

static std::unique_ptr<std::string> ToOptionalStringW(const SQLWCHAR *str,
                                                      SQLSMALLINT len) {
  if (!str) return nullptr;
  std::string s = SqlWCharToString(str, len);
  return std::unique_ptr<std::string>(new std::string(std::move(s)));
}

static std::string SqlCharToString(const SQLCHAR *str, SQLSMALLINT len) {
  if (!str) return std::string();
  if (len == SQL_NTS) {
    return std::string(reinterpret_cast<const char *>(str));
  }
  return std::string(reinterpret_cast<const char *>(str), len);
}

// ============================================================================
// Supported functions bitmap for SQLGetFunctions
// ============================================================================

static void SetFunction(SQLUSMALLINT *bitmap, SQLUSMALLINT funcId) {
  bitmap[funcId >> 4] |= (1 << (funcId & 0x0F));
}

static void FillFunctionBitmap(SQLUSMALLINT *bitmap) {
  memset(bitmap, 0, sizeof(SQLUSMALLINT) * SQL_API_ODBC3_ALL_FUNCTIONS_SIZE);

  // Handle management
  SetFunction(bitmap, SQL_API_SQLALLOCHANDLE);
  SetFunction(bitmap, SQL_API_SQLFREEHANDLE);
  SetFunction(bitmap, SQL_API_SQLFREESTMT);

  // Environment
  SetFunction(bitmap, SQL_API_SQLSETENVATTR);
  SetFunction(bitmap, SQL_API_SQLGETENVATTR);

  // Connection
  SetFunction(bitmap, SQL_API_SQLDRIVERCONNECT);
  SetFunction(bitmap, SQL_API_SQLCONNECT);
  SetFunction(bitmap, SQL_API_SQLBROWSECONNECT);
  SetFunction(bitmap, SQL_API_SQLDISCONNECT);
  SetFunction(bitmap, SQL_API_SQLGETINFO);
  SetFunction(bitmap, SQL_API_SQLSETCONNECTATTR);
  SetFunction(bitmap, SQL_API_SQLGETCONNECTATTR);
  SetFunction(bitmap, SQL_API_SQLGETFUNCTIONS);
  SetFunction(bitmap, SQL_API_SQLENDTRAN);
  SetFunction(bitmap, SQL_API_SQLNATIVESQL);

  // Statement execution
  SetFunction(bitmap, SQL_API_SQLPREPARE);
  SetFunction(bitmap, SQL_API_SQLEXECUTE);
  SetFunction(bitmap, SQL_API_SQLEXECDIRECT);
  SetFunction(bitmap, SQL_API_SQLCANCEL);

  // Results
  SetFunction(bitmap, SQL_API_SQLFETCH);
  SetFunction(bitmap, SQL_API_SQLFETCHSCROLL);
  SetFunction(bitmap, SQL_API_SQLEXTENDEDFETCH);
  SetFunction(bitmap, SQL_API_SQLGETDATA);
  SetFunction(bitmap, SQL_API_SQLBINDCOL);
  SetFunction(bitmap, SQL_API_SQLNUMRESULTCOLS);
  SetFunction(bitmap, SQL_API_SQLDESCRIBECOL);
  SetFunction(bitmap, SQL_API_SQLCOLATTRIBUTE);
  SetFunction(bitmap, SQL_API_SQLROWCOUNT);
  SetFunction(bitmap, SQL_API_SQLMORERESULTS);
  SetFunction(bitmap, SQL_API_SQLCLOSECURSOR);

  // Statement attributes
  SetFunction(bitmap, SQL_API_SQLSETSTMTATTR);
  SetFunction(bitmap, SQL_API_SQLGETSTMTATTR);

  // Descriptors
  SetFunction(bitmap, SQL_API_SQLGETDESCFIELD);
  SetFunction(bitmap, SQL_API_SQLSETDESCFIELD);
  SetFunction(bitmap, SQL_API_SQLGETDESCREC);
  SetFunction(bitmap, SQL_API_SQLSETDESCREC);
  SetFunction(bitmap, SQL_API_SQLCOPYDESC);

  // Catalog functions
  SetFunction(bitmap, SQL_API_SQLTABLES);
  SetFunction(bitmap, SQL_API_SQLCOLUMNS);
  SetFunction(bitmap, SQL_API_SQLGETTYPEINFO);
  SetFunction(bitmap, SQL_API_SQLPRIMARYKEYS);
  SetFunction(bitmap, SQL_API_SQLFOREIGNKEYS);
  SetFunction(bitmap, SQL_API_SQLSTATISTICS);
  SetFunction(bitmap, SQL_API_SQLSPECIALCOLUMNS);
  SetFunction(bitmap, SQL_API_SQLPROCEDURES);
  SetFunction(bitmap, SQL_API_SQLPROCEDURECOLUMNS);
  SetFunction(bitmap, SQL_API_SQLTABLEPRIVILEGES);
  SetFunction(bitmap, SQL_API_SQLCOLUMNPRIVILEGES);

  // Diagnostics
  SetFunction(bitmap, SQL_API_SQLGETDIAGREC);
  SetFunction(bitmap, SQL_API_SQLGETDIAGFIELD);

  // Parameters (stub but declared)
  SetFunction(bitmap, SQL_API_SQLBINDPARAMETER);
  SetFunction(bitmap, SQL_API_SQLNUMPARAMS);

  // Cursor
  SetFunction(bitmap, SQL_API_SQLGETCURSORNAME);
  SetFunction(bitmap, SQL_API_SQLSETCURSORNAME);

  // Bulk/Position
  SetFunction(bitmap, SQL_API_SQLBULKOPERATIONS);
  SetFunction(bitmap, SQL_API_SQLSETPOS);
}

// ============================================================================
// Handle Management
// ============================================================================

// Internal (static) implementations — these are NOT subject to symbol
// interposition by the Driver Manager.  The exported SQLAllocHandle /
// SQLFreeHandle entry points AND the ODBC 2.x compatibility wrappers
// (SQLAllocEnv, SQLAllocConnect, …) all call these directly, so the DM
// can never hijack the internal call chain.

static SQLRETURN AllocHandleImpl(SQLSMALLINT handleType,
                                 SQLHANDLE inputHandle,
                                 SQLHANDLE *outputHandle) {
  switch (handleType) {
  case SQL_HANDLE_ENV: {
    auto driver = gizmosql::odbc::GetGlobalDriver();
    auto *env = new ODBCEnvironment(driver);
    *outputHandle = reinterpret_cast<SQLHANDLE>(env);
    return SQL_SUCCESS;
  }
  case SQL_HANDLE_DBC:
    return ODBCEnvironment::ExecuteWithDiagnostics(
        inputHandle, SQL_SUCCESS, [&]() {
          auto *env = ODBCEnvironment::of(inputHandle);
          auto conn = env->CreateConnection();
          *outputHandle = reinterpret_cast<SQLHANDLE>(conn.get());
          return SQL_SUCCESS;
        });
  case SQL_HANDLE_STMT:
    return ODBCConnection::ExecuteWithDiagnostics(
        inputHandle, SQL_SUCCESS, [&]() {
          auto *conn = ODBCConnection::of(inputHandle);
          auto stmt = conn->createStatement();
          *outputHandle = reinterpret_cast<SQLHANDLE>(stmt.get());
          return SQL_SUCCESS;
        });
  case SQL_HANDLE_DESC:
    return ODBCConnection::ExecuteWithDiagnostics(
        inputHandle, SQL_SUCCESS, [&]() {
          auto *conn = ODBCConnection::of(inputHandle);
          auto desc = conn->createDescriptor();
          *outputHandle = reinterpret_cast<SQLHANDLE>(desc.get());
          return SQL_SUCCESS;
        });
  default:
    return SQL_ERROR;
  }
}

static SQLRETURN FreeHandleImpl(SQLSMALLINT handleType, SQLHANDLE handle) {
  if (!handle) return SQL_INVALID_HANDLE;

  switch (handleType) {
  case SQL_HANDLE_ENV: {
    auto *env = ODBCEnvironment::of(handle);
    delete env;
    return SQL_SUCCESS;
  }
  case SQL_HANDLE_DBC: {
    auto *conn = ODBCConnection::of(handle);
    SQLRETURN ret = ODBCConnection::ExecuteWithDiagnostics(
        handle, SQL_SUCCESS, [&]() {
          conn->releaseConnection();
          return SQL_SUCCESS;
        });
    conn->GetEnvironment().DropConnection(conn);
    return ret;
  }
  case SQL_HANDLE_STMT: {
    auto *stmt = ODBCStatement::of(handle);
    SQLRETURN ret = ODBCStatement::ExecuteWithDiagnostics(
        handle, SQL_SUCCESS, [&]() {
          stmt->releaseStatement();
          return SQL_SUCCESS;
        });
    // Drop the statement AFTER ExecuteWithDiagnostics returns.
    // releaseStatement() only closes the cursor; dropStatement() erases
    // the last shared_ptr which destroys the object.  Calling it inside
    // executeWithLock would be use-after-free (diagnostics + mutex).
    stmt->GetConnection().dropStatement(stmt);
    return ret;
  }
  case SQL_HANDLE_DESC: {
    auto *desc = ODBCDescriptor::of(handle);
    SQLRETURN ret = ODBCDescriptor::ExecuteWithDiagnostics(
        handle, SQL_SUCCESS, [&]() {
          desc->ReleaseDescriptor();
          return SQL_SUCCESS;
        });
    auto *owningConn = desc->GetOwningConnection();
    if (owningConn) {
      owningConn->dropDescriptor(desc);
    }
    return ret;
  }
  default:
    return SQL_ERROR;
  }
}

extern "C" {

SQLRETURN SQL_API SQLAllocHandle(SQLSMALLINT handleType, SQLHANDLE inputHandle,
                                SQLHANDLE *outputHandle) {
  return AllocHandleImpl(handleType, inputHandle, outputHandle);
}

SQLRETURN SQL_API SQLFreeHandle(SQLSMALLINT handleType, SQLHANDLE handle) {
  return FreeHandleImpl(handleType, handle);
}

SQLRETURN SQL_API SQLFreeStmt(SQLHSTMT hStmt, SQLUSMALLINT option) {
  switch (option) {
  case SQL_CLOSE:
    return ODBCStatement::ExecuteWithDiagnostics(
        hStmt, SQL_SUCCESS, [&]() {
          ODBCStatement::of(hStmt)->closeCursor(true);
          return SQL_SUCCESS;
        });
  case SQL_UNBIND: {
    if (!hStmt) return SQL_INVALID_HANDLE;
    auto *stmt = ODBCStatement::of(hStmt);
    auto *ard = stmt->GetARD();
    ard->GetRecords().clear();
    return SQL_SUCCESS;
  }
  case SQL_RESET_PARAMS:
    // No parameter support — no-op.
    return SQL_SUCCESS;
  case SQL_DROP:
    return FreeHandleImpl(SQL_HANDLE_STMT, hStmt);
  default:
    return SQL_ERROR;
  }
}

// ODBC 2.x compatibility wrappers — call internal functions directly
// to avoid symbol interposition by the Driver Manager.
SQLRETURN SQL_API SQLAllocEnv(SQLHENV *phEnv) {
  return AllocHandleImpl(SQL_HANDLE_ENV, SQL_NULL_HANDLE,
                         reinterpret_cast<SQLHANDLE *>(phEnv));
}

SQLRETURN SQL_API SQLAllocConnect(SQLHENV hEnv, SQLHDBC *phDbc) {
  return AllocHandleImpl(SQL_HANDLE_DBC, hEnv,
                         reinterpret_cast<SQLHANDLE *>(phDbc));
}

SQLRETURN SQL_API SQLAllocStmt(SQLHDBC hDbc, SQLHSTMT *phStmt) {
  return AllocHandleImpl(SQL_HANDLE_STMT, hDbc,
                         reinterpret_cast<SQLHANDLE *>(phStmt));
}

SQLRETURN SQL_API SQLFreeEnv(SQLHENV hEnv) {
  return FreeHandleImpl(SQL_HANDLE_ENV, hEnv);
}

SQLRETURN SQL_API SQLFreeConnect(SQLHDBC hDbc) {
  return FreeHandleImpl(SQL_HANDLE_DBC, hDbc);
}

// ============================================================================
// Environment
// ============================================================================

SQLRETURN SQL_API SQLSetEnvAttr(SQLHENV hEnv, SQLINTEGER attribute,
                               SQLPOINTER value, SQLINTEGER stringLength) {
  return ODBCEnvironment::ExecuteWithDiagnostics(
      hEnv, SQL_SUCCESS, [&]() {
        auto *env = ODBCEnvironment::of(hEnv);
        switch (attribute) {
        case SQL_ATTR_ODBC_VERSION:
          env->setODBCVersion(static_cast<SQLINTEGER>(
              reinterpret_cast<uintptr_t>(value)));
          break;
        case SQL_ATTR_CONNECTION_POOLING:
          env->setConnectionPooling(static_cast<SQLINTEGER>(
              reinterpret_cast<uintptr_t>(value)));
          break;
        case SQL_ATTR_CP_MATCH:
          // Accept but ignore.
          break;
        case SQL_ATTR_OUTPUT_NTS:
          // ODBC always NUL-terminates output. Accept SQL_TRUE only.
          if (reinterpret_cast<uintptr_t>(value) != SQL_TRUE) {
            throw DriverException("Optional feature not implemented", "HYC00");
          }
          break;
        default:
          throw DriverException("Invalid attribute", "HY092");
        }
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLGetEnvAttr(SQLHENV hEnv, SQLINTEGER attribute,
                               SQLPOINTER value, SQLINTEGER bufferLength,
                               SQLINTEGER *stringLength) {
  return ODBCEnvironment::ExecuteWithDiagnostics(
      hEnv, SQL_SUCCESS, [&]() {
        auto *env = ODBCEnvironment::of(hEnv);
        switch (attribute) {
        case SQL_ATTR_ODBC_VERSION:
          GetAttribute(env->getODBCVersion(), value, bufferLength,
                       stringLength);
          break;
        case SQL_ATTR_CONNECTION_POOLING:
          GetAttribute(env->getConnectionPooling(), value, bufferLength,
                       stringLength);
          break;
        case SQL_ATTR_CP_MATCH:
          GetAttribute(static_cast<SQLUINTEGER>(SQL_CP_STRICT_MATCH), value,
                       bufferLength, stringLength);
          break;
        case SQL_ATTR_OUTPUT_NTS:
          GetAttribute(static_cast<SQLINTEGER>(SQL_TRUE), value, bufferLength,
                       stringLength);
          break;
        default:
          throw DriverException("Invalid attribute", "HY092");
        }
        return SQL_SUCCESS;
      });
}

// ============================================================================
// Connection
// ============================================================================

SQLRETURN SQL_API SQLDriverConnect(SQLHDBC hDbc, SQLHWND hWnd,
                                  SQLCHAR *connStrIn,
                                  SQLSMALLINT connStrInLen,
                                  SQLCHAR *connStrOut,
                                  SQLSMALLINT connStrOutMax,
                                  SQLSMALLINT *connStrOutLen,
                                  SQLUSMALLINT driverCompletion) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS, [&]() {
        auto *conn = ODBCConnection::of(hDbc);
        std::string connStr = SqlCharToString(connStrIn, connStrInLen);

        Connection::ConnPropertyMap properties;
        std::string dsn =
            ODBCConnection::getPropertiesFromConnString(connStr, properties);

        std::vector<std::string> missing;
        conn->connect(dsn, properties, missing);

        // Write back the connection string.
        if (connStrOut && connStrOutMax > 0) {
          size_t toCopy =
              std::min(static_cast<size_t>(connStrOutMax - 1), connStr.size());
          memcpy(connStrOut, connStr.c_str(), toCopy);
          connStrOut[toCopy] = '\0';
          if (connStrOutLen) *connStrOutLen = static_cast<SQLSMALLINT>(connStr.size());
        } else if (connStrOutLen) {
          *connStrOutLen = static_cast<SQLSMALLINT>(connStr.size());
        }

        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLDriverConnectW(SQLHDBC hDbc, SQLHWND hWnd,
                                   SQLWCHAR *connStrIn,
                                   SQLSMALLINT connStrInLen,
                                   SQLWCHAR *connStrOut,
                                   SQLSMALLINT connStrOutMax,
                                   SQLSMALLINT *connStrOutLen,
                                   SQLUSMALLINT driverCompletion) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS, [&]() {
        auto *conn = ODBCConnection::of(hDbc);
        std::string connStr = SqlWCharToString(connStrIn, connStrInLen);

        Connection::ConnPropertyMap properties;
        std::string dsn =
            ODBCConnection::getPropertiesFromConnString(connStr, properties);

        std::vector<std::string> missing;
        conn->connect(dsn, properties, missing);

        if (connStrOut && connStrOutMax > 0) {
          SQLSMALLINT outLen = 0;
          Utf8ToSqlWChar(connStr, connStrOut,
                         static_cast<SQLSMALLINT>(connStrOutMax * GetSqlWCharSize()),
                         &outLen);
          if (connStrOutLen) *connStrOutLen = static_cast<SQLSMALLINT>(outLen / GetSqlWCharSize());
        } else if (connStrOutLen) {
          *connStrOutLen = static_cast<SQLSMALLINT>(connStr.size());
        }

        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLConnect(SQLHDBC hDbc, SQLCHAR *serverName,
                            SQLSMALLINT nameLen1, SQLCHAR *userName,
                            SQLSMALLINT nameLen2, SQLCHAR *auth,
                            SQLSMALLINT nameLen3) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS, [&]() {
        auto *conn = ODBCConnection::of(hDbc);
        std::string dsn = SqlCharToString(serverName, nameLen1);

        Connection::ConnPropertyMap properties;
        // Load properties from the DSN in odbc.ini.
        std::string connStr = "DSN=" + dsn;
        ODBCConnection::getPropertiesFromConnString(connStr, properties);

        if (userName) {
          properties["UID"] = SqlCharToString(userName, nameLen2);
        }
        if (auth) {
          properties["PWD"] = SqlCharToString(auth, nameLen3);
        }

        std::vector<std::string> missing;
        conn->connect(dsn, properties, missing);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLConnectW(SQLHDBC hDbc, SQLWCHAR *serverName,
                             SQLSMALLINT nameLen1, SQLWCHAR *userName,
                             SQLSMALLINT nameLen2, SQLWCHAR *auth,
                             SQLSMALLINT nameLen3) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS, [&]() {
        auto *conn = ODBCConnection::of(hDbc);
        std::string dsn = SqlWCharToString(serverName, nameLen1);

        Connection::ConnPropertyMap properties;
        std::string connStr = "DSN=" + dsn;
        ODBCConnection::getPropertiesFromConnString(connStr, properties);

        if (userName) {
          properties["UID"] = SqlWCharToString(userName, nameLen2);
        }
        if (auth) {
          properties["PWD"] = SqlWCharToString(auth, nameLen3);
        }

        std::vector<std::string> missing;
        conn->connect(dsn, properties, missing);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLBrowseConnect(SQLHDBC hDbc, SQLCHAR *connStrIn,
                                  SQLSMALLINT connStrInLen,
                                  SQLCHAR *connStrOut,
                                  SQLSMALLINT connStrOutMax,
                                  SQLSMALLINT *connStrOutLen) {
  return SQLDriverConnect(hDbc, nullptr, connStrIn, connStrInLen, connStrOut,
                          connStrOutMax, connStrOutLen, SQL_DRIVER_NOPROMPT);
}

SQLRETURN SQL_API SQLBrowseConnectW(SQLHDBC hDbc, SQLWCHAR *connStrIn,
                                   SQLSMALLINT connStrInLen,
                                   SQLWCHAR *connStrOut,
                                   SQLSMALLINT connStrOutMax,
                                   SQLSMALLINT *connStrOutLen) {
  return SQLDriverConnectW(hDbc, nullptr, connStrIn, connStrInLen, connStrOut,
                           connStrOutMax, connStrOutLen, SQL_DRIVER_NOPROMPT);
}

SQLRETURN SQL_API SQLDisconnect(SQLHDBC hDbc) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS,
      [&]() {
        ODBCConnection::of(hDbc)->disconnect();
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLGetInfo(SQLHDBC hDbc, SQLUSMALLINT infoType,
                            SQLPOINTER value, SQLSMALLINT bufferLength,
                            SQLSMALLINT *stringLength) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS, [&]() {
        ODBCConnection::of(hDbc)->GetInfo(infoType, value, bufferLength,
                                          stringLength, false);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLGetInfoW(SQLHDBC hDbc, SQLUSMALLINT infoType,
                             SQLPOINTER value, SQLSMALLINT bufferLength,
                             SQLSMALLINT *stringLength) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS, [&]() {
        ODBCConnection::of(hDbc)->GetInfo(infoType, value, bufferLength,
                                          stringLength, true);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLSetConnectAttr(SQLHDBC hDbc, SQLINTEGER attribute,
                                   SQLPOINTER value,
                                   SQLINTEGER stringLength) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS, [&]() {
        ODBCConnection::of(hDbc)->SetConnectAttr(attribute, value,
                                                  stringLength, false);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLSetConnectAttrW(SQLHDBC hDbc, SQLINTEGER attribute,
                                    SQLPOINTER value,
                                    SQLINTEGER stringLength) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS, [&]() {
        ODBCConnection::of(hDbc)->SetConnectAttr(attribute, value,
                                                  stringLength, true);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLGetConnectAttr(SQLHDBC hDbc, SQLINTEGER attribute,
                                   SQLPOINTER value, SQLINTEGER bufferLength,
                                   SQLINTEGER *stringLength) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS, [&]() {
        ODBCConnection::of(hDbc)->GetConnectAttr(attribute, value,
                                                  bufferLength, stringLength,
                                                  false);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLGetConnectAttrW(SQLHDBC hDbc, SQLINTEGER attribute,
                                    SQLPOINTER value, SQLINTEGER bufferLength,
                                    SQLINTEGER *stringLength) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS, [&]() {
        ODBCConnection::of(hDbc)->GetConnectAttr(attribute, value,
                                                  bufferLength, stringLength,
                                                  true);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLGetFunctions(SQLHDBC hDbc, SQLUSMALLINT functionId,
                                 SQLUSMALLINT *supported) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS, [&]() {
        if (functionId == SQL_API_ODBC3_ALL_FUNCTIONS) {
          FillFunctionBitmap(supported);
        } else if (functionId == SQL_API_ALL_FUNCTIONS) {
          // ODBC 2.x all-functions array (100 entries)
          memset(supported, 0, sizeof(SQLUSMALLINT) * 100);
          SQLUSMALLINT bitmap[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE];
          FillFunctionBitmap(bitmap);
          for (SQLUSMALLINT i = 0; i < 100; i++) {
            if (SQL_FUNC_EXISTS(bitmap, i)) {
              supported[i] = SQL_TRUE;
            }
          }
        } else {
          SQLUSMALLINT bitmap[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE];
          FillFunctionBitmap(bitmap);
          *supported = SQL_FUNC_EXISTS(bitmap, functionId) ? SQL_TRUE : SQL_FALSE;
        }
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLEndTran(SQLSMALLINT handleType, SQLHANDLE handle,
                            SQLSMALLINT completionType) {
  if (completionType == SQL_COMMIT) {
    return SQL_SUCCESS; // Auto-commit mode, no-op.
  }

  // SQL_ROLLBACK — not supported
  if (handleType == SQL_HANDLE_ENV) {
    return ODBCEnvironment::ExecuteWithDiagnostics(
        handle, SQL_ERROR, [&]() -> SQLRETURN {
          throw DriverException("Transactions not supported", "HYC00");
        });
  }
  return ODBCConnection::ExecuteWithDiagnostics(
      handle, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("Transactions not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLNativeSql(SQLHDBC hDbc, SQLCHAR *inSql,
                              SQLINTEGER inSqlLen, SQLCHAR *outSql,
                              SQLINTEGER outSqlMax,
                              SQLINTEGER *outSqlLen) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS, [&]() {
        std::string sql = SqlCharToString(
            inSql, inSqlLen == SQL_NTS ? SQL_NTS : static_cast<SQLSMALLINT>(inSqlLen));
        if (outSqlLen) *outSqlLen = static_cast<SQLINTEGER>(sql.size());
        if (outSql && outSqlMax > 0) {
          size_t toCopy =
              std::min(static_cast<size_t>(outSqlMax - 1), sql.size());
          memcpy(outSql, sql.c_str(), toCopy);
          outSql[toCopy] = '\0';
        }
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLNativeSqlW(SQLHDBC hDbc, SQLWCHAR *inSql,
                               SQLINTEGER inSqlLen, SQLWCHAR *outSql,
                               SQLINTEGER outSqlMax,
                               SQLINTEGER *outSqlLen) {
  return ODBCConnection::ExecuteWithDiagnostics(
      hDbc, SQL_SUCCESS, [&]() {
        std::string sql = SqlWCharToString(
            inSql, inSqlLen == SQL_NTS ? SQL_NTS : static_cast<SQLSMALLINT>(inSqlLen));
        if (outSql && outSqlMax > 0) {
          SQLSMALLINT bytesWritten = 0;
          Utf8ToSqlWChar(sql, outSql,
                         static_cast<SQLSMALLINT>(outSqlMax * GetSqlWCharSize()),
                         &bytesWritten);
          if (outSqlLen) *outSqlLen = static_cast<SQLINTEGER>(bytesWritten / GetSqlWCharSize());
        } else if (outSqlLen) {
          *outSqlLen = static_cast<SQLINTEGER>(sql.size());
        }
        return SQL_SUCCESS;
      });
}

// ============================================================================
// Statement Execution
// ============================================================================

SQLRETURN SQL_API SQLPrepare(SQLHSTMT hStmt, SQLCHAR *sqlStr,
                            SQLINTEGER sqlStrLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        std::string sql = SqlCharToString(
            sqlStr, sqlStrLen == SQL_NTS ? SQL_NTS : static_cast<SQLSMALLINT>(sqlStrLen));
        ODBCStatement::of(hStmt)->Prepare(sql);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLPrepareW(SQLHSTMT hStmt, SQLWCHAR *sqlStr,
                             SQLINTEGER sqlStrLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        std::string sql = SqlWCharToString(
            sqlStr, sqlStrLen == SQL_NTS ? SQL_NTS : static_cast<SQLSMALLINT>(sqlStrLen));
        ODBCStatement::of(hStmt)->Prepare(sql);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLExecute(SQLHSTMT hStmt) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        ODBCStatement::of(hStmt)->ExecutePrepared();
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLExecDirect(SQLHSTMT hStmt, SQLCHAR *sqlStr,
                               SQLINTEGER sqlStrLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        std::string sql = SqlCharToString(
            sqlStr, sqlStrLen == SQL_NTS ? SQL_NTS : static_cast<SQLSMALLINT>(sqlStrLen));
        ODBCStatement::of(hStmt)->ExecuteDirect(sql);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLExecDirectW(SQLHSTMT hStmt, SQLWCHAR *sqlStr,
                                SQLINTEGER sqlStrLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        std::string sql = SqlWCharToString(
            sqlStr, sqlStrLen == SQL_NTS ? SQL_NTS : static_cast<SQLSMALLINT>(sqlStrLen));
        ODBCStatement::of(hStmt)->ExecuteDirect(sql);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLCancel(SQLHSTMT hStmt) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        ODBCStatement::of(hStmt)->Cancel();
        return SQL_SUCCESS;
      });
}

// ============================================================================
// Results
// ============================================================================

SQLRETURN SQL_API SQLFetch(SQLHSTMT hStmt) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto *stmt = ODBCStatement::of(hStmt);
        bool hasData = stmt->Fetch(stmt->GetARD()->GetArraySize());
        return hasData ? SQL_SUCCESS : SQL_NO_DATA;
      });
}

SQLRETURN SQL_API SQLFetchScroll(SQLHSTMT hStmt, SQLSMALLINT orientation,
                                SQLLEN offset) {
  if (orientation != SQL_FETCH_NEXT) {
    return ODBCStatement::ExecuteWithDiagnostics(
        hStmt, SQL_ERROR, [&]() -> SQLRETURN {
          throw DriverException("Fetch type out of range. Only SQL_FETCH_NEXT is supported.", "HY106");
        });
  }
  return SQLFetch(hStmt);
}

SQLRETURN SQL_API SQLExtendedFetch(SQLHSTMT hStmt, SQLUSMALLINT orientation,
                                  SQLLEN offset, SQLULEN *rowCount,
                                  SQLUSMALLINT *rowStatusArray) {
  if (orientation != SQL_FETCH_NEXT) {
    return ODBCStatement::ExecuteWithDiagnostics(
        hStmt, SQL_ERROR, [&]() -> SQLRETURN {
          throw DriverException("Fetch type out of range. Only SQL_FETCH_NEXT is supported.", "HY106");
        });
  }
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto *stmt = ODBCStatement::of(hStmt);
        bool hasData = stmt->Fetch(stmt->GetRowsetSize());
        return hasData ? SQL_SUCCESS : SQL_NO_DATA;
      });
}

SQLRETURN SQL_API SQLGetData(SQLHSTMT hStmt, SQLUSMALLINT colNum,
                            SQLSMALLINT targetType, SQLPOINTER targetValue,
                            SQLLEN bufferLength, SQLLEN *strLenOrInd) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        bool hasData = ODBCStatement::of(hStmt)->GetData(
            colNum, targetType, targetValue, bufferLength, strLenOrInd);
        return hasData ? SQL_SUCCESS : SQL_NO_DATA;
      });
}

SQLRETURN SQL_API SQLBindCol(SQLHSTMT hStmt, SQLUSMALLINT colNum,
                            SQLSMALLINT targetType, SQLPOINTER targetValue,
                            SQLLEN bufferLength, SQLLEN *strLenOrInd) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        ODBCStatement::of(hStmt)->GetARD()->BindCol(
            colNum, targetType, targetValue, bufferLength, strLenOrInd);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLNumResultCols(SQLHSTMT hStmt, SQLSMALLINT *colCount) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto *ird = ODBCStatement::of(hStmt)->GetIRD();
        *colCount = static_cast<SQLSMALLINT>(ird->GetRecords().size());
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLDescribeCol(SQLHSTMT hStmt, SQLUSMALLINT colNum,
                                SQLCHAR *colName, SQLSMALLINT bufferLength,
                                SQLSMALLINT *nameLength,
                                SQLSMALLINT *dataType, SQLULEN *colSize,
                                SQLSMALLINT *decimalDigits,
                                SQLSMALLINT *nullable) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto *ird = ODBCStatement::of(hStmt)->GetIRD();
        const auto &records = ird->GetRecords();
        if (colNum < 1 || colNum > records.size()) {
          throw DriverException("Invalid descriptor index", "07009");
        }
        const auto &rec = records[colNum - 1];

        if (colName || nameLength) {
          SQLRETURN rc = GetAttributeUTF8(rec.m_name, colName, bufferLength,
                                          nameLength);
          if (rc == SQL_SUCCESS_WITH_INFO) {
            ird->GetDiagnostics().AddTruncationWarning();
          }
        }
        if (dataType) *dataType = rec.m_conciseType;
        if (colSize) *colSize = rec.m_length;
        if (decimalDigits) *decimalDigits = rec.m_scale;
        if (nullable) *nullable = rec.m_nullable;

        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLDescribeColW(SQLHSTMT hStmt, SQLUSMALLINT colNum,
                                 SQLWCHAR *colName, SQLSMALLINT bufferLength,
                                 SQLSMALLINT *nameLength,
                                 SQLSMALLINT *dataType, SQLULEN *colSize,
                                 SQLSMALLINT *decimalDigits,
                                 SQLSMALLINT *nullable) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto *ird = ODBCStatement::of(hStmt)->GetIRD();
        const auto &records = ird->GetRecords();
        if (colNum < 1 || colNum > records.size()) {
          throw DriverException("Invalid descriptor index", "07009");
        }
        const auto &rec = records[colNum - 1];

        if (colName || nameLength) {
          SQLRETURN rc = GetAttributeSQLWCHAR(rec.m_name, true, colName,
                                              bufferLength, nameLength);
          if (rc == SQL_SUCCESS_WITH_INFO) {
            ird->GetDiagnostics().AddTruncationWarning();
          }
        }
        if (dataType) *dataType = rec.m_conciseType;
        if (colSize) *colSize = rec.m_length;
        if (decimalDigits) *decimalDigits = rec.m_scale;
        if (nullable) *nullable = rec.m_nullable;

        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLColAttribute(SQLHSTMT hStmt, SQLUSMALLINT colNum,
                                 SQLUSMALLINT fieldId, SQLPOINTER charAttr,
                                 SQLSMALLINT bufferLength,
                                 SQLSMALLINT *stringLength,
                                 SQLLEN *numericAttr) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto *ird = ODBCStatement::of(hStmt)->GetIRD();
        SQLINTEGER intLen = 0;
        ird->GetField(colNum, fieldId, charAttr, bufferLength, &intLen);
        if (stringLength) *stringLength = static_cast<SQLSMALLINT>(intLen);
        // For numeric attributes, the value is returned in charAttr for
        // string fields; for integer fields it's already written into charAttr
        // which we also copy to numericAttr.
        if (numericAttr && charAttr) {
          *numericAttr = reinterpret_cast<SQLLEN>(
              *reinterpret_cast<SQLLEN *>(charAttr));
        }
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLColAttributeW(SQLHSTMT hStmt, SQLUSMALLINT colNum,
                                  SQLUSMALLINT fieldId, SQLPOINTER charAttr,
                                  SQLSMALLINT bufferLength,
                                  SQLSMALLINT *stringLength,
                                  SQLLEN *numericAttr) {
  // The W variant — field values that are strings are returned as SQLWCHAR.
  // ODBCDescriptor::GetField handles this if we pass the right buffer.
  // For now, use the same path as ANSI since GetField writes strings into the buffer.
  return SQLColAttribute(hStmt, colNum, fieldId, charAttr, bufferLength,
                         stringLength, numericAttr);
}

SQLRETURN SQL_API SQLRowCount(SQLHSTMT hStmt, SQLLEN *rowCount) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        if (rowCount) {
          long count = ODBCStatement::of(hStmt)->GetUpdateCount();
          *rowCount = (count >= 0) ? static_cast<SQLLEN>(count) : 0;
        }
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLMoreResults(SQLHSTMT hStmt) {
  return SQL_NO_DATA;
}

SQLRETURN SQL_API SQLCloseCursor(SQLHSTMT hStmt) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        ODBCStatement::of(hStmt)->closeCursor(false);
        return SQL_SUCCESS;
      });
}

// ============================================================================
// Statement Attributes
// ============================================================================

SQLRETURN SQL_API SQLSetStmtAttr(SQLHSTMT hStmt, SQLINTEGER attribute,
                                SQLPOINTER value, SQLINTEGER stringLength) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        ODBCStatement::of(hStmt)->SetStmtAttr(attribute, value, stringLength,
                                               false);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLSetStmtAttrW(SQLHSTMT hStmt, SQLINTEGER attribute,
                                 SQLPOINTER value, SQLINTEGER stringLength) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        ODBCStatement::of(hStmt)->SetStmtAttr(attribute, value, stringLength,
                                               true);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLGetStmtAttr(SQLHSTMT hStmt, SQLINTEGER attribute,
                                SQLPOINTER value, SQLINTEGER bufferLength,
                                SQLINTEGER *stringLength) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        ODBCStatement::of(hStmt)->GetStmtAttr(attribute, value, bufferLength,
                                               stringLength, false);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLGetStmtAttrW(SQLHSTMT hStmt, SQLINTEGER attribute,
                                 SQLPOINTER value, SQLINTEGER bufferLength,
                                 SQLINTEGER *stringLength) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        ODBCStatement::of(hStmt)->GetStmtAttr(attribute, value, bufferLength,
                                               stringLength, true);
        return SQL_SUCCESS;
      });
}

// ============================================================================
// Descriptors
// ============================================================================

SQLRETURN SQL_API SQLGetDescField(SQLHDESC hDesc, SQLSMALLINT recNum,
                                 SQLSMALLINT fieldId, SQLPOINTER value,
                                 SQLINTEGER bufferLength,
                                 SQLINTEGER *stringLength) {
  return ODBCDescriptor::ExecuteWithDiagnostics(
      hDesc, SQL_SUCCESS, [&]() {
        auto *desc = ODBCDescriptor::of(hDesc);
        if (recNum == 0) {
          desc->GetHeaderField(fieldId, value, bufferLength, stringLength);
        } else {
          desc->GetField(recNum, fieldId, value, bufferLength, stringLength);
        }
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLGetDescFieldW(SQLHDESC hDesc, SQLSMALLINT recNum,
                                  SQLSMALLINT fieldId, SQLPOINTER value,
                                  SQLINTEGER bufferLength,
                                  SQLINTEGER *stringLength) {
  return SQLGetDescField(hDesc, recNum, fieldId, value, bufferLength,
                         stringLength);
}

SQLRETURN SQL_API SQLSetDescField(SQLHDESC hDesc, SQLSMALLINT recNum,
                                 SQLSMALLINT fieldId, SQLPOINTER value,
                                 SQLINTEGER bufferLength) {
  return ODBCDescriptor::ExecuteWithDiagnostics(
      hDesc, SQL_SUCCESS, [&]() {
        auto *desc = ODBCDescriptor::of(hDesc);
        if (recNum == 0) {
          desc->SetHeaderField(fieldId, value, bufferLength);
        } else {
          desc->SetField(recNum, fieldId, value, bufferLength);
        }
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLSetDescFieldW(SQLHDESC hDesc, SQLSMALLINT recNum,
                                  SQLSMALLINT fieldId, SQLPOINTER value,
                                  SQLINTEGER bufferLength) {
  return SQLSetDescField(hDesc, recNum, fieldId, value, bufferLength);
}

SQLRETURN SQL_API SQLGetDescRec(SQLHDESC hDesc, SQLSMALLINT recNum,
                               SQLCHAR *name, SQLSMALLINT bufferLength,
                               SQLSMALLINT *stringLength,
                               SQLSMALLINT *type, SQLSMALLINT *subType,
                               SQLLEN *length, SQLSMALLINT *precision,
                               SQLSMALLINT *scale, SQLSMALLINT *nullable) {
  return ODBCDescriptor::ExecuteWithDiagnostics(
      hDesc, SQL_SUCCESS, [&]() {
        auto *desc = ODBCDescriptor::of(hDesc);
        const auto &records = desc->GetRecords();
        if (recNum < 1 || static_cast<size_t>(recNum) > records.size()) {
          return SQL_NO_DATA;
        }
        const auto &rec = records[recNum - 1];

        if (name || stringLength) {
          GetAttributeUTF8(rec.m_name, name, bufferLength, stringLength);
        }
        if (type) *type = rec.m_type;
        if (subType) *subType = rec.m_datetimeIntervalCode;
        if (length) *length = static_cast<SQLLEN>(rec.m_length);
        if (precision) *precision = rec.m_precision;
        if (scale) *scale = rec.m_scale;
        if (nullable) *nullable = rec.m_nullable;

        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLGetDescRecW(SQLHDESC hDesc, SQLSMALLINT recNum,
                                SQLWCHAR *name, SQLSMALLINT bufferLength,
                                SQLSMALLINT *stringLength,
                                SQLSMALLINT *type, SQLSMALLINT *subType,
                                SQLLEN *length, SQLSMALLINT *precision,
                                SQLSMALLINT *scale, SQLSMALLINT *nullable) {
  return ODBCDescriptor::ExecuteWithDiagnostics(
      hDesc, SQL_SUCCESS, [&]() {
        auto *desc = ODBCDescriptor::of(hDesc);
        const auto &records = desc->GetRecords();
        if (recNum < 1 || static_cast<size_t>(recNum) > records.size()) {
          return SQL_NO_DATA;
        }
        const auto &rec = records[recNum - 1];

        if (name || stringLength) {
          GetAttributeSQLWCHAR(rec.m_name, true, name, bufferLength,
                               stringLength);
        }
        if (type) *type = rec.m_type;
        if (subType) *subType = rec.m_datetimeIntervalCode;
        if (length) *length = static_cast<SQLLEN>(rec.m_length);
        if (precision) *precision = rec.m_precision;
        if (scale) *scale = rec.m_scale;
        if (nullable) *nullable = rec.m_nullable;

        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLSetDescRec(SQLHDESC hDesc, SQLSMALLINT recNum,
                               SQLSMALLINT type, SQLSMALLINT subType,
                               SQLLEN length, SQLSMALLINT precision,
                               SQLSMALLINT scale, SQLPOINTER dataPtr,
                               SQLLEN *stringLengthPtr,
                               SQLLEN *indicatorPtr) {
  return ODBCDescriptor::ExecuteWithDiagnostics(
      hDesc, SQL_SUCCESS, [&]() {
        auto *desc = ODBCDescriptor::of(hDesc);
        desc->SetField(recNum, SQL_DESC_TYPE, reinterpret_cast<SQLPOINTER>(static_cast<SQLLEN>(type)), 0);
        desc->SetField(recNum, SQL_DESC_DATETIME_INTERVAL_CODE,
                       reinterpret_cast<SQLPOINTER>(static_cast<SQLLEN>(subType)), 0);
        desc->SetField(recNum, SQL_DESC_OCTET_LENGTH, reinterpret_cast<SQLPOINTER>(length), 0);
        desc->SetField(recNum, SQL_DESC_PRECISION,
                       reinterpret_cast<SQLPOINTER>(static_cast<SQLLEN>(precision)), 0);
        desc->SetField(recNum, SQL_DESC_SCALE,
                       reinterpret_cast<SQLPOINTER>(static_cast<SQLLEN>(scale)), 0);
        desc->SetField(recNum, SQL_DESC_DATA_PTR, dataPtr, 0);
        desc->SetField(recNum, SQL_DESC_OCTET_LENGTH_PTR,
                       reinterpret_cast<SQLPOINTER>(stringLengthPtr), 0);
        desc->SetField(recNum, SQL_DESC_INDICATOR_PTR,
                       reinterpret_cast<SQLPOINTER>(indicatorPtr), 0);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLCopyDesc(SQLHDESC hDescSrc, SQLHDESC hDescDst) {
  if (!hDescSrc || !hDescDst) return SQL_INVALID_HANDLE;

  return ODBCDescriptor::ExecuteWithDiagnostics(
      hDescDst, SQL_SUCCESS, [&]() {
        auto *src = ODBCDescriptor::of(hDescSrc);
        auto *dst = ODBCDescriptor::of(hDescDst);
        dst->GetRecords() = src->GetRecords();
        dst->NotifyBindingsHaveChanged();
        return SQL_SUCCESS;
      });
}

// ============================================================================
// Catalog Functions
// ============================================================================

SQLRETURN SQL_API SQLTables(SQLHSTMT hStmt, SQLCHAR *catalog,
                           SQLSMALLINT catalogLen, SQLCHAR *schema,
                           SQLSMALLINT schemaLen, SQLCHAR *table,
                           SQLSMALLINT tableLen, SQLCHAR *tableType,
                           SQLSMALLINT tableTypeLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto cat = ToOptionalString(catalog, catalogLen);
        auto sch = ToOptionalString(schema, schemaLen);
        auto tbl = ToOptionalString(table, tableLen);
        auto typ = ToOptionalString(tableType, tableTypeLen);
        ODBCStatement::of(hStmt)->GetTables(cat.get(), sch.get(), tbl.get(),
                                             typ.get());
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLTablesW(SQLHSTMT hStmt, SQLWCHAR *catalog,
                            SQLSMALLINT catalogLen, SQLWCHAR *schema,
                            SQLSMALLINT schemaLen, SQLWCHAR *table,
                            SQLSMALLINT tableLen, SQLWCHAR *tableType,
                            SQLSMALLINT tableTypeLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto cat = ToOptionalStringW(catalog, catalogLen);
        auto sch = ToOptionalStringW(schema, schemaLen);
        auto tbl = ToOptionalStringW(table, tableLen);
        auto typ = ToOptionalStringW(tableType, tableTypeLen);
        ODBCStatement::of(hStmt)->GetTables(cat.get(), sch.get(), tbl.get(),
                                             typ.get());
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLColumns(SQLHSTMT hStmt, SQLCHAR *catalog,
                            SQLSMALLINT catalogLen, SQLCHAR *schema,
                            SQLSMALLINT schemaLen, SQLCHAR *table,
                            SQLSMALLINT tableLen, SQLCHAR *column,
                            SQLSMALLINT columnLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto cat = ToOptionalString(catalog, catalogLen);
        auto sch = ToOptionalString(schema, schemaLen);
        auto tbl = ToOptionalString(table, tableLen);
        auto col = ToOptionalString(column, columnLen);
        ODBCStatement::of(hStmt)->GetColumns(cat.get(), sch.get(), tbl.get(),
                                              col.get());
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLColumnsW(SQLHSTMT hStmt, SQLWCHAR *catalog,
                             SQLSMALLINT catalogLen, SQLWCHAR *schema,
                             SQLSMALLINT schemaLen, SQLWCHAR *table,
                             SQLSMALLINT tableLen, SQLWCHAR *column,
                             SQLSMALLINT columnLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto cat = ToOptionalStringW(catalog, catalogLen);
        auto sch = ToOptionalStringW(schema, schemaLen);
        auto tbl = ToOptionalStringW(table, tableLen);
        auto col = ToOptionalStringW(column, columnLen);
        ODBCStatement::of(hStmt)->GetColumns(cat.get(), sch.get(), tbl.get(),
                                              col.get());
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLGetTypeInfo(SQLHSTMT hStmt, SQLSMALLINT dataType) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        ODBCStatement::of(hStmt)->GetTypeInfo(dataType);
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLGetTypeInfoW(SQLHSTMT hStmt, SQLSMALLINT dataType) {
  return SQLGetTypeInfo(hStmt, dataType);
}

SQLRETURN SQL_API SQLPrimaryKeys(SQLHSTMT hStmt, SQLCHAR *catalog,
                                SQLSMALLINT catalogLen, SQLCHAR *schema,
                                SQLSMALLINT schemaLen, SQLCHAR *table,
                                SQLSMALLINT tableLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto cat = ToOptionalString(catalog, catalogLen);
        auto sch = ToOptionalString(schema, schemaLen);
        auto tbl = ToOptionalString(table, tableLen);
        ODBCStatement::of(hStmt)->GetPrimaryKeys(cat.get(), sch.get(),
                                                  tbl.get());
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLPrimaryKeysW(SQLHSTMT hStmt, SQLWCHAR *catalog,
                                 SQLSMALLINT catalogLen, SQLWCHAR *schema,
                                 SQLSMALLINT schemaLen, SQLWCHAR *table,
                                 SQLSMALLINT tableLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto cat = ToOptionalStringW(catalog, catalogLen);
        auto sch = ToOptionalStringW(schema, schemaLen);
        auto tbl = ToOptionalStringW(table, tableLen);
        ODBCStatement::of(hStmt)->GetPrimaryKeys(cat.get(), sch.get(),
                                                  tbl.get());
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLForeignKeys(
    SQLHSTMT hStmt, SQLCHAR *pkCatalog, SQLSMALLINT pkCatalogLen,
    SQLCHAR *pkSchema, SQLSMALLINT pkSchemaLen, SQLCHAR *pkTable,
    SQLSMALLINT pkTableLen, SQLCHAR *fkCatalog, SQLSMALLINT fkCatalogLen,
    SQLCHAR *fkSchema, SQLSMALLINT fkSchemaLen, SQLCHAR *fkTable,
    SQLSMALLINT fkTableLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto pkCat = ToOptionalString(pkCatalog, pkCatalogLen);
        auto pkSch = ToOptionalString(pkSchema, pkSchemaLen);
        auto pkTbl = ToOptionalString(pkTable, pkTableLen);
        auto fkCat = ToOptionalString(fkCatalog, fkCatalogLen);
        auto fkSch = ToOptionalString(fkSchema, fkSchemaLen);
        auto fkTbl = ToOptionalString(fkTable, fkTableLen);
        ODBCStatement::of(hStmt)->GetForeignKeys(
            pkCat.get(), pkSch.get(), pkTbl.get(), fkCat.get(), fkSch.get(),
            fkTbl.get());
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLForeignKeysW(
    SQLHSTMT hStmt, SQLWCHAR *pkCatalog, SQLSMALLINT pkCatalogLen,
    SQLWCHAR *pkSchema, SQLSMALLINT pkSchemaLen, SQLWCHAR *pkTable,
    SQLSMALLINT pkTableLen, SQLWCHAR *fkCatalog, SQLSMALLINT fkCatalogLen,
    SQLWCHAR *fkSchema, SQLSMALLINT fkSchemaLen, SQLWCHAR *fkTable,
    SQLSMALLINT fkTableLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        auto pkCat = ToOptionalStringW(pkCatalog, pkCatalogLen);
        auto pkSch = ToOptionalStringW(pkSchema, pkSchemaLen);
        auto pkTbl = ToOptionalStringW(pkTable, pkTableLen);
        auto fkCat = ToOptionalStringW(fkCatalog, fkCatalogLen);
        auto fkSch = ToOptionalStringW(fkSchema, fkSchemaLen);
        auto fkTbl = ToOptionalStringW(fkTable, fkTableLen);
        ODBCStatement::of(hStmt)->GetForeignKeys(
            pkCat.get(), pkSch.get(), pkTbl.get(), fkCat.get(), fkSch.get(),
            fkTbl.get());
        return SQL_SUCCESS;
      });
}

// Catalog stubs — return SQL_ERROR (not supported)
SQLRETURN SQL_API SQLStatistics(SQLHSTMT hStmt, SQLCHAR *catalog,
                               SQLSMALLINT catalogLen, SQLCHAR *schema,
                               SQLSMALLINT schemaLen, SQLCHAR *table,
                               SQLSMALLINT tableLen, SQLUSMALLINT unique,
                               SQLUSMALLINT reserved) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLStatistics not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLStatisticsW(SQLHSTMT hStmt, SQLWCHAR *catalog,
                                SQLSMALLINT catalogLen, SQLWCHAR *schema,
                                SQLSMALLINT schemaLen, SQLWCHAR *table,
                                SQLSMALLINT tableLen, SQLUSMALLINT unique,
                                SQLUSMALLINT reserved) {
  return SQLStatistics(hStmt, nullptr, 0, nullptr, 0, nullptr, 0, unique,
                       reserved);
}

SQLRETURN SQL_API SQLSpecialColumns(SQLHSTMT hStmt, SQLUSMALLINT idType,
                                   SQLCHAR *catalog, SQLSMALLINT catalogLen,
                                   SQLCHAR *schema, SQLSMALLINT schemaLen,
                                   SQLCHAR *table, SQLSMALLINT tableLen,
                                   SQLUSMALLINT scope,
                                   SQLUSMALLINT nullable) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLSpecialColumns not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLSpecialColumnsW(SQLHSTMT hStmt, SQLUSMALLINT idType,
                                    SQLWCHAR *catalog, SQLSMALLINT catalogLen,
                                    SQLWCHAR *schema, SQLSMALLINT schemaLen,
                                    SQLWCHAR *table, SQLSMALLINT tableLen,
                                    SQLUSMALLINT scope,
                                    SQLUSMALLINT nullable) {
  return SQLSpecialColumns(hStmt, idType, nullptr, 0, nullptr, 0, nullptr, 0,
                           scope, nullable);
}

SQLRETURN SQL_API SQLProcedures(SQLHSTMT hStmt, SQLCHAR *catalog,
                               SQLSMALLINT catalogLen, SQLCHAR *schema,
                               SQLSMALLINT schemaLen, SQLCHAR *proc,
                               SQLSMALLINT procLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLProcedures not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLProceduresW(SQLHSTMT hStmt, SQLWCHAR *catalog,
                                SQLSMALLINT catalogLen, SQLWCHAR *schema,
                                SQLSMALLINT schemaLen, SQLWCHAR *proc,
                                SQLSMALLINT procLen) {
  return SQLProcedures(hStmt, nullptr, 0, nullptr, 0, nullptr, 0);
}

SQLRETURN SQL_API SQLProcedureColumns(SQLHSTMT hStmt, SQLCHAR *catalog,
                                     SQLSMALLINT catalogLen, SQLCHAR *schema,
                                     SQLSMALLINT schemaLen, SQLCHAR *proc,
                                     SQLSMALLINT procLen, SQLCHAR *column,
                                     SQLSMALLINT columnLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLProcedureColumns not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLProcedureColumnsW(SQLHSTMT hStmt, SQLWCHAR *catalog,
                                      SQLSMALLINT catalogLen, SQLWCHAR *schema,
                                      SQLSMALLINT schemaLen, SQLWCHAR *proc,
                                      SQLSMALLINT procLen, SQLWCHAR *column,
                                      SQLSMALLINT columnLen) {
  return SQLProcedureColumns(hStmt, nullptr, 0, nullptr, 0, nullptr, 0,
                             nullptr, 0);
}

SQLRETURN SQL_API SQLTablePrivileges(SQLHSTMT hStmt, SQLCHAR *catalog,
                                    SQLSMALLINT catalogLen, SQLCHAR *schema,
                                    SQLSMALLINT schemaLen, SQLCHAR *table,
                                    SQLSMALLINT tableLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLTablePrivileges not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLTablePrivilegesW(SQLHSTMT hStmt, SQLWCHAR *catalog,
                                     SQLSMALLINT catalogLen, SQLWCHAR *schema,
                                     SQLSMALLINT schemaLen, SQLWCHAR *table,
                                     SQLSMALLINT tableLen) {
  return SQLTablePrivileges(hStmt, nullptr, 0, nullptr, 0, nullptr, 0);
}

SQLRETURN SQL_API SQLColumnPrivileges(SQLHSTMT hStmt, SQLCHAR *catalog,
                                     SQLSMALLINT catalogLen, SQLCHAR *schema,
                                     SQLSMALLINT schemaLen, SQLCHAR *table,
                                     SQLSMALLINT tableLen, SQLCHAR *column,
                                     SQLSMALLINT columnLen) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLColumnPrivileges not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLColumnPrivilegesW(SQLHSTMT hStmt, SQLWCHAR *catalog,
                                      SQLSMALLINT catalogLen, SQLWCHAR *schema,
                                      SQLSMALLINT schemaLen, SQLWCHAR *table,
                                      SQLSMALLINT tableLen, SQLWCHAR *column,
                                      SQLSMALLINT columnLen) {
  return SQLColumnPrivileges(hStmt, nullptr, 0, nullptr, 0, nullptr, 0,
                             nullptr, 0);
}

// ============================================================================
// Diagnostics — must NOT clear diagnostics
// ============================================================================

SQLRETURN SQL_API SQLGetDiagRec(SQLSMALLINT handleType, SQLHANDLE handle,
                               SQLSMALLINT recNumber, SQLCHAR *sqlState,
                               SQLINTEGER *nativeError, SQLCHAR *messageText,
                               SQLSMALLINT bufferLength,
                               SQLSMALLINT *textLength) {
  if (!handle) return SQL_INVALID_HANDLE;
  if (recNumber < 1) return SQL_ERROR;

  Diagnostics *diag = nullptr;
  switch (handleType) {
  case SQL_HANDLE_ENV:
    diag = &ODBCEnvironment::of(handle)->GetDiagnostics();
    break;
  case SQL_HANDLE_DBC:
    diag = &ODBCConnection::of(handle)->GetDiagnostics();
    break;
  case SQL_HANDLE_STMT:
    diag = &ODBCStatement::of(handle)->GetDiagnostics();
    break;
  case SQL_HANDLE_DESC:
    diag = &ODBCDescriptor::of(handle)->GetDiagnostics();
    break;
  default:
    return SQL_ERROR;
  }

  uint32_t idx = static_cast<uint32_t>(recNumber - 1);
  if (!diag->HasRecord(idx)) return SQL_NO_DATA;

  if (sqlState) {
    std::string state = diag->GetSQLState(idx);
    memcpy(sqlState, state.c_str(), std::min(state.size(), static_cast<size_t>(5)));
    sqlState[5] = '\0';
  }
  if (nativeError) *nativeError = diag->GetNativeError(idx);

  std::string msg = diag->GetMessageText(idx);
  if (textLength) *textLength = static_cast<SQLSMALLINT>(msg.size());
  if (messageText && bufferLength > 0) {
    size_t toCopy =
        std::min(static_cast<size_t>(bufferLength - 1), msg.size());
    memcpy(messageText, msg.c_str(), toCopy);
    messageText[toCopy] = '\0';
    if (static_cast<size_t>(bufferLength) <= msg.size()) {
      return SQL_SUCCESS_WITH_INFO;
    }
  }

  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetDiagRecW(SQLSMALLINT handleType, SQLHANDLE handle,
                                SQLSMALLINT recNumber, SQLWCHAR *sqlState,
                                SQLINTEGER *nativeError, SQLWCHAR *messageText,
                                SQLSMALLINT bufferLength,
                                SQLSMALLINT *textLength) {
  if (!handle) return SQL_INVALID_HANDLE;
  if (recNumber < 1) return SQL_ERROR;

  Diagnostics *diag = nullptr;
  switch (handleType) {
  case SQL_HANDLE_ENV:
    diag = &ODBCEnvironment::of(handle)->GetDiagnostics();
    break;
  case SQL_HANDLE_DBC:
    diag = &ODBCConnection::of(handle)->GetDiagnostics();
    break;
  case SQL_HANDLE_STMT:
    diag = &ODBCStatement::of(handle)->GetDiagnostics();
    break;
  case SQL_HANDLE_DESC:
    diag = &ODBCDescriptor::of(handle)->GetDiagnostics();
    break;
  default:
    return SQL_ERROR;
  }

  uint32_t idx = static_cast<uint32_t>(recNumber - 1);
  if (!diag->HasRecord(idx)) return SQL_NO_DATA;

  if (sqlState) {
    std::string state = diag->GetSQLState(idx);
    // SQL state is always 5 chars
    SQLSMALLINT stateOutLen = 0;
    Utf8ToSqlWChar(state, sqlState,
                   static_cast<SQLSMALLINT>(6 * GetSqlWCharSize()),
                   &stateOutLen);
  }
  if (nativeError) *nativeError = diag->GetNativeError(idx);

  std::string msg = diag->GetMessageText(idx);
  if (messageText && bufferLength > 0) {
    SQLSMALLINT bytesWritten = 0;
    Utf8ToSqlWChar(msg, messageText,
                   static_cast<SQLSMALLINT>(bufferLength * GetSqlWCharSize()),
                   &bytesWritten);
    if (textLength) *textLength = static_cast<SQLSMALLINT>(bytesWritten / GetSqlWCharSize());
    // Check for truncation.
    if (static_cast<size_t>(bufferLength) * GetSqlWCharSize() <
        msg.size() * GetSqlWCharSize() + GetSqlWCharSize()) {
      return SQL_SUCCESS_WITH_INFO;
    }
  } else if (textLength) {
    *textLength = static_cast<SQLSMALLINT>(msg.size());
  }

  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetDiagField(SQLSMALLINT handleType, SQLHANDLE handle,
                                 SQLSMALLINT recNumber,
                                 SQLSMALLINT diagIdentifier,
                                 SQLPOINTER diagInfo,
                                 SQLSMALLINT bufferLength,
                                 SQLSMALLINT *stringLength) {
  if (!handle) return SQL_INVALID_HANDLE;

  Diagnostics *diag = nullptr;
  switch (handleType) {
  case SQL_HANDLE_ENV:
    diag = &ODBCEnvironment::of(handle)->GetDiagnostics();
    break;
  case SQL_HANDLE_DBC:
    diag = &ODBCConnection::of(handle)->GetDiagnostics();
    break;
  case SQL_HANDLE_STMT:
    diag = &ODBCStatement::of(handle)->GetDiagnostics();
    break;
  case SQL_HANDLE_DESC:
    diag = &ODBCDescriptor::of(handle)->GetDiagnostics();
    break;
  default:
    return SQL_ERROR;
  }

  // Header fields (recNumber = 0)
  if (diagIdentifier == SQL_DIAG_NUMBER) {
    if (diagInfo)
      *reinterpret_cast<SQLINTEGER *>(diagInfo) =
          static_cast<SQLINTEGER>(diag->GetRecordCount());
    return SQL_SUCCESS;
  }
  if (diagIdentifier == SQL_DIAG_RETURNCODE) {
    // The driver manager handles this.
    return SQL_ERROR;
  }

  // Record fields
  uint32_t idx = static_cast<uint32_t>(recNumber - 1);
  if (!diag->HasRecord(idx)) return SQL_NO_DATA;

  switch (diagIdentifier) {
  case SQL_DIAG_SQLSTATE: {
    std::string state = diag->GetSQLState(idx);
    if (diagInfo && bufferLength > 0) {
      size_t toCopy =
          std::min(static_cast<size_t>(bufferLength - 1), state.size());
      memcpy(diagInfo, state.c_str(), toCopy);
      reinterpret_cast<char *>(diagInfo)[toCopy] = '\0';
    }
    if (stringLength) *stringLength = static_cast<SQLSMALLINT>(state.size());
    return SQL_SUCCESS;
  }
  case SQL_DIAG_NATIVE: {
    if (diagInfo)
      *reinterpret_cast<SQLINTEGER *>(diagInfo) = diag->GetNativeError(idx);
    return SQL_SUCCESS;
  }
  case SQL_DIAG_MESSAGE_TEXT: {
    std::string msg = diag->GetMessageText(idx);
    if (diagInfo && bufferLength > 0) {
      size_t toCopy =
          std::min(static_cast<size_t>(bufferLength - 1), msg.size());
      memcpy(diagInfo, msg.c_str(), toCopy);
      reinterpret_cast<char *>(diagInfo)[toCopy] = '\0';
    }
    if (stringLength) *stringLength = static_cast<SQLSMALLINT>(msg.size());
    if (diagInfo && static_cast<size_t>(bufferLength) <= msg.size()) {
      return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
  }
  case SQL_DIAG_CLASS_ORIGIN:
  case SQL_DIAG_SUBCLASS_ORIGIN: {
    std::string origin = "ISO 9075";
    std::string state = diag->GetSQLState(idx);
    if (state.size() >= 2 && state[0] == 'I' && state[1] == 'M') {
      origin = "ODBC 3.0";
    }
    if (diagInfo && bufferLength > 0) {
      size_t toCopy =
          std::min(static_cast<size_t>(bufferLength - 1), origin.size());
      memcpy(diagInfo, origin.c_str(), toCopy);
      reinterpret_cast<char *>(diagInfo)[toCopy] = '\0';
    }
    if (stringLength) *stringLength = static_cast<SQLSMALLINT>(origin.size());
    return SQL_SUCCESS;
  }
  case SQL_DIAG_SERVER_NAME:
  case SQL_DIAG_CONNECTION_NAME: {
    if (diagInfo && bufferLength > 0) {
      reinterpret_cast<char *>(diagInfo)[0] = '\0';
    }
    if (stringLength) *stringLength = 0;
    return SQL_SUCCESS;
  }
  default:
    return SQL_ERROR;
  }
}

SQLRETURN SQL_API SQLGetDiagFieldW(SQLSMALLINT handleType, SQLHANDLE handle,
                                  SQLSMALLINT recNumber,
                                  SQLSMALLINT diagIdentifier,
                                  SQLPOINTER diagInfo,
                                  SQLSMALLINT bufferLength,
                                  SQLSMALLINT *stringLength) {
  // For most fields, delegate to ANSI version.
  // The Driver Manager usually handles W→A conversion for diagnostics.
  return SQLGetDiagField(handleType, handle, recNumber, diagIdentifier,
                         diagInfo, bufferLength, stringLength);
}

// ODBC 2.x SQLError
SQLRETURN SQL_API SQLError(SQLHENV hEnv, SQLHDBC hDbc, SQLHSTMT hStmt,
                          SQLCHAR *sqlState, SQLINTEGER *nativeError,
                          SQLCHAR *messageText, SQLSMALLINT bufferLength,
                          SQLSMALLINT *textLength) {
  SQLSMALLINT handleType;
  SQLHANDLE handle;
  if (hStmt) {
    handleType = SQL_HANDLE_STMT;
    handle = hStmt;
  } else if (hDbc) {
    handleType = SQL_HANDLE_DBC;
    handle = hDbc;
  } else if (hEnv) {
    handleType = SQL_HANDLE_ENV;
    handle = hEnv;
  } else {
    return SQL_INVALID_HANDLE;
  }

  // SQLError iterates through records starting at 1.
  // The Driver Manager typically manages the iteration counter.
  return SQLGetDiagRec(handleType, handle, 1, sqlState, nativeError,
                       messageText, bufferLength, textLength);
}

SQLRETURN SQL_API SQLErrorW(SQLHENV hEnv, SQLHDBC hDbc, SQLHSTMT hStmt,
                           SQLWCHAR *sqlState, SQLINTEGER *nativeError,
                           SQLWCHAR *messageText, SQLSMALLINT bufferLength,
                           SQLSMALLINT *textLength) {
  SQLSMALLINT handleType;
  SQLHANDLE handle;
  if (hStmt) {
    handleType = SQL_HANDLE_STMT;
    handle = hStmt;
  } else if (hDbc) {
    handleType = SQL_HANDLE_DBC;
    handle = hDbc;
  } else if (hEnv) {
    handleType = SQL_HANDLE_ENV;
    handle = hEnv;
  } else {
    return SQL_INVALID_HANDLE;
  }

  return SQLGetDiagRecW(handleType, handle, 1, sqlState, nativeError,
                        messageText, bufferLength, textLength);
}

// ============================================================================
// Parameter Stubs (not supported)
// ============================================================================

SQLRETURN SQL_API SQLBindParameter(SQLHSTMT hStmt, SQLUSMALLINT paramNum,
                                  SQLSMALLINT ioType, SQLSMALLINT valueType,
                                  SQLSMALLINT paramType, SQLULEN colSize,
                                  SQLSMALLINT decDigits, SQLPOINTER paramValue,
                                  SQLLEN bufferLength,
                                  SQLLEN *strLenOrInd) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLBindParameter not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLDescribeParam(SQLHSTMT hStmt, SQLUSMALLINT paramNum,
                                  SQLSMALLINT *dataType, SQLULEN *paramSize,
                                  SQLSMALLINT *decDigits,
                                  SQLSMALLINT *nullable) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLDescribeParam not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLParamData(SQLHSTMT hStmt, SQLPOINTER *value) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLParamData not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLPutData(SQLHSTMT hStmt, SQLPOINTER data,
                            SQLLEN strLenOrInd) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLPutData not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLNumParams(SQLHSTMT hStmt, SQLSMALLINT *paramCount) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        if (paramCount) *paramCount = 0;
        return SQL_SUCCESS;
      });
}

// ============================================================================
// Cursor Name Stubs
// ============================================================================

SQLRETURN SQL_API SQLGetCursorName(SQLHSTMT hStmt, SQLCHAR *cursorName,
                                  SQLSMALLINT bufferLength,
                                  SQLSMALLINT *nameLength) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        std::string name = "SQL_CUR";
        if (nameLength) *nameLength = static_cast<SQLSMALLINT>(name.size());
        if (cursorName && bufferLength > 0) {
          size_t toCopy =
              std::min(static_cast<size_t>(bufferLength - 1), name.size());
          memcpy(cursorName, name.c_str(), toCopy);
          cursorName[toCopy] = '\0';
        }
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLGetCursorNameW(SQLHSTMT hStmt, SQLWCHAR *cursorName,
                                   SQLSMALLINT bufferLength,
                                   SQLSMALLINT *nameLength) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_SUCCESS, [&]() {
        std::string name = "SQL_CUR";
        if (cursorName && bufferLength > 0) {
          SQLSMALLINT outLen = 0;
          Utf8ToSqlWChar(name, cursorName,
                         static_cast<SQLSMALLINT>(bufferLength * GetSqlWCharSize()),
                         &outLen);
          if (nameLength) *nameLength = static_cast<SQLSMALLINT>(outLen / GetSqlWCharSize());
        } else if (nameLength) {
          *nameLength = static_cast<SQLSMALLINT>(name.size());
        }
        return SQL_SUCCESS;
      });
}

SQLRETURN SQL_API SQLSetCursorName(SQLHSTMT hStmt, SQLCHAR *cursorName,
                                  SQLSMALLINT nameLength) {
  // Accept but ignore — we don't support positioned updates.
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLSetCursorNameW(SQLHSTMT hStmt, SQLWCHAR *cursorName,
                                   SQLSMALLINT nameLength) {
  return SQL_SUCCESS;
}

// ============================================================================
// Bulk / Position Stubs
// ============================================================================

SQLRETURN SQL_API SQLBulkOperations(SQLHSTMT hStmt,
                                   SQLSMALLINT operation) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLBulkOperations not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLSetPos(SQLHSTMT hStmt, SQLSETPOSIROW rowNumber,
                           SQLUSMALLINT operation, SQLUSMALLINT lockType) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLSetPos not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLSetScrollOptions(SQLHSTMT hStmt,
                                     SQLUSMALLINT concurrency,
                                     SQLLEN crowKeyset,
                                     SQLUSMALLINT crowRowset) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLSetScrollOptions not supported", "HYC00");
      });
}

// ============================================================================
// ODBC 2.x Deprecated Functions
// ============================================================================

SQLRETURN SQL_API SQLParamOptions(SQLHSTMT hStmt, SQLULEN crowRow,
                                 SQLULEN *pirow) {
  return ODBCStatement::ExecuteWithDiagnostics(
      hStmt, SQL_ERROR, [&]() -> SQLRETURN {
        throw DriverException("SQLParamOptions not supported", "HYC00");
      });
}

SQLRETURN SQL_API SQLColAttributes(SQLHSTMT hStmt, SQLUSMALLINT colNum,
                                  SQLUSMALLINT fieldId, SQLPOINTER charAttr,
                                  SQLSMALLINT bufferLength,
                                  SQLSMALLINT *stringLength,
                                  SQLLEN *numericAttr) {
  return SQLColAttribute(hStmt, colNum, fieldId, charAttr, bufferLength,
                         stringLength, numericAttr);
}

SQLRETURN SQL_API SQLColAttributesW(SQLHSTMT hStmt, SQLUSMALLINT colNum,
                                   SQLUSMALLINT fieldId, SQLPOINTER charAttr,
                                   SQLSMALLINT bufferLength,
                                   SQLSMALLINT *stringLength,
                                   SQLLEN *numericAttr) {
  return SQLColAttributeW(hStmt, colNum, fieldId, charAttr, bufferLength,
                          stringLength, numericAttr);
}

SQLRETURN SQL_API SQLGetConnectOption(SQLHDBC hDbc, SQLUSMALLINT option,
                                     SQLPOINTER value) {
  return SQLGetConnectAttr(hDbc, static_cast<SQLINTEGER>(option), value,
                           SQL_MAX_OPTION_STRING_LENGTH, nullptr);
}

SQLRETURN SQL_API SQLGetConnectOptionW(SQLHDBC hDbc, SQLUSMALLINT option,
                                      SQLPOINTER value) {
  return SQLGetConnectAttrW(hDbc, static_cast<SQLINTEGER>(option), value,
                            SQL_MAX_OPTION_STRING_LENGTH, nullptr);
}

SQLRETURN SQL_API SQLSetConnectOption(SQLHDBC hDbc, SQLUSMALLINT option,
                                     SQLULEN value) {
  return SQLSetConnectAttr(hDbc, static_cast<SQLINTEGER>(option),
                           reinterpret_cast<SQLPOINTER>(value),
                           SQL_IS_UINTEGER);
}

SQLRETURN SQL_API SQLSetConnectOptionW(SQLHDBC hDbc, SQLUSMALLINT option,
                                      SQLULEN value) {
  return SQLSetConnectAttrW(hDbc, static_cast<SQLINTEGER>(option),
                            reinterpret_cast<SQLPOINTER>(value),
                            SQL_IS_UINTEGER);
}

SQLRETURN SQL_API SQLGetStmtOption(SQLHSTMT hStmt, SQLUSMALLINT option,
                                  SQLPOINTER value) {
  return SQLGetStmtAttr(hStmt, static_cast<SQLINTEGER>(option), value,
                        SQL_MAX_OPTION_STRING_LENGTH, nullptr);
}

SQLRETURN SQL_API SQLSetStmtOption(SQLHSTMT hStmt, SQLUSMALLINT option,
                                  SQLULEN value) {
  return SQLSetStmtAttr(hStmt, static_cast<SQLINTEGER>(option),
                        reinterpret_cast<SQLPOINTER>(value), SQL_IS_UINTEGER);
}

SQLRETURN SQL_API SQLTransact(SQLHENV hEnv, SQLHDBC hDbc,
                             SQLUSMALLINT completionType) {
  if (hDbc) {
    return SQLEndTran(SQL_HANDLE_DBC, hDbc, completionType);
  }
  if (hEnv) {
    return SQLEndTran(SQL_HANDLE_ENV, hEnv, completionType);
  }
  return SQL_INVALID_HANDLE;
}

} // extern "C"
