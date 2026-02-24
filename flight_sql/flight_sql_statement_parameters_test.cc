/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

/*
 * Unit tests for parameter binding type conversion logic.
 *
 * Tests the ODBC C type → Arrow array conversion that SetParameters() performs,
 * exercising each supported type path: string, int16/32/64, float32/64,
 * boolean, date32, timestamp. Also tests NULL handling and string-to-numeric
 * parsing (Power BI sends many values as SQL_C_CHAR strings).
 */

#include "gtest/gtest.h"

#include <arrow/builder.h>
#include <arrow/record_batch.h>
#include <arrow/type.h>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <odbcabstraction/platform.h>
#include <sql.h>
#include <sqlext.h>

#include "odbcabstraction/types.h"
#include "odbcabstraction/encoding.h"

// timegm is POSIX; Windows provides _mkgmtime instead
#ifdef _WIN32
#define portable_timegm _mkgmtime
#else
#define portable_timegm timegm
#endif

namespace driver {
namespace flight_sql {

// Helper: convert SQL_C_WCHAR binding to UTF-8 string for numeric parsing
static std::string wcharToUtf8(const odbcabstraction::ParameterBinding& binding) {
  ssize_t byte_len = binding.indicator_ptr ? *binding.indicator_ptr : -1;
  size_t wchar_size = odbcabstraction::GetSqlWCharSize();
  size_t code_units;
  if (byte_len == SQL_NTS || byte_len < 0) {
    code_units = odbcabstraction::wcsstrlen(binding.data_ptr);
  } else {
    code_units = static_cast<size_t>(byte_len) / wchar_size;
  }
  std::vector<uint8_t> utf8_buf;
  odbcabstraction::WcsToUtf8(binding.data_ptr, code_units, &utf8_buf);
  return std::string(reinterpret_cast<const char*>(utf8_buf.data()), utf8_buf.size());
}

// Helper: build an Arrow array from an ODBC parameter binding,
// mirroring the logic in FlightSqlStatement::SetParameters().
// This allows testing type conversion without a live Flight SQL connection.
static std::shared_ptr<arrow::Array> BuildArrayFromBinding(
    const std::shared_ptr<arrow::DataType>& arrow_type,
    const odbcabstraction::ParameterBinding& binding) {

  bool is_null = binding.indicator_ptr &&
                 *binding.indicator_ptr == odbcabstraction::NULL_DATA;

  std::shared_ptr<arrow::Array> array;

  switch (arrow_type->id()) {
    case arrow::Type::STRING: {
      arrow::StringBuilder builder;
      if (is_null) {
        EXPECT_TRUE(builder.AppendNull().ok());
      } else if (binding.c_type == SQL_C_CHAR) {
        ssize_t len = binding.indicator_ptr ? *binding.indicator_ptr : -1;
        const char* str = static_cast<const char*>(binding.data_ptr);
        if (len == SQL_NTS || len < 0) {
          len = static_cast<ssize_t>(strlen(str));
        }
        EXPECT_TRUE(builder.Append(str, static_cast<int32_t>(len)).ok());
      } else if (binding.c_type == SQL_C_WCHAR) {
        auto s = wcharToUtf8(binding);
        EXPECT_TRUE(builder.Append(s.data(), static_cast<int32_t>(s.size())).ok());
      } else if (binding.c_type == SQL_C_SLONG) {
        char buf[64];
        int written = snprintf(buf, sizeof(buf), "%d",
                               *static_cast<const int32_t*>(binding.data_ptr));
        EXPECT_TRUE(builder.Append(buf, written).ok());
      }
      EXPECT_TRUE(builder.Finish(&array).ok());
      break;
    }

    case arrow::Type::INT32: {
      arrow::Int32Builder builder;
      if (is_null) {
        EXPECT_TRUE(builder.AppendNull().ok());
      } else if (binding.c_type == SQL_C_SLONG) {
        EXPECT_TRUE(builder.Append(*static_cast<const int32_t*>(binding.data_ptr)).ok());
      } else if (binding.c_type == SQL_C_CHAR) {
        EXPECT_TRUE(builder.Append(
            static_cast<int32_t>(strtol(static_cast<const char*>(binding.data_ptr), nullptr, 10))).ok());
      } else if (binding.c_type == SQL_C_WCHAR) {
        auto s = wcharToUtf8(binding);
        EXPECT_TRUE(builder.Append(static_cast<int32_t>(strtol(s.c_str(), nullptr, 10))).ok());
      }
      EXPECT_TRUE(builder.Finish(&array).ok());
      break;
    }

    case arrow::Type::INT64: {
      arrow::Int64Builder builder;
      if (is_null) {
        EXPECT_TRUE(builder.AppendNull().ok());
      } else if (binding.c_type == SQL_C_SBIGINT) {
        EXPECT_TRUE(builder.Append(*static_cast<const int64_t*>(binding.data_ptr)).ok());
      } else if (binding.c_type == SQL_C_SLONG) {
        EXPECT_TRUE(builder.Append(
            static_cast<int64_t>(*static_cast<const int32_t*>(binding.data_ptr))).ok());
      } else if (binding.c_type == SQL_C_CHAR) {
        EXPECT_TRUE(builder.Append(
            static_cast<int64_t>(strtoll(static_cast<const char*>(binding.data_ptr), nullptr, 10))).ok());
      } else if (binding.c_type == SQL_C_WCHAR) {
        auto s = wcharToUtf8(binding);
        EXPECT_TRUE(builder.Append(static_cast<int64_t>(strtoll(s.c_str(), nullptr, 10))).ok());
      }
      EXPECT_TRUE(builder.Finish(&array).ok());
      break;
    }

    case arrow::Type::INT16: {
      arrow::Int16Builder builder;
      if (is_null) {
        EXPECT_TRUE(builder.AppendNull().ok());
      } else if (binding.c_type == SQL_C_SSHORT) {
        EXPECT_TRUE(builder.Append(*static_cast<const int16_t*>(binding.data_ptr)).ok());
      } else if (binding.c_type == SQL_C_CHAR) {
        EXPECT_TRUE(builder.Append(
            static_cast<int16_t>(strtol(static_cast<const char*>(binding.data_ptr), nullptr, 10))).ok());
      } else if (binding.c_type == SQL_C_WCHAR) {
        auto s = wcharToUtf8(binding);
        EXPECT_TRUE(builder.Append(static_cast<int16_t>(strtol(s.c_str(), nullptr, 10))).ok());
      }
      EXPECT_TRUE(builder.Finish(&array).ok());
      break;
    }

    case arrow::Type::DOUBLE: {
      arrow::DoubleBuilder builder;
      if (is_null) {
        EXPECT_TRUE(builder.AppendNull().ok());
      } else if (binding.c_type == SQL_C_DOUBLE) {
        EXPECT_TRUE(builder.Append(*static_cast<const double*>(binding.data_ptr)).ok());
      } else if (binding.c_type == SQL_C_CHAR) {
        EXPECT_TRUE(builder.Append(strtod(static_cast<const char*>(binding.data_ptr), nullptr)).ok());
      } else if (binding.c_type == SQL_C_WCHAR) {
        auto s = wcharToUtf8(binding);
        EXPECT_TRUE(builder.Append(strtod(s.c_str(), nullptr)).ok());
      }
      EXPECT_TRUE(builder.Finish(&array).ok());
      break;
    }

    case arrow::Type::FLOAT: {
      arrow::FloatBuilder builder;
      if (is_null) {
        EXPECT_TRUE(builder.AppendNull().ok());
      } else if (binding.c_type == SQL_C_FLOAT) {
        EXPECT_TRUE(builder.Append(*static_cast<const float*>(binding.data_ptr)).ok());
      } else if (binding.c_type == SQL_C_CHAR) {
        EXPECT_TRUE(builder.Append(
            static_cast<float>(strtod(static_cast<const char*>(binding.data_ptr), nullptr))).ok());
      } else if (binding.c_type == SQL_C_WCHAR) {
        auto s = wcharToUtf8(binding);
        EXPECT_TRUE(builder.Append(static_cast<float>(strtod(s.c_str(), nullptr))).ok());
      }
      EXPECT_TRUE(builder.Finish(&array).ok());
      break;
    }

    case arrow::Type::type(1) /* BOOL */: {
      arrow::BooleanBuilder builder;
      if (is_null) {
        EXPECT_TRUE(builder.AppendNull().ok());
      } else if (binding.c_type == SQL_C_BIT) {
        EXPECT_TRUE(builder.Append(*static_cast<const uint8_t*>(binding.data_ptr) != 0).ok());
      } else if (binding.c_type == SQL_C_CHAR) {
        const char* str = static_cast<const char*>(binding.data_ptr);
        EXPECT_TRUE(builder.Append(str[0] == '1' || str[0] == 't' || str[0] == 'T').ok());
      } else if (binding.c_type == SQL_C_WCHAR) {
        auto s = wcharToUtf8(binding);
        EXPECT_TRUE(builder.Append(!s.empty() && (s[0] == '1' || s[0] == 't' || s[0] == 'T')).ok());
      }
      EXPECT_TRUE(builder.Finish(&array).ok());
      break;
    }

    case arrow::Type::DATE32: {
      arrow::Date32Builder builder;
      if (is_null) {
        EXPECT_TRUE(builder.AppendNull().ok());
      } else if (binding.c_type == SQL_C_TYPE_DATE) {
        const auto* ds = static_cast<const odbcabstraction::DATE_STRUCT*>(binding.data_ptr);
        struct tm t = {};
        t.tm_year = ds->year - 1900;
        t.tm_mon = ds->month - 1;
        t.tm_mday = ds->day;
        time_t epoch = portable_timegm(&t);
        int32_t days = static_cast<int32_t>(epoch / 86400);
        EXPECT_TRUE(builder.Append(days).ok());
      }
      EXPECT_TRUE(builder.Finish(&array).ok());
      break;
    }

    case arrow::Type::TIMESTAMP: {
      arrow::TimestampBuilder builder(arrow::timestamp(arrow::TimeUnit::MICRO),
                                      arrow::default_memory_pool());
      if (is_null) {
        EXPECT_TRUE(builder.AppendNull().ok());
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
        EXPECT_TRUE(builder.Append(micros).ok());
      }
      EXPECT_TRUE(builder.Finish(&array).ok());
      break;
    }

    default:
      ADD_FAILURE() << "Unsupported Arrow type: " << arrow_type->ToString();
      break;
  }

  return array;
}

// ============================================================================
// Integer parameter tests
// ============================================================================

TEST(ParameterBinding, Int32_from_SLONG) {
  int32_t value = 42;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = &value;
  binding.indicator_ptr = nullptr;
  binding.c_type = SQL_C_SLONG;

  auto array = BuildArrayFromBinding(arrow::int32(), binding);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(array->length(), 1);
  ASSERT_FALSE(array->IsNull(0));
  auto typed = std::static_pointer_cast<arrow::Int32Array>(array);
  EXPECT_EQ(typed->Value(0), 42);
}

TEST(ParameterBinding, Int32_from_CHAR_string) {
  const char* str = "12345";
  ssize_t indicator = SQL_NTS;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = const_cast<char*>(str);
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_CHAR;

  auto array = BuildArrayFromBinding(arrow::int32(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::Int32Array>(array);
  EXPECT_EQ(typed->Value(0), 12345);
}

TEST(ParameterBinding, Int64_from_SBIGINT) {
  int64_t value = 9876543210LL;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = &value;
  binding.indicator_ptr = nullptr;
  binding.c_type = SQL_C_SBIGINT;

  auto array = BuildArrayFromBinding(arrow::int64(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::Int64Array>(array);
  EXPECT_EQ(typed->Value(0), 9876543210LL);
}

TEST(ParameterBinding, Int64_from_SLONG_promotion) {
  int32_t value = 42;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = &value;
  binding.indicator_ptr = nullptr;
  binding.c_type = SQL_C_SLONG;

  auto array = BuildArrayFromBinding(arrow::int64(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::Int64Array>(array);
  EXPECT_EQ(typed->Value(0), 42);
}

TEST(ParameterBinding, Int16_from_SSHORT) {
  int16_t value = 300;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = &value;
  binding.indicator_ptr = nullptr;
  binding.c_type = SQL_C_SSHORT;

  auto array = BuildArrayFromBinding(arrow::int16(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::Int16Array>(array);
  EXPECT_EQ(typed->Value(0), 300);
}

// ============================================================================
// Float parameter tests
// ============================================================================

TEST(ParameterBinding, Double_from_DOUBLE) {
  double value = 3.14159;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = &value;
  binding.indicator_ptr = nullptr;
  binding.c_type = SQL_C_DOUBLE;

  auto array = BuildArrayFromBinding(arrow::float64(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::DoubleArray>(array);
  EXPECT_DOUBLE_EQ(typed->Value(0), 3.14159);
}

TEST(ParameterBinding, Double_from_CHAR_string) {
  const char* str = "2.71828";
  ssize_t indicator = SQL_NTS;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = const_cast<char*>(str);
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_CHAR;

  auto array = BuildArrayFromBinding(arrow::float64(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::DoubleArray>(array);
  EXPECT_DOUBLE_EQ(typed->Value(0), 2.71828);
}

TEST(ParameterBinding, Float_from_FLOAT) {
  float value = 1.5f;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = &value;
  binding.indicator_ptr = nullptr;
  binding.c_type = SQL_C_FLOAT;

  auto array = BuildArrayFromBinding(arrow::float32(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::FloatArray>(array);
  EXPECT_FLOAT_EQ(typed->Value(0), 1.5f);
}

// ============================================================================
// String parameter tests
// ============================================================================

TEST(ParameterBinding, String_from_CHAR) {
  const char* str = "hello world";
  ssize_t indicator = SQL_NTS;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = const_cast<char*>(str);
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_CHAR;

  auto array = BuildArrayFromBinding(arrow::utf8(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::StringArray>(array);
  EXPECT_EQ(typed->GetString(0), "hello world");
}

TEST(ParameterBinding, String_from_CHAR_with_explicit_length) {
  const char* str = "hello world extra data";
  ssize_t indicator = 5; // Only "hello"
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = const_cast<char*>(str);
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_CHAR;

  auto array = BuildArrayFromBinding(arrow::utf8(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::StringArray>(array);
  EXPECT_EQ(typed->GetString(0), "hello");
}

TEST(ParameterBinding, String_from_SLONG_numeric) {
  int32_t value = 42;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = &value;
  binding.indicator_ptr = nullptr;
  binding.c_type = SQL_C_SLONG;

  auto array = BuildArrayFromBinding(arrow::utf8(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::StringArray>(array);
  EXPECT_EQ(typed->GetString(0), "42");
}

// ============================================================================
// Boolean parameter tests
// ============================================================================

TEST(ParameterBinding, Boolean_from_BIT_true) {
  uint8_t value = 1;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = &value;
  binding.indicator_ptr = nullptr;
  binding.c_type = SQL_C_BIT;

  auto array = BuildArrayFromBinding(arrow::boolean(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::BooleanArray>(array);
  EXPECT_TRUE(typed->Value(0));
}

TEST(ParameterBinding, Boolean_from_BIT_false) {
  uint8_t value = 0;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = &value;
  binding.indicator_ptr = nullptr;
  binding.c_type = SQL_C_BIT;

  auto array = BuildArrayFromBinding(arrow::boolean(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::BooleanArray>(array);
  EXPECT_FALSE(typed->Value(0));
}

TEST(ParameterBinding, Boolean_from_CHAR_string) {
  const char* str = "1";
  ssize_t indicator = SQL_NTS;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = const_cast<char*>(str);
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_CHAR;

  auto array = BuildArrayFromBinding(arrow::boolean(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::BooleanArray>(array);
  EXPECT_TRUE(typed->Value(0));
}

// ============================================================================
// Date parameter tests
// ============================================================================

TEST(ParameterBinding, Date32_from_DATE_STRUCT) {
  odbcabstraction::DATE_STRUCT ds = {};
  ds.year = 2024;
  ds.month = 6;
  ds.day = 15;

  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = &ds;
  binding.indicator_ptr = nullptr;
  binding.c_type = SQL_C_TYPE_DATE;

  auto array = BuildArrayFromBinding(arrow::date32(), binding);
  ASSERT_NE(array, nullptr);
  ASSERT_FALSE(array->IsNull(0));
  auto typed = std::static_pointer_cast<arrow::Date32Array>(array);
  int32_t days = typed->Value(0);

  // 2024-06-15 should be a positive day count from epoch
  EXPECT_GT(days, 0);

  // Verify by computing expected days
  struct tm t = {};
  t.tm_year = 2024 - 1900;
  t.tm_mon = 6 - 1;
  t.tm_mday = 15;
  time_t epoch = portable_timegm(&t);
  int32_t expected_days = static_cast<int32_t>(epoch / 86400);
  EXPECT_EQ(days, expected_days);
}

// ============================================================================
// Timestamp parameter tests
// ============================================================================

TEST(ParameterBinding, Timestamp_from_TIMESTAMP_STRUCT) {
  odbcabstraction::TIMESTAMP_STRUCT ts = {};
  ts.year = 2024;
  ts.month = 6;
  ts.day = 15;
  ts.hour = 10;
  ts.minute = 30;
  ts.second = 45;
  ts.fraction = 123000000; // 123ms in nanoseconds

  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = &ts;
  binding.indicator_ptr = nullptr;
  binding.c_type = SQL_C_TYPE_TIMESTAMP;

  auto array = BuildArrayFromBinding(arrow::timestamp(arrow::TimeUnit::MICRO), binding);
  ASSERT_NE(array, nullptr);
  ASSERT_FALSE(array->IsNull(0));
  auto typed = std::static_pointer_cast<arrow::TimestampArray>(array);
  int64_t micros = typed->Value(0);

  // Should be a large positive microsecond count
  EXPECT_GT(micros, 0);

  // Verify the fractional part (123ms = 123000µs)
  struct tm t = {};
  t.tm_year = 2024 - 1900;
  t.tm_mon = 6 - 1;
  t.tm_mday = 15;
  t.tm_hour = 10;
  t.tm_min = 30;
  t.tm_sec = 45;
  time_t epoch = portable_timegm(&t);
  int64_t expected_micros = static_cast<int64_t>(epoch) * 1000000LL + 123000LL;
  EXPECT_EQ(micros, expected_micros);
}

// ============================================================================
// NULL parameter tests
// ============================================================================

TEST(ParameterBinding, Null_Int32) {
  ssize_t indicator = odbcabstraction::NULL_DATA;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = nullptr;
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_SLONG;

  auto array = BuildArrayFromBinding(arrow::int32(), binding);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(array->length(), 1);
  EXPECT_TRUE(array->IsNull(0));
}

TEST(ParameterBinding, Null_String) {
  ssize_t indicator = odbcabstraction::NULL_DATA;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = nullptr;
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_CHAR;

  auto array = BuildArrayFromBinding(arrow::utf8(), binding);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(array->length(), 1);
  EXPECT_TRUE(array->IsNull(0));
}

TEST(ParameterBinding, Null_Double) {
  ssize_t indicator = odbcabstraction::NULL_DATA;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = nullptr;
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_DOUBLE;

  auto array = BuildArrayFromBinding(arrow::float64(), binding);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(array->length(), 1);
  EXPECT_TRUE(array->IsNull(0));
}

TEST(ParameterBinding, Null_Date32) {
  ssize_t indicator = odbcabstraction::NULL_DATA;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = nullptr;
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_TYPE_DATE;

  auto array = BuildArrayFromBinding(arrow::date32(), binding);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(array->length(), 1);
  EXPECT_TRUE(array->IsNull(0));
}

TEST(ParameterBinding, Null_Timestamp) {
  ssize_t indicator = odbcabstraction::NULL_DATA;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = nullptr;
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_TYPE_TIMESTAMP;

  auto array = BuildArrayFromBinding(arrow::timestamp(arrow::TimeUnit::MICRO), binding);
  ASSERT_NE(array, nullptr);
  ASSERT_EQ(array->length(), 1);
  EXPECT_TRUE(array->IsNull(0));
}

// ============================================================================
// RecordBatch assembly test (simulates full SetParameters flow)
// ============================================================================

TEST(ParameterBinding, RecordBatch_MultipleParams) {
  // Simulate: SELECT * FROM t WHERE id = ? AND name = ? AND score > ?
  auto schema = arrow::schema({
    arrow::field("param1", arrow::int32()),
    arrow::field("param2", arrow::utf8()),
    arrow::field("param3", arrow::float64()),
  });

  // Param 1: int32 = 42
  int32_t id_val = 42;
  odbcabstraction::ParameterBinding p1 = {};
  p1.data_ptr = &id_val;
  p1.indicator_ptr = nullptr;
  p1.c_type = SQL_C_SLONG;

  // Param 2: string = "Alice"
  const char* name_val = "Alice";
  ssize_t name_ind = SQL_NTS;
  odbcabstraction::ParameterBinding p2 = {};
  p2.data_ptr = const_cast<char*>(name_val);
  p2.indicator_ptr = &name_ind;
  p2.c_type = SQL_C_CHAR;

  // Param 3: double = 95.5
  double score_val = 95.5;
  odbcabstraction::ParameterBinding p3 = {};
  p3.data_ptr = &score_val;
  p3.indicator_ptr = nullptr;
  p3.c_type = SQL_C_DOUBLE;

  // Build arrays (same as SetParameters)
  auto arr1 = BuildArrayFromBinding(schema->field(0)->type(), p1);
  auto arr2 = BuildArrayFromBinding(schema->field(1)->type(), p2);
  auto arr3 = BuildArrayFromBinding(schema->field(2)->type(), p3);

  ASSERT_NE(arr1, nullptr);
  ASSERT_NE(arr2, nullptr);
  ASSERT_NE(arr3, nullptr);

  // Assemble RecordBatch
  auto batch = arrow::RecordBatch::Make(schema, 1, {arr1, arr2, arr3});
  ASSERT_NE(batch, nullptr);
  ASSERT_EQ(batch->num_rows(), 1);
  ASSERT_EQ(batch->num_columns(), 3);

  // Verify values
  auto col0 = std::static_pointer_cast<arrow::Int32Array>(batch->column(0));
  EXPECT_EQ(col0->Value(0), 42);

  auto col1 = std::static_pointer_cast<arrow::StringArray>(batch->column(1));
  EXPECT_EQ(col1->GetString(0), "Alice");

  auto col2 = std::static_pointer_cast<arrow::DoubleArray>(batch->column(2));
  EXPECT_DOUBLE_EQ(col2->Value(0), 95.5);
}

// ============================================================================
// SQL_C_WCHAR parameter tests (Power BI sends values as wide strings)
// ============================================================================

// Helper: create a wide-char buffer from a UTF-8 string for testing.
// On macOS, SQLWCHAR is 4 bytes (UTF-32); on Windows, 2 bytes (UTF-16).
// For ASCII test strings, each char maps to one code unit either way.
static std::vector<uint8_t> MakeWcharBuffer(const char* utf8_str) {
  size_t wchar_size = odbcabstraction::GetSqlWCharSize();
  size_t len = strlen(utf8_str);
  // Allocate space for chars + null terminator
  std::vector<uint8_t> buf((len + 1) * wchar_size, 0);
  for (size_t i = 0; i < len; ++i) {
    buf[i * wchar_size] = static_cast<uint8_t>(utf8_str[i]);
    // Remaining bytes in each code unit are already zero-initialized
  }
  return buf;
}

TEST(ParameterBinding, Int64_from_WCHAR) {
  auto wbuf = MakeWcharBuffer("42");
  ssize_t indicator = SQL_NTS;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = wbuf.data();
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_WCHAR;

  auto array = BuildArrayFromBinding(arrow::int64(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::Int64Array>(array);
  EXPECT_EQ(typed->Value(0), 42);
}

TEST(ParameterBinding, Int32_from_WCHAR) {
  auto wbuf = MakeWcharBuffer("12345");
  ssize_t indicator = SQL_NTS;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = wbuf.data();
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_WCHAR;

  auto array = BuildArrayFromBinding(arrow::int32(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::Int32Array>(array);
  EXPECT_EQ(typed->Value(0), 12345);
}

TEST(ParameterBinding, Int16_from_WCHAR) {
  auto wbuf = MakeWcharBuffer("300");
  ssize_t indicator = SQL_NTS;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = wbuf.data();
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_WCHAR;

  auto array = BuildArrayFromBinding(arrow::int16(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::Int16Array>(array);
  EXPECT_EQ(typed->Value(0), 300);
}

TEST(ParameterBinding, Double_from_WCHAR) {
  auto wbuf = MakeWcharBuffer("3.14159");
  ssize_t indicator = SQL_NTS;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = wbuf.data();
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_WCHAR;

  auto array = BuildArrayFromBinding(arrow::float64(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::DoubleArray>(array);
  EXPECT_DOUBLE_EQ(typed->Value(0), 3.14159);
}

TEST(ParameterBinding, Float_from_WCHAR) {
  auto wbuf = MakeWcharBuffer("1.5");
  ssize_t indicator = SQL_NTS;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = wbuf.data();
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_WCHAR;

  auto array = BuildArrayFromBinding(arrow::float32(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::FloatArray>(array);
  EXPECT_FLOAT_EQ(typed->Value(0), 1.5f);
}

TEST(ParameterBinding, Boolean_from_WCHAR_true) {
  auto wbuf = MakeWcharBuffer("1");
  ssize_t indicator = SQL_NTS;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = wbuf.data();
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_WCHAR;

  auto array = BuildArrayFromBinding(arrow::boolean(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::BooleanArray>(array);
  EXPECT_TRUE(typed->Value(0));
}

TEST(ParameterBinding, Boolean_from_WCHAR_false) {
  auto wbuf = MakeWcharBuffer("0");
  ssize_t indicator = SQL_NTS;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = wbuf.data();
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_WCHAR;

  auto array = BuildArrayFromBinding(arrow::boolean(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::BooleanArray>(array);
  EXPECT_FALSE(typed->Value(0));
}

TEST(ParameterBinding, String_from_WCHAR) {
  auto wbuf = MakeWcharBuffer("hello world");
  ssize_t indicator = SQL_NTS;
  odbcabstraction::ParameterBinding binding = {};
  binding.data_ptr = wbuf.data();
  binding.indicator_ptr = &indicator;
  binding.c_type = SQL_C_WCHAR;

  auto array = BuildArrayFromBinding(arrow::utf8(), binding);
  ASSERT_NE(array, nullptr);
  auto typed = std::static_pointer_cast<arrow::StringArray>(array);
  EXPECT_EQ(typed->GetString(0), "hello world");
}

} // namespace flight_sql
} // namespace driver
