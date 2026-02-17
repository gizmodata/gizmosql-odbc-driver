/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include "flight_sql_connection.h"
#include <arrow/flight/client.h>
#include <map>
#include <memory>
#include <odbcabstraction/spi/connection.h>
#include <string>

namespace driver {
namespace flight_sql {

class FlightSqlAuthMethod {
public:
  virtual ~FlightSqlAuthMethod() = default;

  virtual void Authenticate(FlightSqlConnection &connection,
                            arrow::flight::FlightCallOptions &call_options) = 0;

  virtual std::string GetUser() { return std::string(); }

  static std::unique_ptr<FlightSqlAuthMethod> FromProperties(
      const std::unique_ptr<arrow::flight::FlightClient> &client,
      const odbcabstraction::Connection::ConnPropertyMap &properties);

protected:
  FlightSqlAuthMethod() = default;
};

/// OAuth authentication using the server-side discovery flow.
/// Sends username=__discover__ to the server, receives an OAuth URL,
/// opens the system browser for user authentication, and receives a
/// bearer token from the server.
class OAuthAuthMethod : public FlightSqlAuthMethod {
public:
  OAuthAuthMethod(arrow::flight::FlightClient &client,
                  const std::string &host, int port, bool use_encryption);

  void Authenticate(FlightSqlConnection &connection,
                    arrow::flight::FlightCallOptions &call_options) override;

private:
  /// Send __discover__ credentials to the server and parse the OAuth URL
  /// from the response.
  std::string Discover(FlightSqlConnection &connection,
                       arrow::flight::FlightCallOptions &call_options);

  /// Open the user's default browser to the given URL.
  static void LaunchBrowser(const std::string &url);

  /// Poll the server for a bearer token after the user completes browser auth.
  std::string WaitForToken(FlightSqlConnection &connection,
                           arrow::flight::FlightCallOptions &call_options);

  arrow::flight::FlightClient &client_;
  std::string host_;
  int port_;
  bool use_encryption_;
};

} // namespace flight_sql
} // namespace driver
