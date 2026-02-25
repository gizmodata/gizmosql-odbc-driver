/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "odbc_escape_sequences.h"
#include "gtest/gtest.h"

namespace driver {
namespace flight_sql {

// === Date/Time/Timestamp Literals ===

TEST(OdbcEscapeSequences, DateLiteral) {
  EXPECT_EQ("DATE '2000-01-01'", TranslateOdbcEscapes("{d '2000-01-01'}"));
}

TEST(OdbcEscapeSequences, TimeLiteral) {
  EXPECT_EQ("TIME '12:30:00'", TranslateOdbcEscapes("{t '12:30:00'}"));
}

TEST(OdbcEscapeSequences, TimestampLiteral) {
  EXPECT_EQ("TIMESTAMP '2000-01-01 00:00:00'",
            TranslateOdbcEscapes("{ts '2000-01-01 00:00:00'}"));
}

TEST(OdbcEscapeSequences, TimestampLiteralWithFraction) {
  EXPECT_EQ("TIMESTAMP '2024-06-15 13:45:30.123'",
            TranslateOdbcEscapes("{ts '2024-06-15 13:45:30.123'}"));
}

// === Scalar Functions ===

TEST(OdbcEscapeSequences, FnYear) {
  EXPECT_EQ("YEAR(\"col\")", TranslateOdbcEscapes("{ fn YEAR(\"col\") }"));
}

TEST(OdbcEscapeSequences, FnMonth) {
  EXPECT_EQ("MONTH(\"col\")", TranslateOdbcEscapes("{ fn MONTH(\"col\") }"));
}

TEST(OdbcEscapeSequences, FnDayOfMonth) {
  EXPECT_EQ("DAY(\"col\")", TranslateOdbcEscapes("{ fn DAYOFMONTH(\"col\") }"));
}

TEST(OdbcEscapeSequences, FnQuarter) {
  EXPECT_EQ("QUARTER(x)", TranslateOdbcEscapes("{ fn QUARTER(x) }"));
}

TEST(OdbcEscapeSequences, FnHour) {
  EXPECT_EQ("HOUR(x)", TranslateOdbcEscapes("{ fn HOUR(x) }"));
}

TEST(OdbcEscapeSequences, FnMinute) {
  EXPECT_EQ("MINUTE(x)", TranslateOdbcEscapes("{ fn MINUTE(x) }"));
}

TEST(OdbcEscapeSequences, FnSecond) {
  EXPECT_EQ("SECOND(x)", TranslateOdbcEscapes("{ fn SECOND(x) }"));
}

TEST(OdbcEscapeSequences, FnNow) {
  EXPECT_EQ("NOW()", TranslateOdbcEscapes("{ fn NOW() }"));
}

TEST(OdbcEscapeSequences, FnCurdate) {
  EXPECT_EQ("CURRENT_DATE", TranslateOdbcEscapes("{ fn CURDATE() }"));
}

TEST(OdbcEscapeSequences, FnCurtime) {
  EXPECT_EQ("CURRENT_TIME", TranslateOdbcEscapes("{ fn CURTIME() }"));
}

// === TIMESTAMPADD / TIMESTAMPDIFF ===

TEST(OdbcEscapeSequences, TimestampAdd_Day) {
  EXPECT_EQ("date_add(\"col\", INTERVAL (5) DAY)",
            TranslateOdbcEscapes("{ fn timestampadd(SQL_TSI_DAY, 5, \"col\") }"));
}

TEST(OdbcEscapeSequences, TimestampAdd_Month) {
  EXPECT_EQ("date_add(x, INTERVAL (3) MONTH)",
            TranslateOdbcEscapes("{ fn timestampadd(SQL_TSI_MONTH, 3, x) }"));
}

TEST(OdbcEscapeSequences, TimestampAdd_Year) {
  EXPECT_EQ("date_add(x, INTERVAL (1) YEAR)",
            TranslateOdbcEscapes("{ fn timestampadd(SQL_TSI_YEAR, 1, x) }"));
}

TEST(OdbcEscapeSequences, TimestampDiff_Day) {
  EXPECT_EQ("date_diff('day', \"a\", \"b\")",
            TranslateOdbcEscapes("{ fn timestampdiff(SQL_TSI_DAY, \"a\", \"b\") }"));
}

TEST(OdbcEscapeSequences, TimestampDiff_Second) {
  EXPECT_EQ("date_diff('second', x, y)",
            TranslateOdbcEscapes("{ fn timestampdiff(SQL_TSI_SECOND, x, y) }"));
}

// === Nested Escapes ===

TEST(OdbcEscapeSequences, NestedDateInFunction) {
  // { fn timestampdiff(SQL_TSI_DAY, {d '2000-01-01'}, "C1") }
  EXPECT_EQ("date_diff('day', DATE '2000-01-01', \"C1\")",
            TranslateOdbcEscapes("{ fn timestampdiff(SQL_TSI_DAY, {d '2000-01-01'}, \"C1\") }"));
}

TEST(OdbcEscapeSequences, NestedFunctionInFunction) {
  // { fn timestampadd(SQL_TSI_DAY, { fn timestampdiff(SQL_TSI_DAY, {d '2000-01-01'}, "C1") }, {ts '2000-01-01 00:00:00'}) }
  std::string input =
      "{ fn timestampadd(SQL_TSI_DAY, "
      "{ fn timestampdiff(SQL_TSI_DAY, {d '2000-01-01'}, \"C1\") }, "
      "{ts '2000-01-01 00:00:00'}) }";
  std::string expected =
      "date_add(TIMESTAMP '2000-01-01 00:00:00', "
      "INTERVAL (date_diff('day', DATE '2000-01-01', \"C1\")) DAY)";
  EXPECT_EQ(expected, TranslateOdbcEscapes(input));
}

// === Real Power BI Query (from trace) ===

TEST(OdbcEscapeSequences, PowerBIDateQuery) {
  std::string input =
      "select \"l_commitdate\"\r\n"
      "from \r\n"
      "(\r\n"
      "    select \"l_commitdate\",\r\n"
      "        \"C1\",\r\n"
      "        case\r\n"
      "            when \"C1\" is not null\r\n"
      "            then { fn timestampadd(SQL_TSI_DAY, { fn timestampdiff(SQL_TSI_DAY, {d '2000-01-01'}, \"C1\") }, {ts '2000-01-01 00:00:00'}) }\r\n"
      "            else {ts '1899-12-28 00:00:00'}\r\n"
      "        end as \"C2\",\r\n"
      "        case\r\n"
      "            when \"C1\" is null\r\n"
      "            then 0\r\n"
      "            else 1\r\n"
      "        end as \"C3\"\r\n"
      "    from \r\n"
      "    (\r\n"
      "        select \"l_commitdate\",\r\n"
      "            \"l_commitdate\" as \"C1\"\r\n"
      "        from \"memory\".\"main\".\"lineitem\"\r\n"
      "    ) as \"ITBL\"\r\n"
      "    group by \"l_commitdate\",\r\n"
      "        \"C1\"\r\n"
      ") as \"ITBL\"\r\n"
      "order by \"ITBL\".\"C2\",\r\n"
      "        \"ITBL\".\"C3\"\r\n"
      "limit 501";

  std::string result = TranslateOdbcEscapes(input);

  // Should NOT contain any { or } escape sequences
  EXPECT_EQ(std::string::npos, result.find("{fn"));
  EXPECT_EQ(std::string::npos, result.find("{d "));
  EXPECT_EQ(std::string::npos, result.find("{ts "));

  // Should contain translated functions
  EXPECT_NE(std::string::npos, result.find("date_add("));
  EXPECT_NE(std::string::npos, result.find("date_diff("));
  EXPECT_NE(std::string::npos, result.find("DATE '2000-01-01'"));
  EXPECT_NE(std::string::npos, result.find("TIMESTAMP '2000-01-01 00:00:00'"));
  EXPECT_NE(std::string::npos, result.find("TIMESTAMP '1899-12-28 00:00:00'"));
}

// === Passthrough (no escapes) ===

TEST(OdbcEscapeSequences, NoEscapes) {
  std::string sql = "SELECT \"col\" FROM \"table\" WHERE x = 1";
  EXPECT_EQ(sql, TranslateOdbcEscapes(sql));
}

TEST(OdbcEscapeSequences, BracesInsideSingleQuotedString) {
  // Braces inside string literals should NOT be translated
  std::string sql = "SELECT '{d ''2000-01-01''}' AS x";
  EXPECT_EQ(sql, TranslateOdbcEscapes(sql));
}

TEST(OdbcEscapeSequences, BracesInsideDoubleQuotedIdentifier) {
  // Braces inside double-quoted identifiers should NOT be translated
  std::string sql = "SELECT \"{fn col}\" FROM t";
  EXPECT_EQ(sql, TranslateOdbcEscapes(sql));
}

TEST(OdbcEscapeSequences, EmptyString) {
  EXPECT_EQ("", TranslateOdbcEscapes(""));
}

// === All SQL_TSI intervals ===

TEST(OdbcEscapeSequences, AllTsiIntervals) {
  EXPECT_EQ("date_add(x, INTERVAL (1) MICROSECOND)",
            TranslateOdbcEscapes("{ fn timestampadd(SQL_TSI_FRAC_SECOND, 1, x) }"));
  EXPECT_EQ("date_add(x, INTERVAL (1) SECOND)",
            TranslateOdbcEscapes("{ fn timestampadd(SQL_TSI_SECOND, 1, x) }"));
  EXPECT_EQ("date_add(x, INTERVAL (1) MINUTE)",
            TranslateOdbcEscapes("{ fn timestampadd(SQL_TSI_MINUTE, 1, x) }"));
  EXPECT_EQ("date_add(x, INTERVAL (1) HOUR)",
            TranslateOdbcEscapes("{ fn timestampadd(SQL_TSI_HOUR, 1, x) }"));
  EXPECT_EQ("date_add(x, INTERVAL (1) DAY)",
            TranslateOdbcEscapes("{ fn timestampadd(SQL_TSI_DAY, 1, x) }"));
  EXPECT_EQ("date_add(x, INTERVAL (1) WEEK)",
            TranslateOdbcEscapes("{ fn timestampadd(SQL_TSI_WEEK, 1, x) }"));
  EXPECT_EQ("date_add(x, INTERVAL (1) MONTH)",
            TranslateOdbcEscapes("{ fn timestampadd(SQL_TSI_MONTH, 1, x) }"));
  EXPECT_EQ("date_add(x, INTERVAL (1) QUARTER)",
            TranslateOdbcEscapes("{ fn timestampadd(SQL_TSI_QUARTER, 1, x) }"));
  EXPECT_EQ("date_add(x, INTERVAL (1) YEAR)",
            TranslateOdbcEscapes("{ fn timestampadd(SQL_TSI_YEAR, 1, x) }"));
}

// === Mixed escapes and plain SQL ===

TEST(OdbcEscapeSequences, MixedContent) {
  std::string input = "SELECT * FROM t WHERE d > {d '2024-01-01'} AND x = { fn YEAR(\"col\") }";
  std::string expected = "SELECT * FROM t WHERE d > DATE '2024-01-01' AND x = YEAR(\"col\")";
  EXPECT_EQ(expected, TranslateOdbcEscapes(input));
}

// === LIKE Escape Character ===

TEST(OdbcEscapeSequences, LikeEscapeChar) {
  EXPECT_EQ("SELECT * FROM t WHERE col LIKE '%\\_%' ESCAPE '\\'",
            TranslateOdbcEscapes("SELECT * FROM t WHERE col LIKE '%\\_%' {escape '\\'}"));
}

TEST(OdbcEscapeSequences, LikeEscapeCharHash) {
  EXPECT_EQ("SELECT * FROM t WHERE col LIKE 'foo#%bar' ESCAPE '#'",
            TranslateOdbcEscapes("SELECT * FROM t WHERE col LIKE 'foo#%bar' {escape '#'}"));
}

// === Outer Join Escape ===

TEST(OdbcEscapeSequences, OuterJoinEscape) {
  EXPECT_EQ("\"t1\" LEFT OUTER JOIN \"t2\" ON \"t1\".\"id\" = \"t2\".\"id\"",
            TranslateOdbcEscapes("{oj \"t1\" LEFT OUTER JOIN \"t2\" ON \"t1\".\"id\" = \"t2\".\"id\"}"));
}

// === CONVERT (type casting) ===

TEST(OdbcEscapeSequences, ConvertToVarchar) {
  EXPECT_EQ("CAST(\"col\" AS VARCHAR)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_VARCHAR) }"));
}

TEST(OdbcEscapeSequences, ConvertToInteger) {
  EXPECT_EQ("CAST(\"col\" AS INTEGER)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_INTEGER) }"));
}

TEST(OdbcEscapeSequences, ConvertToBigint) {
  EXPECT_EQ("CAST(\"col\" AS BIGINT)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_BIGINT) }"));
}

TEST(OdbcEscapeSequences, ConvertToDouble) {
  EXPECT_EQ("CAST(\"col\" AS DOUBLE)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_DOUBLE) }"));
}

TEST(OdbcEscapeSequences, ConvertToFloat) {
  EXPECT_EQ("CAST(\"col\" AS FLOAT)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_FLOAT) }"));
}

TEST(OdbcEscapeSequences, ConvertToDate) {
  EXPECT_EQ("CAST(\"col\" AS DATE)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_TYPE_DATE) }"));
}

TEST(OdbcEscapeSequences, ConvertToTimestamp) {
  EXPECT_EQ("CAST(\"col\" AS TIMESTAMP)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_TYPE_TIMESTAMP) }"));
}

TEST(OdbcEscapeSequences, ConvertToBoolean) {
  EXPECT_EQ("CAST(\"col\" AS BOOLEAN)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_BIT) }"));
}

TEST(OdbcEscapeSequences, ConvertToSmallint) {
  EXPECT_EQ("CAST(\"col\" AS SMALLINT)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_SMALLINT) }"));
}

TEST(OdbcEscapeSequences, ConvertToDecimal) {
  EXPECT_EQ("CAST(\"col\" AS DECIMAL)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_DECIMAL) }"));
}

TEST(OdbcEscapeSequences, ConvertToBlob) {
  EXPECT_EQ("CAST(\"col\" AS BLOB)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_VARBINARY) }"));
}

TEST(OdbcEscapeSequences, ConvertToTime) {
  EXPECT_EQ("CAST(\"col\" AS TIME)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_TYPE_TIME) }"));
}

TEST(OdbcEscapeSequences, ConvertToWVarchar) {
  EXPECT_EQ("CAST(\"col\" AS VARCHAR)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_WVARCHAR) }"));
}

TEST(OdbcEscapeSequences, ConvertToTinyint) {
  EXPECT_EQ("CAST(\"col\" AS TINYINT)",
            TranslateOdbcEscapes("{ fn CONVERT(\"col\", SQL_TINYINT) }"));
}

} // namespace flight_sql
} // namespace driver
