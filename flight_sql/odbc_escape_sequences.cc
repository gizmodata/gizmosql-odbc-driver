/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "odbc_escape_sequences.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace driver {
namespace flight_sql {

namespace {

// Map SQL_TSI_xxx interval keywords to DuckDB datepart strings (quoted, for date_diff).
std::string TranslateTsiToPart(const std::string &tsi) {
  std::string upper = tsi;
  std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

  if (upper == "SQL_TSI_FRAC_SECOND") return "'microsecond'";
  if (upper == "SQL_TSI_SECOND")      return "'second'";
  if (upper == "SQL_TSI_MINUTE")      return "'minute'";
  if (upper == "SQL_TSI_HOUR")        return "'hour'";
  if (upper == "SQL_TSI_DAY")         return "'day'";
  if (upper == "SQL_TSI_WEEK")        return "'week'";
  if (upper == "SQL_TSI_MONTH")       return "'month'";
  if (upper == "SQL_TSI_QUARTER")     return "'quarter'";
  if (upper == "SQL_TSI_YEAR")        return "'year'";
  return tsi;
}

// Map SQL_TSI_xxx interval keywords to DuckDB INTERVAL unit names (unquoted, for date_add).
std::string TranslateTsiToUnit(const std::string &tsi) {
  std::string upper = tsi;
  std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

  if (upper == "SQL_TSI_FRAC_SECOND") return "MICROSECOND";
  if (upper == "SQL_TSI_SECOND")      return "SECOND";
  if (upper == "SQL_TSI_MINUTE")      return "MINUTE";
  if (upper == "SQL_TSI_HOUR")        return "HOUR";
  if (upper == "SQL_TSI_DAY")         return "DAY";
  if (upper == "SQL_TSI_WEEK")        return "WEEK";
  if (upper == "SQL_TSI_MONTH")       return "MONTH";
  if (upper == "SQL_TSI_QUARTER")     return "QUARTER";
  if (upper == "SQL_TSI_YEAR")        return "YEAR";
  return tsi;
}

// Skip over a single-quoted string literal starting at pos (which points to the
// opening quote). Returns the index after the closing quote.
size_t SkipSingleQuoted(const std::string &sql, size_t pos) {
  pos++; // skip opening quote
  while (pos < sql.size()) {
    if (sql[pos] == '\'') {
      if (pos + 1 < sql.size() && sql[pos + 1] == '\'') {
        pos += 2; // escaped quote
      } else {
        return pos + 1; // past closing quote
      }
    } else {
      pos++;
    }
  }
  return pos; // unterminated — return end
}

// Skip over a double-quoted identifier starting at pos.
size_t SkipDoubleQuoted(const std::string &sql, size_t pos) {
  pos++; // skip opening quote
  while (pos < sql.size()) {
    if (sql[pos] == '"') {
      if (pos + 1 < sql.size() && sql[pos + 1] == '"') {
        pos += 2; // escaped quote
      } else {
        return pos + 1;
      }
    } else {
      pos++;
    }
  }
  return pos;
}

// Find the matching closing '}' for an escape sequence starting at pos
// (which points to the opening '{'). Handles nesting and string literals.
size_t FindClosingBrace(const std::string &sql, size_t pos) {
  int depth = 0;
  while (pos < sql.size()) {
    char c = sql[pos];
    if (c == '\'') {
      pos = SkipSingleQuoted(sql, pos);
    } else if (c == '"') {
      pos = SkipDoubleQuoted(sql, pos);
    } else if (c == '{') {
      depth++;
      pos++;
    } else if (c == '}') {
      depth--;
      if (depth == 0) return pos;
      pos++;
    } else {
      pos++;
    }
  }
  return std::string::npos; // no matching brace
}

// Trim leading and trailing whitespace.
std::string Trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

// Case-insensitive prefix check.
bool StartsWithCI(const std::string &s, const std::string &prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); i++) {
    if (std::toupper(s[i]) != std::toupper(prefix[i])) return false;
  }
  return true;
}

// Split function arguments respecting parentheses, braces, and quotes.
// Splits on top-level commas only.
std::vector<std::string> SplitArgs(const std::string &s) {
  std::vector<std::string> args;
  int parenDepth = 0;
  int braceDepth = 0;
  size_t start = 0;
  size_t i = 0;

  while (i < s.size()) {
    char c = s[i];
    if (c == '\'') {
      i = SkipSingleQuoted(s, i);
    } else if (c == '"') {
      i = SkipDoubleQuoted(s, i);
    } else if (c == '(') {
      parenDepth++;
      i++;
    } else if (c == ')') {
      parenDepth--;
      i++;
    } else if (c == '{') {
      braceDepth++;
      i++;
    } else if (c == '}') {
      braceDepth--;
      i++;
    } else if (c == ',' && parenDepth == 0 && braceDepth == 0) {
      args.push_back(Trim(s.substr(start, i - start)));
      start = i + 1;
      i++;
    } else {
      i++;
    }
  }
  // Last argument
  std::string last = Trim(s.substr(start));
  if (!last.empty()) {
    args.push_back(last);
  }
  return args;
}

// Extract function name and the rest (arguments in parentheses) from
// a string like "timestampadd(SQL_TSI_DAY, expr, expr)".
// Returns function name (lowercased) and the raw arguments string (without parens).
bool ParseFunctionCall(const std::string &body, std::string &funcName, std::string &argsStr) {
  size_t paren = body.find('(');
  if (paren == std::string::npos) return false;

  funcName = Trim(body.substr(0, paren));
  std::transform(funcName.begin(), funcName.end(), funcName.begin(), ::tolower);

  // Find matching closing paren
  int depth = 0;
  size_t endParen = std::string::npos;
  for (size_t i = paren; i < body.size(); i++) {
    char c = body[i];
    if (c == '\'') {
      i = SkipSingleQuoted(body, i) - 1;
    } else if (c == '"') {
      i = SkipDoubleQuoted(body, i) - 1;
    } else if (c == '{') {
      // Skip nested braces within the args (they'll be translated recursively)
      // Just count depth
      depth++;
    } else if (c == '}') {
      depth--;
    } else if (c == '(') {
      depth++;
    } else if (c == ')') {
      depth--;
      if (depth == 0) {
        endParen = i;
        break;
      }
    }
  }

  if (endParen == std::string::npos) return false;
  argsStr = body.substr(paren + 1, endParen - paren - 1);
  return true;
}

// Translate a single { fn ... } body (after "fn " prefix) to native SQL.
// The body has already been recursively processed for nested escapes.
std::string TranslateScalarFunction(const std::string &body) {
  std::string funcName, argsStr;
  if (!ParseFunctionCall(body, funcName, argsStr)) {
    // Can't parse — return as-is
    return body;
  }

  auto args = SplitArgs(argsStr);

  // timestampadd(interval, count, base) → date_add(base, INTERVAL (count) UNIT)
  if (funcName == "timestampadd" && args.size() == 3) {
    return "date_add(" + args[2] + ", INTERVAL (" + args[1] + ") " + TranslateTsiToUnit(args[0]) + ")";
  }

  // timestampdiff(interval, ts1, ts2) → date_diff('part', ts1, ts2)
  if (funcName == "timestampdiff" && args.size() == 3) {
    return "date_diff(" + TranslateTsiToPart(args[0]) + ", " + args[1] + ", " + args[2] + ")";
  }

  // CURDATE() → CURRENT_DATE
  if (funcName == "curdate" && args.empty()) {
    return "CURRENT_DATE";
  }

  // CURTIME() → CURRENT_TIME
  if (funcName == "curtime" && args.empty()) {
    return "CURRENT_TIME";
  }

  // NOW() → NOW()
  if (funcName == "now" && args.empty()) {
    return "NOW()";
  }

  // DAYOFMONTH(x) → DAY(x)
  if (funcName == "dayofmonth" && args.size() == 1) {
    return "DAY(" + args[0] + ")";
  }

  // DAYOFWEEK(x) → DAYOFWEEK(x)  (DuckDB supports this)
  // DAYOFYEAR(x) → DAYOFYEAR(x)
  // YEAR, MONTH, QUARTER, WEEK, HOUR, MINUTE, SECOND — DuckDB supports all natively
  // DAYNAME, MONTHNAME — DuckDB supports these
  // For all of these, just strip the {fn } wrapper and pass through.

  // Default: just output funcname(args) — strip the {fn} wrapper
  return body;
}

// Translate a single escape sequence. `inner` is the content between { and }.
// It has NOT been recursively processed yet; this function handles recursion.
std::string TranslateEscape(const std::string &inner) {
  std::string trimmed = Trim(inner);

  // First, recursively translate any nested escape sequences in the body.
  std::string processed = TranslateOdbcEscapes(trimmed);

  // Date literal: d 'YYYY-MM-DD'
  if (StartsWithCI(processed, "d ") || StartsWithCI(processed, "d\t")) {
    std::string val = Trim(processed.substr(1));
    return "DATE " + val;
  }

  // Timestamp literal: ts 'YYYY-MM-DD HH:MM:SS'
  if (StartsWithCI(processed, "ts ") || StartsWithCI(processed, "ts\t")) {
    std::string val = Trim(processed.substr(2));
    return "TIMESTAMP " + val;
  }

  // Time literal: t 'HH:MM:SS'
  if (StartsWithCI(processed, "t ") || StartsWithCI(processed, "t\t")) {
    // Make sure we don't match "ts" — already handled above
    std::string val = Trim(processed.substr(1));
    return "TIME " + val;
  }

  // Scalar function: fn funcname(args)
  if (StartsWithCI(processed, "fn ") || StartsWithCI(processed, "fn\t")) {
    std::string body = Trim(processed.substr(2));
    return TranslateScalarFunction(body);
  }

  // Unknown escape type — return content as-is (without braces)
  return processed;
}

} // anonymous namespace

std::string TranslateOdbcEscapes(const std::string &sql) {
  std::string result;
  result.reserve(sql.size());
  size_t pos = 0;

  while (pos < sql.size()) {
    char c = sql[pos];

    // Skip string literals
    if (c == '\'') {
      size_t end = SkipSingleQuoted(sql, pos);
      result.append(sql, pos, end - pos);
      pos = end;
    } else if (c == '"') {
      size_t end = SkipDoubleQuoted(sql, pos);
      result.append(sql, pos, end - pos);
      pos = end;
    } else if (c == '{') {
      // Found potential escape sequence
      size_t closeBrace = FindClosingBrace(sql, pos);
      if (closeBrace == std::string::npos) {
        // No matching brace — output as-is
        result += c;
        pos++;
      } else {
        // Extract content between { and }
        std::string inner = sql.substr(pos + 1, closeBrace - pos - 1);
        result += TranslateEscape(inner);
        pos = closeBrace + 1;
      }
    } else {
      result += c;
      pos++;
    }
  }

  return result;
}

} // namespace flight_sql
} // namespace driver
