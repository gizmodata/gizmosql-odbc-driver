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
#include <mutex>
#include <odbcabstraction/spi/connection.h>
#include <string>

namespace driver {
namespace flight_sql {

/// Middleware that captures the x-gizmosql-oauth-url gRPC response header
/// from a single call.
class OAuthDiscoveryMiddleware : public arrow::flight::ClientMiddleware {
public:
  explicit OAuthDiscoveryMiddleware(class OAuthDiscoveryMiddlewareFactory *factory);
  void SendingHeaders(arrow::flight::AddCallHeaders *outgoing_headers) override;
  void ReceivedHeaders(const arrow::flight::CallHeaders &incoming_headers) override;
  void CallCompleted(const arrow::Status &status) override;

private:
  OAuthDiscoveryMiddlewareFactory *factory_;
};

/// Middleware factory that captures the x-gizmosql-oauth-url gRPC response header.
class OAuthDiscoveryMiddlewareFactory : public arrow::flight::ClientMiddlewareFactory {
public:
  void StartCall(const arrow::flight::CallInfo &info,
                 std::unique_ptr<arrow::flight::ClientMiddleware> *middleware) override;
  std::string GetDiscoveredUrl() const;
  void SetDiscoveredUrl(const std::string &url);

private:
  std::string discovered_url_;
  mutable std::mutex mutex_;
};

class FlightSqlAuthMethod {
public:
  virtual ~FlightSqlAuthMethod() = default;

  virtual void Authenticate(FlightSqlConnection &connection,
                            arrow::flight::FlightCallOptions &call_options) = 0;

  virtual std::string GetUser() { return std::string(); }

  static std::unique_ptr<FlightSqlAuthMethod> FromProperties(
      const std::unique_ptr<arrow::flight::FlightClient> &client,
      const odbcabstraction::Connection::ConnPropertyMap &properties,
      std::shared_ptr<OAuthDiscoveryMiddlewareFactory> oauth_discovery = nullptr);

protected:
  FlightSqlAuthMethod() = default;
};

/// OAuth authentication using the GizmoSQL server's 3-phase protocol:
/// 1. Discover the OAuth base URL via x-gizmosql-oauth-url gRPC response header
/// 2. Initiate OAuth session and poll for token via HTTP REST endpoints
/// 3. Exchange access token via Flight Handshake with username="token"
class OAuthAuthMethod : public FlightSqlAuthMethod {
public:
  OAuthAuthMethod(arrow::flight::FlightClient &client,
                  const std::string &host, int port, bool use_encryption,
                  int oauth_server_port, bool disable_cert_verification,
                  std::shared_ptr<OAuthDiscoveryMiddlewareFactory> discovery_factory);

  void Authenticate(FlightSqlConnection &connection,
                    arrow::flight::FlightCallOptions &call_options) override;

private:
  /// Phase 1: Trigger a gRPC call and read the x-gizmosql-oauth-url header
  /// from the discovery middleware. Falls back to constructed URL if not present.
  std::string DiscoverOAuthUrl(FlightSqlConnection &connection);

  /// Phase 2: HTTP REST flow — initiate session, open browser, poll for token.
  std::string PerformHttpOAuthFlow(const std::string &oauth_base_url);

  /// Send an HTTP(S) GET request and return the response body.
  std::string HttpGet(const std::string &url);

  /// Phase 3: Exchange the access token for a bearer token via gRPC Handshake.
  void ExchangeToken(const std::string &access_token,
                     arrow::flight::FlightCallOptions &call_options);

  /// Open the user's default browser to the given URL.
  static void LaunchBrowser(const std::string &url);

public:
  /// Clear any cached bearer token for the given host:port.
  static void ClearCachedToken(const std::string &host, int port);

private:
  arrow::flight::FlightClient &client_;
  std::string host_;
  int port_;
  bool use_encryption_;
  int oauth_server_port_;
  bool disable_cert_verification_;
  std::shared_ptr<OAuthDiscoveryMiddlewareFactory> discovery_factory_;
};

} // namespace flight_sql
} // namespace driver
