/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "flight_sql_statement.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <odbcabstraction/platform.h>
#include "flight_sql_result_set.h"
#include "flight_sql_result_set_metadata.h"
#include "flight_sql_statement_get_columns.h"
#include "flight_sql_statement_get_tables.h"
#include "flight_sql_statement_get_type_info.h"
#include "record_batch_transformer.h"
#include "utils.h"
#include <arrow/io/memory.h>
#include <arrow/builder.h>
#include <arrow/record_batch.h>
#include <arrow/type.h>
#include <sql.h>
#include <sqlext.h>

#include <boost/optional.hpp>
#include <utility>
#include <odbcabstraction/exceptions.h>
#include <odbcabstraction/encoding.h>

// timegm is POSIX; Windows provides _mkgmtime instead
#ifdef _WIN32
#define portable_timegm _mkgmtime
#else
#define portable_timegm timegm
#endif

namespace driver {
namespace flight_sql {

using arrow::Result;
using arrow::Status;
using arrow::flight::FlightCallOptions;
using arrow::flight::FlightClientOptions;
using arrow::flight::FlightInfo;
using arrow::flight::Location;
using arrow::flight::TimeoutDuration;
using arrow::flight::sql::FlightSqlClient;
using arrow::flight::sql::PreparedStatement;
using driver::odbcabstraction::DriverException;
using driver::odbcabstraction::ResultSet;
using driver::odbcabstraction::ResultSetMetadata;
using driver::odbcabstraction::Statement;

namespace {

void ClosePreparedStatementIfAny(
    std::shared_ptr<arrow::flight::sql::PreparedStatement>
        &prepared_statement,
    const FlightCallOptions &call_options) {
  if (prepared_statement != nullptr) {
    ThrowIfNotOK(prepared_statement->Close(call_options));
    prepared_statement.reset();
  }
}

} // namespace

FlightSqlStatement::FlightSqlStatement(
    const odbcabstraction::Diagnostics& diagnostics,
    FlightSqlClient &sql_client,
    FlightCallOptions call_options,
    const odbcabstraction::MetadataSettings& metadata_settings)
    : diagnostics_("GizmoData", diagnostics.GetDataSourceComponent(), diagnostics.GetOdbcVersion()),
      sql_client_(sql_client), call_options_(std::move(call_options)), metadata_settings_(metadata_settings) {
  attribute_[METADATA_ID] = static_cast<size_t>(SQL_FALSE);
  attribute_[MAX_LENGTH] = static_cast<size_t>(0);
  attribute_[NOSCAN] = static_cast<size_t>(SQL_NOSCAN_OFF);
  attribute_[QUERY_TIMEOUT] = static_cast<size_t>(0);
  call_options_.timeout = TimeoutDuration{-1};
}

FlightSqlStatement::~FlightSqlStatement() {
  // Explicitly close the prepared statement with auth headers before destruction.
  // Arrow 23's PreparedStatement destructor calls Close() with empty options,
  // which fails when the server requires authentication.
  if (prepared_statement_ != nullptr) {
    auto status = prepared_statement_->Close(call_options_);
    // Ignore errors during destruction — the statement is going away regardless.
    (void)status;
    prepared_statement_.reset();
  }
}

bool FlightSqlStatement::SetAttribute(StatementAttributeId attribute,
                                      const Attribute &value) {
  switch (attribute) {
  case METADATA_ID:
    return CheckIfSetToOnlyValidValue(value, static_cast<size_t>(SQL_FALSE));
  case NOSCAN:
    return CheckIfSetToOnlyValidValue(value, static_cast<size_t>(SQL_NOSCAN_OFF));
  case MAX_LENGTH:
    return CheckIfSetToOnlyValidValue(value, static_cast<size_t>(0));
  case QUERY_TIMEOUT:
    if (boost::get<size_t>(value) > 0) {
      call_options_.timeout =
          TimeoutDuration{static_cast<double>(boost::get<size_t>(value))};
    } else {
      call_options_.timeout = TimeoutDuration{-1};
      // Intentional fall-through.
    }
  default:
    attribute_[attribute] = value;
    return true;
  }
}

boost::optional<Statement::Attribute>
FlightSqlStatement::GetAttribute(StatementAttributeId attribute) {
  const auto &it = attribute_.find(attribute);
  return boost::make_optional(it != attribute_.end(), it->second);
}

boost::optional<std::shared_ptr<ResultSetMetadata>>
FlightSqlStatement::Prepare(const std::string &query) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  Result<std::shared_ptr<PreparedStatement>> result =
      sql_client_.Prepare(call_options_, query);
  ThrowIfNotOK(result.status());

  prepared_statement_ = *result;

  const auto &result_set_metadata =
      std::make_shared<FlightSqlResultSetMetadata>(
          prepared_statement_->dataset_schema(), metadata_settings_);
  return boost::optional<std::shared_ptr<ResultSetMetadata>>(
      result_set_metadata);
}

bool FlightSqlStatement::ExecutePrepared() {
  assert(prepared_statement_.get() != nullptr);

  Result<std::shared_ptr<FlightInfo>> result = prepared_statement_->Execute(call_options_);
  ThrowIfNotOK(result.status());

  flight_info_ = result.ValueOrDie();

  update_count_ = flight_info_->total_records();
  current_result_set_ = std::make_shared<FlightSqlResultSet>(
      sql_client_, call_options_, flight_info_, nullptr, diagnostics_, metadata_settings_);

  return true;
}

bool FlightSqlStatement::Execute(const std::string &query) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  Result<std::shared_ptr<FlightInfo>> result =
      sql_client_.Execute(call_options_, query);
  ThrowIfNotOK(result.status());

  flight_info_ = result.ValueOrDie();

  update_count_ = flight_info_->total_records();
  current_result_set_ = std::make_shared<FlightSqlResultSet>(
      sql_client_, call_options_, flight_info_, nullptr, diagnostics_, metadata_settings_);

  return true;
}

std::shared_ptr<ResultSet> FlightSqlStatement::GetResultSet() {
  return current_result_set_;
}

long FlightSqlStatement::GetUpdateCount() { return update_count_; }

std::shared_ptr<odbcabstraction::ResultSet> FlightSqlStatement::GetTables(
    const std::string *catalog_name, const std::string *schema_name,
    const std::string *table_name, const std::string *table_type,
    const ColumnNames &column_names) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  std::vector<std::string> table_types;

  if ((catalog_name && *catalog_name == "%") &&
      (schema_name && schema_name->empty()) &&
      (table_name && table_name->empty())) {
    current_result_set_ =
        GetTablesForSQLAllCatalogs(
                column_names, call_options_, sql_client_, diagnostics_, metadata_settings_);
  } else if ((catalog_name && catalog_name->empty()) &&
             (schema_name && *schema_name == "%") &&
             (table_name && table_name->empty())) {
    current_result_set_ = GetTablesForSQLAllDbSchemas(
        column_names, call_options_, sql_client_, schema_name, diagnostics_, metadata_settings_);
  } else if ((catalog_name && catalog_name->empty()) &&
             (schema_name && schema_name->empty()) &&
             (table_name && table_name->empty()) &&
             (table_type && *table_type == "%")) {
    current_result_set_ =
        GetTablesForSQLAllTableTypes(
                column_names, call_options_, sql_client_, diagnostics_, metadata_settings_);
  } else {
    if (table_type) {
      ParseTableTypes(*table_type, table_types);
    }

    // ODBC uses "TABLE" for base tables, but SQL standard / DuckDB uses
    // "BASE TABLE".  When callers (e.g. Power Query) request "TABLE",
    // also include "BASE TABLE" so the Flight SQL server returns results.
    if (std::find(table_types.begin(), table_types.end(), "TABLE") != table_types.end()) {
      table_types.emplace_back("BASE TABLE");
    }

    current_result_set_ = GetTablesForGenericUse(
        column_names, call_options_, sql_client_, catalog_name, schema_name,
        table_name, table_types, diagnostics_, metadata_settings_);
  }

  return current_result_set_;
}

std::shared_ptr<ResultSet> FlightSqlStatement::GetTables_V2(
    const std::string *catalog_name, const std::string *schema_name,
    const std::string *table_name, const std::string *table_type) {
  ColumnNames column_names{"TABLE_QUALIFIER", "TABLE_OWNER", "TABLE_NAME",
                           "TABLE_TYPE", "REMARKS"};

  return GetTables(catalog_name, schema_name, table_name, table_type,
                   column_names);
}

std::shared_ptr<ResultSet> FlightSqlStatement::GetTables_V3(
    const std::string *catalog_name, const std::string *schema_name,
    const std::string *table_name, const std::string *table_type) {
  ColumnNames column_names{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME",
                           "TABLE_TYPE", "REMARKS"};

  return GetTables(catalog_name, schema_name, table_name, table_type,
                   column_names);
}

std::shared_ptr<ResultSet> FlightSqlStatement::GetColumns_V2(
    const std::string *catalog_name, const std::string *schema_name,
    const std::string *table_name, const std::string *column_name) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  Result<std::shared_ptr<FlightInfo>> result = sql_client_.GetTables(
      call_options_, catalog_name, schema_name, table_name, true, nullptr);
  ThrowIfNotOK(result.status());

  auto flight_info = result.ValueOrDie();

  auto transformer = std::make_shared<GetColumns_Transformer>(
      metadata_settings_, odbcabstraction::V_2, column_name);

  current_result_set_ = std::make_shared<FlightSqlResultSet>(
      sql_client_, call_options_, flight_info, transformer, diagnostics_, metadata_settings_);

  return current_result_set_;
}

std::shared_ptr<ResultSet> FlightSqlStatement::GetColumns_V3(
    const std::string *catalog_name, const std::string *schema_name,
    const std::string *table_name, const std::string *column_name) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  Result<std::shared_ptr<FlightInfo>> result = sql_client_.GetTables(
      call_options_, catalog_name, schema_name, table_name, true, nullptr);
  ThrowIfNotOK(result.status());

  auto flight_info = result.ValueOrDie();

  auto transformer = std::make_shared<GetColumns_Transformer>(
      metadata_settings_, odbcabstraction::V_3, column_name);

  current_result_set_ = std::make_shared<FlightSqlResultSet>(
      sql_client_, call_options_, flight_info, transformer, diagnostics_, metadata_settings_);

  return current_result_set_;
}

std::shared_ptr<ResultSet> FlightSqlStatement::GetTypeInfo_V2(int16_t data_type) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  Result<std::shared_ptr<FlightInfo>> result = sql_client_.GetXdbcTypeInfo(
          call_options_);
  ThrowIfNotOK(result.status());

  auto flight_info = result.ValueOrDie();

  auto transformer = std::make_shared<GetTypeInfo_Transformer>(
          metadata_settings_, odbcabstraction::V_2, data_type);

  current_result_set_ = std::make_shared<FlightSqlResultSet>(
      sql_client_, call_options_, flight_info, transformer, diagnostics_, metadata_settings_);

  return current_result_set_;
}

std::shared_ptr<ResultSet> FlightSqlStatement::GetTypeInfo_V3(int16_t data_type) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  Result<std::shared_ptr<FlightInfo>> result = sql_client_.GetXdbcTypeInfo(
          call_options_);
  ThrowIfNotOK(result.status());

  auto flight_info = result.ValueOrDie();

  auto transformer = std::make_shared<GetTypeInfo_Transformer>(
          metadata_settings_, odbcabstraction::V_3, data_type);

  current_result_set_ = std::make_shared<FlightSqlResultSet>(
      sql_client_, call_options_, flight_info, transformer, diagnostics_, metadata_settings_);

  return current_result_set_;
}

std::shared_ptr<ResultSet> FlightSqlStatement::GetPrimaryKeys(
    const std::string *catalog_name, const std::string *schema_name,
    const std::string *table_name) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  auto schema = arrow::schema({
    arrow::field("TABLE_CAT", arrow::utf8(), true),      // nullable
    arrow::field("TABLE_SCHEM", arrow::utf8(), true),    // nullable
    arrow::field("TABLE_NAME", arrow::utf8(), false),    // not nullable
    arrow::field("COLUMN_NAME", arrow::utf8(), false),   // not nullable
    arrow::field("KEY_SEQ", arrow::int16(), false),      // not nullable
    arrow::field("PK_NAME", arrow::utf8(), true)         // nullable
  });

  auto flight_info_result = arrow::flight::FlightInfo::Make(
    *schema, arrow::flight::FlightDescriptor::Command(""),
    std::vector<arrow::flight::FlightEndpoint>(), 0, 0);
  ThrowIfNotOK(flight_info_result.status());
  auto flight_info = std::make_shared<arrow::flight::FlightInfo>(std::move(flight_info_result.ValueOrDie()));

  current_result_set_ = std::make_shared<FlightSqlResultSet>(
    sql_client_, call_options_, flight_info, nullptr, diagnostics_, metadata_settings_);

  return current_result_set_;
}

std::shared_ptr<ResultSet> FlightSqlStatement::GetForeignKeys(
    const std::string *pk_catalog_name, const std::string *pk_schema_name,
    const std::string *pk_table_name, const std::string *fk_catalog_name,
    const std::string *fk_schema_name, const std::string *fk_table_name) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  auto schema = arrow::schema({
    arrow::field("PKTABLE_CAT", arrow::utf8(), true),     // nullable
    arrow::field("PKTABLE_SCHEM", arrow::utf8(), true),   // nullable
    arrow::field("PKTABLE_NAME", arrow::utf8(), false),   // not nullable
    arrow::field("PKCOLUMN_NAME", arrow::utf8(), false),  // not nullable
    arrow::field("FKTABLE_CAT", arrow::utf8(), true),     // nullable
    arrow::field("FKTABLE_SCHEM", arrow::utf8(), true),   // nullable
    arrow::field("FKTABLE_NAME", arrow::utf8(), false),   // not nullable
    arrow::field("FKCOLUMN_NAME", arrow::utf8(), false),  // not nullable
    arrow::field("KEY_SEQ", arrow::int16(), false),       // not nullable
    arrow::field("UPDATE_RULE", arrow::int16(), true),    // nullable
    arrow::field("DELETE_RULE", arrow::int16(), true),    // nullable
    arrow::field("FK_NAME", arrow::utf8(), true),         // nullable
    arrow::field("PK_NAME", arrow::utf8(), true),         // nullable
    arrow::field("DEFERRABILITY", arrow::int16(), true)   // nullable
  });

  auto flight_info_result = arrow::flight::FlightInfo::Make(
    *schema, arrow::flight::FlightDescriptor::Command(""),
    std::vector<arrow::flight::FlightEndpoint>(), 0, 0);
  ThrowIfNotOK(flight_info_result.status());
  auto flight_info = std::make_shared<arrow::flight::FlightInfo>(std::move(flight_info_result.ValueOrDie()));

  current_result_set_ = std::make_shared<FlightSqlResultSet>(
    sql_client_, call_options_, flight_info, nullptr, diagnostics_, metadata_settings_);

  return current_result_set_;
}

std::shared_ptr<ResultSetMetadata> FlightSqlStatement::GetParameterSchema() {
  if (!prepared_statement_) {
    return nullptr;
  }

  auto param_schema = prepared_statement_->parameter_schema();
  if (!param_schema || param_schema->num_fields() == 0) {
    return nullptr;
  }

  return std::make_shared<FlightSqlResultSetMetadata>(param_schema, metadata_settings_);
}

void FlightSqlStatement::SetParameters(
    const std::vector<odbcabstraction::ParameterBinding>& params) {
  if (!prepared_statement_) {
    throw DriverException("Statement is not prepared", "HY010");
  }

  auto param_schema = prepared_statement_->parameter_schema();
  if (!param_schema) {
    throw DriverException("Prepared statement has no parameter schema", "07002");
  }

  if (static_cast<int>(params.size()) != param_schema->num_fields()) {
    throw DriverException(
        "Parameter count mismatch: expected " +
            std::to_string(param_schema->num_fields()) + " but got " +
            std::to_string(params.size()),
        "07002");
  }

  std::vector<std::shared_ptr<arrow::Array>> arrays;
  arrays.reserve(params.size());

  for (int i = 0; i < static_cast<int>(params.size()); ++i) {
    const auto& binding = params[i];
    const auto& field = param_schema->field(i);
    const auto& arrow_type = field->type();

    // Check for NULL
    bool is_null = false;
    if (binding.indicator_ptr && *binding.indicator_ptr == odbcabstraction::NULL_DATA) {
      is_null = true;
    }

    std::shared_ptr<arrow::Array> array;

    switch (arrow_type->id()) {
      case arrow::Type::STRING: {
        arrow::StringBuilder builder;
        if (is_null) {
          ThrowIfNotOK(builder.AppendNull());
        } else if (binding.c_type == SQL_C_CHAR) {
          ssize_t len = binding.indicator_ptr ? *binding.indicator_ptr : -1;
          const char* str = static_cast<const char*>(binding.data_ptr);
          if (len == SQL_NTS || len < 0) {
            len = static_cast<ssize_t>(strlen(str));
          }
          ThrowIfNotOK(builder.Append(str, static_cast<int32_t>(len)));
        } else if (binding.c_type == SQL_C_WCHAR) {
          ssize_t char_len = binding.indicator_ptr ? *binding.indicator_ptr : -1;
          if (char_len == SQL_NTS || char_len < 0) {
            char_len = static_cast<ssize_t>(
                odbcabstraction::wcsstrlen(binding.data_ptr) *
                odbcabstraction::GetSqlWCharSize());
          }
          size_t code_units = static_cast<size_t>(char_len) / odbcabstraction::GetSqlWCharSize();
          std::vector<uint8_t> utf8_buf;
          odbcabstraction::WcsToUtf8(binding.data_ptr, code_units, &utf8_buf);
          ThrowIfNotOK(builder.Append(
              reinterpret_cast<const char*>(utf8_buf.data()),
              static_cast<int32_t>(utf8_buf.size())));
        } else {
          // Numeric C types: convert to string via snprintf
          char buf[64];
          int written = 0;
          switch (binding.c_type) {
            case SQL_C_SLONG:
              written = snprintf(buf, sizeof(buf), "%d",
                                 *static_cast<const int32_t*>(binding.data_ptr));
              break;
            case SQL_C_ULONG:
              written = snprintf(buf, sizeof(buf), "%u",
                                 *static_cast<const uint32_t*>(binding.data_ptr));
              break;
            case SQL_C_SBIGINT:
              written = snprintf(buf, sizeof(buf), "%lld",
                                 static_cast<long long>(*static_cast<const int64_t*>(binding.data_ptr)));
              break;
            case SQL_C_UBIGINT:
              written = snprintf(buf, sizeof(buf), "%llu",
                                 static_cast<unsigned long long>(*static_cast<const uint64_t*>(binding.data_ptr)));
              break;
            case SQL_C_SSHORT:
              written = snprintf(buf, sizeof(buf), "%d",
                                 static_cast<int>(*static_cast<const int16_t*>(binding.data_ptr)));
              break;
            case SQL_C_DOUBLE:
              written = snprintf(buf, sizeof(buf), "%.17g",
                                 *static_cast<const double*>(binding.data_ptr));
              break;
            case SQL_C_FLOAT:
              written = snprintf(buf, sizeof(buf), "%.9g",
                                 static_cast<double>(*static_cast<const float*>(binding.data_ptr)));
              break;
            default:
              throw DriverException(
                  "Cannot convert C type " + std::to_string(binding.c_type) +
                      " to Arrow utf8",
                  "07006");
          }
          ThrowIfNotOK(builder.Append(buf, written));
        }
        ThrowIfNotOK(builder.Finish(&array));
        break;
      }

      case arrow::Type::INT32: {
        arrow::Int32Builder builder;
        if (is_null) {
          ThrowIfNotOK(builder.AppendNull());
        } else if (binding.c_type == SQL_C_SLONG) {
          ThrowIfNotOK(builder.Append(*static_cast<const int32_t*>(binding.data_ptr)));
        } else if (binding.c_type == SQL_C_CHAR) {
          ThrowIfNotOK(builder.Append(
              static_cast<int32_t>(strtol(static_cast<const char*>(binding.data_ptr), nullptr, 10))));
        } else if (binding.c_type == SQL_C_SBIGINT) {
          ThrowIfNotOK(builder.Append(
              static_cast<int32_t>(*static_cast<const int64_t*>(binding.data_ptr))));
        } else if (binding.c_type == SQL_C_SSHORT) {
          ThrowIfNotOK(builder.Append(
              static_cast<int32_t>(*static_cast<const int16_t*>(binding.data_ptr))));
        } else {
          throw DriverException(
              "Cannot convert C type " + std::to_string(binding.c_type) + " to Arrow int32", "07006");
        }
        ThrowIfNotOK(builder.Finish(&array));
        break;
      }

      case arrow::Type::INT64: {
        arrow::Int64Builder builder;
        if (is_null) {
          ThrowIfNotOK(builder.AppendNull());
        } else if (binding.c_type == SQL_C_SBIGINT) {
          ThrowIfNotOK(builder.Append(*static_cast<const int64_t*>(binding.data_ptr)));
        } else if (binding.c_type == SQL_C_SLONG) {
          ThrowIfNotOK(builder.Append(
              static_cast<int64_t>(*static_cast<const int32_t*>(binding.data_ptr))));
        } else if (binding.c_type == SQL_C_CHAR) {
          ThrowIfNotOK(builder.Append(
              static_cast<int64_t>(strtoll(static_cast<const char*>(binding.data_ptr), nullptr, 10))));
        } else {
          throw DriverException(
              "Cannot convert C type " + std::to_string(binding.c_type) + " to Arrow int64", "07006");
        }
        ThrowIfNotOK(builder.Finish(&array));
        break;
      }

      case arrow::Type::INT16: {
        arrow::Int16Builder builder;
        if (is_null) {
          ThrowIfNotOK(builder.AppendNull());
        } else if (binding.c_type == SQL_C_SSHORT) {
          ThrowIfNotOK(builder.Append(*static_cast<const int16_t*>(binding.data_ptr)));
        } else if (binding.c_type == SQL_C_SLONG) {
          ThrowIfNotOK(builder.Append(
              static_cast<int16_t>(*static_cast<const int32_t*>(binding.data_ptr))));
        } else if (binding.c_type == SQL_C_CHAR) {
          ThrowIfNotOK(builder.Append(
              static_cast<int16_t>(strtol(static_cast<const char*>(binding.data_ptr), nullptr, 10))));
        } else {
          throw DriverException(
              "Cannot convert C type " + std::to_string(binding.c_type) + " to Arrow int16", "07006");
        }
        ThrowIfNotOK(builder.Finish(&array));
        break;
      }

      case arrow::Type::DOUBLE: {
        arrow::DoubleBuilder builder;
        if (is_null) {
          ThrowIfNotOK(builder.AppendNull());
        } else if (binding.c_type == SQL_C_DOUBLE) {
          ThrowIfNotOK(builder.Append(*static_cast<const double*>(binding.data_ptr)));
        } else if (binding.c_type == SQL_C_FLOAT) {
          ThrowIfNotOK(builder.Append(
              static_cast<double>(*static_cast<const float*>(binding.data_ptr))));
        } else if (binding.c_type == SQL_C_CHAR) {
          ThrowIfNotOK(builder.Append(strtod(static_cast<const char*>(binding.data_ptr), nullptr)));
        } else {
          throw DriverException(
              "Cannot convert C type " + std::to_string(binding.c_type) + " to Arrow float64", "07006");
        }
        ThrowIfNotOK(builder.Finish(&array));
        break;
      }

      case arrow::Type::FLOAT: {
        arrow::FloatBuilder builder;
        if (is_null) {
          ThrowIfNotOK(builder.AppendNull());
        } else if (binding.c_type == SQL_C_FLOAT) {
          ThrowIfNotOK(builder.Append(*static_cast<const float*>(binding.data_ptr)));
        } else if (binding.c_type == SQL_C_DOUBLE) {
          ThrowIfNotOK(builder.Append(
              static_cast<float>(*static_cast<const double*>(binding.data_ptr))));
        } else if (binding.c_type == SQL_C_CHAR) {
          ThrowIfNotOK(builder.Append(
              static_cast<float>(strtod(static_cast<const char*>(binding.data_ptr), nullptr))));
        } else {
          throw DriverException(
              "Cannot convert C type " + std::to_string(binding.c_type) + " to Arrow float32", "07006");
        }
        ThrowIfNotOK(builder.Finish(&array));
        break;
      }

      case arrow::Type::type(1) /* BOOL */: {
        arrow::BooleanBuilder builder;
        if (is_null) {
          ThrowIfNotOK(builder.AppendNull());
        } else if (binding.c_type == SQL_C_BIT) {
          ThrowIfNotOK(builder.Append(*static_cast<const uint8_t*>(binding.data_ptr) != 0));
        } else if (binding.c_type == SQL_C_CHAR) {
          const char* str = static_cast<const char*>(binding.data_ptr);
          ThrowIfNotOK(builder.Append(str[0] == '1' || str[0] == 't' || str[0] == 'T'));
        } else {
          throw DriverException(
              "Cannot convert C type " + std::to_string(binding.c_type) + " to Arrow boolean", "07006");
        }
        ThrowIfNotOK(builder.Finish(&array));
        break;
      }

      case arrow::Type::DATE32: {
        arrow::Date32Builder builder;
        if (is_null) {
          ThrowIfNotOK(builder.AppendNull());
        } else if (binding.c_type == SQL_C_TYPE_DATE) {
          const auto* ds = static_cast<const odbcabstraction::DATE_STRUCT*>(binding.data_ptr);
          struct tm t = {};
          t.tm_year = ds->year - 1900;
          t.tm_mon = ds->month - 1;
          t.tm_mday = ds->day;
          time_t epoch = portable_timegm(&t);
          int32_t days = static_cast<int32_t>(epoch / 86400);
          ThrowIfNotOK(builder.Append(days));
        } else if (binding.c_type == SQL_C_CHAR) {
          // Parse "YYYY-MM-DD"
          const char* str = static_cast<const char*>(binding.data_ptr);
          int y, m, d;
          if (sscanf(str, "%d-%d-%d", &y, &m, &d) == 3) {
            struct tm t = {};
            t.tm_year = y - 1900;
            t.tm_mon = m - 1;
            t.tm_mday = d;
            time_t epoch = portable_timegm(&t);
            ThrowIfNotOK(builder.Append(static_cast<int32_t>(epoch / 86400)));
          } else {
            throw DriverException("Cannot parse date string: " + std::string(str), "22007");
          }
        } else {
          throw DriverException(
              "Cannot convert C type " + std::to_string(binding.c_type) + " to Arrow date32", "07006");
        }
        ThrowIfNotOK(builder.Finish(&array));
        break;
      }

      case arrow::Type::TIMESTAMP: {
        arrow::TimestampBuilder builder(arrow::timestamp(arrow::TimeUnit::MICRO),
                                        arrow::default_memory_pool());
        if (is_null) {
          ThrowIfNotOK(builder.AppendNull());
        } else if (binding.c_type == SQL_C_TYPE_TIMESTAMP) {
          const auto* ts = static_cast<const odbcabstraction::TIMESTAMP_STRUCT*>(binding.data_ptr);
          struct tm t = {};
          t.tm_year = ts->year - 1900;
          t.tm_mon = ts->month - 1;
          t.tm_mday = ts->day;
          t.tm_hour = ts->hour;
          t.tm_min = ts->minute;
          t.tm_sec = ts->second;
          time_t epoch = portable_timegm(&t);
          int64_t micros = static_cast<int64_t>(epoch) * 1000000LL +
                           static_cast<int64_t>(ts->fraction) / 1000LL;
          ThrowIfNotOK(builder.Append(micros));
        } else if (binding.c_type == SQL_C_CHAR) {
          // Parse "YYYY-MM-DD HH:MM:SS[.fff]"
          const char* str = static_cast<const char*>(binding.data_ptr);
          int y, mon, d, h, mi, s;
          unsigned int frac = 0;
          int parsed = sscanf(str, "%d-%d-%d %d:%d:%d.%u", &y, &mon, &d, &h, &mi, &s, &frac);
          if (parsed >= 6) {
            struct tm t = {};
            t.tm_year = y - 1900;
            t.tm_mon = mon - 1;
            t.tm_mday = d;
            t.tm_hour = h;
            t.tm_min = mi;
            t.tm_sec = s;
            time_t epoch = portable_timegm(&t);
            int64_t micros = static_cast<int64_t>(epoch) * 1000000LL +
                             static_cast<int64_t>(frac) / 1000LL;
            ThrowIfNotOK(builder.Append(micros));
          } else {
            throw DriverException("Cannot parse timestamp string: " + std::string(str), "22007");
          }
        } else {
          throw DriverException(
              "Cannot convert C type " + std::to_string(binding.c_type) + " to Arrow timestamp", "07006");
        }
        ThrowIfNotOK(builder.Finish(&array));
        break;
      }

      default:
        throw DriverException(
            "Unsupported Arrow parameter type: " + arrow_type->ToString(), "HYC00");
    }

    arrays.push_back(std::move(array));
  }

  auto batch = arrow::RecordBatch::Make(param_schema, 1, std::move(arrays));
  ThrowIfNotOK(prepared_statement_->SetParameters(batch));
}

odbcabstraction::Diagnostics &FlightSqlStatement::GetDiagnostics() {
  return diagnostics_;
}

void FlightSqlStatement::Cancel() {
  if (!current_result_set_) return;

  // Send CancelFlightInfo to the server to stop query execution
  if (flight_info_) {
    auto cancel_info = std::make_unique<FlightInfo>(*flight_info_);
    arrow::flight::CancelFlightInfoRequest request(std::move(cancel_info));
    auto result = sql_client_.CancelFlightInfo(call_options_, request);
    (void)result; // Best-effort; ignore errors on cancel
  }

  current_result_set_->Cancel();
}

} // namespace flight_sql
} // namespace driver
