/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include <memory>
#include <flight_sql/flight_sql_driver.h>

namespace gizmosql {
namespace odbc {

/// Returns the global FlightSqlDriver singleton, creating it on first call.
/// Thread-safe via std::call_once.
std::shared_ptr<driver::flight_sql::FlightSqlDriver> GetGlobalDriver();

} // namespace odbc
} // namespace gizmosql
