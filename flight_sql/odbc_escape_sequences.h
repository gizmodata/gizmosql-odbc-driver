/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include <string>

namespace driver {
namespace flight_sql {

/// Translate ODBC escape sequences in a SQL string to native DuckDB SQL.
///
/// Handles:
///   {d 'YYYY-MM-DD'}                → DATE 'YYYY-MM-DD'
///   {t 'HH:MM:SS'}                  → TIME 'HH:MM:SS'
///   {ts 'YYYY-MM-DD HH:MM:SS...'}  → TIMESTAMP 'YYYY-MM-DD HH:MM:SS...'
///   { fn funcname(args) }           → native DuckDB function call
///
/// Nested escape sequences (e.g. { fn f({ fn g(...) }) }) are supported.
std::string TranslateOdbcEscapes(const std::string &sql);

} // namespace flight_sql
} // namespace driver
