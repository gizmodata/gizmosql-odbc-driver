/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "flight_sql_auth_method.h"

#include <odbcabstraction/platform.h>

#include "flight_sql_connection.h"
#include <odbcabstraction/exceptions.h>
#include <odbcabstraction/logger.h>

#include <arrow/flight/client.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include <chrono>
#include <thread>
#include <utility>

#if defined(__APPLE__)
#include <cstdlib>
#elif defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#include <shellapi.h>
#else
#include <cstdlib>
#endif

using namespace driver::flight_sql;

namespace driver {
namespace flight_sql {

using arrow::Result;
using arrow::flight::FlightCallOptions;
using arrow::flight::FlightClient;
using arrow::flight::TimeoutDuration;
using driver::odbcabstraction::AuthenticationException;
using driver::odbcabstraction::CommunicationException;
using driver::odbcabstraction::Connection;

namespace {
class NoOpClientAuthHandler : public arrow::flight::ClientAuthHandler {
public:
  NoOpClientAuthHandler() {}

  arrow::Status Authenticate(arrow::flight::ClientAuthSender* outgoing, arrow::flight::ClientAuthReader* incoming) override {
    // Write a blank string. The server should ignore this and just accept any Handshake request.
    return outgoing->Write(std::string());
  }

  arrow::Status GetToken(std::string* token) override {
    *token = std::string();
    return arrow::Status::OK();
  }
};

class UserPasswordAuthMethod : public FlightSqlAuthMethod {
public:
  UserPasswordAuthMethod(FlightClient &client, std::string user,
                         std::string password)
      : client_(client), user_(std::move(user)),
        password_(std::move(password)) {}

  void Authenticate(FlightSqlConnection &connection,
                    FlightCallOptions &call_options) override {
    FlightCallOptions auth_call_options;
    const boost::optional<Connection::Attribute> &login_timeout =
        connection.GetAttribute(Connection::LOGIN_TIMEOUT);
    if (login_timeout && boost::get<uint32_t>(*login_timeout) > 0) {
      // ODBC's LOGIN_TIMEOUT attribute and FlightCallOptions.timeout use
      // seconds as time unit.
      double timeout_seconds = static_cast<double>(boost::get<uint32_t>(*login_timeout));
      if (timeout_seconds > 0) {
        auth_call_options.timeout = TimeoutDuration{timeout_seconds};
      }
    }

    Result<std::pair<std::string, std::string>> bearer_result =
        client_.AuthenticateBasicToken(auth_call_options, user_, password_);

    if (!bearer_result.ok()) {
      const auto& flightStatus = arrow::flight::FlightStatusDetail::UnwrapStatus(bearer_result.status());
      if (flightStatus != nullptr) {
        if (flightStatus->code() == arrow::flight::FlightStatusCode::Unauthenticated) {
          throw AuthenticationException("Failed to authenticate with user and password: " +
                                        bearer_result.status().ToString());
        } else if (flightStatus->code() == arrow::flight::FlightStatusCode::Unavailable) {
          throw CommunicationException(bearer_result.status().message());
        }
      }

      throw odbcabstraction::DriverException(bearer_result.status().message());
    }

    call_options.headers.push_back(bearer_result.ValueOrDie());
  }

  std::string GetUser() override { return user_; }

private:
  FlightClient &client_;
  std::string user_;
  std::string password_;
};

    class TokenAuthMethod : public FlightSqlAuthMethod {
    private:
        FlightClient &client_;
        std::string token_; // this is the token the user provides

    public:
        TokenAuthMethod(FlightClient &client, std::string token): client_{client}, token_{std::move(token)} {}

        void Authenticate(FlightSqlConnection &connection, FlightCallOptions &call_options) override {
            // add the token to the headers
            const std::pair<std::string, std::string> token_header("authorization", "Bearer " + token_);
            call_options.headers.push_back(token_header);

            const arrow::Status status = client_.Authenticate(call_options, std::unique_ptr<arrow::flight::ClientAuthHandler>(new NoOpClientAuthHandler()));
            if (!status.ok()) {
                const auto& flightStatus = arrow::flight::FlightStatusDetail::UnwrapStatus(status);
                if (flightStatus != nullptr) {
                    if (flightStatus->code() == arrow::flight::FlightStatusCode::Unauthenticated) {
                        throw AuthenticationException("Failed to authenticate with token: " + token_ + " Message: " + status.message());
                    } else if (flightStatus->code() == arrow::flight::FlightStatusCode::Unavailable) {
                      throw CommunicationException(status.message());
                    }
                }
                throw odbcabstraction::DriverException(status.message());
            }
        }
    };
} // namespace

std::unique_ptr<FlightSqlAuthMethod> FlightSqlAuthMethod::FromProperties(
    const std::unique_ptr<FlightClient> &client,
    const Connection::ConnPropertyMap &properties) {

  // Check if authType=external is set for OAuth
  auto it_auth_type = properties.find(FlightSqlConnection::AUTH_TYPE);
  if (it_auth_type != properties.end() && it_auth_type->second == "external") {
    auto it_host = properties.find(FlightSqlConnection::HOST);
    auto it_port = properties.find(FlightSqlConnection::PORT);
    const std::string &host = it_host != properties.end() ? it_host->second : "localhost";
    int port = it_port != properties.end() ? std::stoi(it_port->second) : 32010;

    auto it_encryption = properties.find(FlightSqlConnection::USE_ENCRYPTION);
    bool use_encryption = true;
    if (it_encryption != properties.end()) {
      auto val = odbcabstraction::AsBool(it_encryption->second);
      if (val) {
        use_encryption = *val;
      }
    }

    return std::unique_ptr<FlightSqlAuthMethod>(
        new OAuthAuthMethod(*client, host, port, use_encryption));
  }

  // Check if should use user-password authentication
  auto it_user = properties.find(FlightSqlConnection::USER);
  if (it_user == properties.end()) {
    // The Microsoft OLE DB to ODBC bridge provider (MSDASQL) will write
    // "User ID" and "Password" properties instead of mapping
    // to ODBC compliant UID/PWD keys.
    it_user = properties.find(FlightSqlConnection::USER_ID);
  }

  auto it_password = properties.find(FlightSqlConnection::PASSWORD);
  auto it_token = properties.find(FlightSqlConnection::TOKEN);

  if (it_user == properties.end() || it_password == properties.end()) {
    // Accept UID/PWD as aliases for User/Password. These are suggested as
    // standard properties in the documentation for SQLDriverConnect.
    it_user = properties.find(FlightSqlConnection::UID);
    it_password = properties.find(FlightSqlConnection::PWD);
  }
  if (it_user != properties.end() || it_password != properties.end()) {
      const std::string &user =
          it_user != properties.end()
              ? it_user->second
              : "";
      const std::string &password =
          it_password != properties.end()
              ? it_password->second
              : "";

    return std::unique_ptr<FlightSqlAuthMethod>(
        new UserPasswordAuthMethod(*client, user, password));
  } else if (it_token != properties.end()) {
    const auto& token = it_token->second;
    return std::unique_ptr<FlightSqlAuthMethod>(new TokenAuthMethod(*client, token));
  }

  throw AuthenticationException(
      "Authentication credentials are required. "
      "Provide user/password, a token, or set authType=external for OAuth.");
}

// --- OAuthAuthMethod implementation ---

OAuthAuthMethod::OAuthAuthMethod(FlightClient &client,
                                 const std::string &host, int port,
                                 bool use_encryption)
    : client_(client), host_(host), port_(port),
      use_encryption_(use_encryption) {}

void OAuthAuthMethod::Authenticate(FlightSqlConnection &connection,
                                   FlightCallOptions &call_options) {
  // Step 1: Discover OAuth endpoint from the server
  std::string oauth_url = Discover(connection, call_options);

  LOG_INFO("OAuth discovery returned URL, launching browser for authentication.");

  // Step 2: Open browser for user to authenticate
  LaunchBrowser(oauth_url);

  // Step 3: Wait for the server to provide a bearer token
  std::string bearer_token = WaitForToken(connection, call_options);

  // Step 4: Set the bearer token on subsequent calls
  const std::pair<std::string, std::string> token_header(
      "authorization", "Bearer " + bearer_token);
  call_options.headers.push_back(token_header);
}

std::string OAuthAuthMethod::Discover(FlightSqlConnection &connection,
                                      FlightCallOptions &call_options) {
  // Send a basic auth handshake with username=__discover__ to trigger
  // the server's OAuth discovery response.
  FlightCallOptions discover_options;

  const boost::optional<Connection::Attribute> &login_timeout =
      connection.GetAttribute(Connection::LOGIN_TIMEOUT);
  if (login_timeout && boost::get<uint32_t>(*login_timeout) > 0) {
    double timeout_seconds =
        static_cast<double>(boost::get<uint32_t>(*login_timeout));
    if (timeout_seconds > 0) {
      discover_options.timeout = TimeoutDuration{timeout_seconds};
    }
  }

  Result<std::pair<std::string, std::string>> bearer_result =
      client_.AuthenticateBasicToken(discover_options, "__discover__", "");

  if (!bearer_result.ok()) {
    const auto &flightStatus =
        arrow::flight::FlightStatusDetail::UnwrapStatus(
            bearer_result.status());
    if (flightStatus != nullptr &&
        flightStatus->code() ==
            arrow::flight::FlightStatusCode::Unauthenticated) {
      // The server should return the OAuth URL in the error detail or
      // extra info field when it sees __discover__.
      std::string extra = flightStatus->extra_info();
      if (!extra.empty()) {
        return extra;
      }
    }
    throw AuthenticationException(
        "OAuth discovery failed: " + bearer_result.status().ToString());
  }

  // If the server responded with a bearer token directly (which contains the
  // OAuth URL), extract it from the authorization header value.
  const auto &header_pair = bearer_result.ValueOrDie();
  return header_pair.second;
}

void OAuthAuthMethod::LaunchBrowser(const std::string &url) {
#if defined(__APPLE__)
  std::string command = "open \"" + url + "\"";
  system(command.c_str());
#elif defined(_WIN32) || defined(_WIN64)
  ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
#else
  std::string command = "xdg-open \"" + url + "\"";
  system(command.c_str());
#endif
}

std::string OAuthAuthMethod::WaitForToken(FlightSqlConnection &connection,
                                          FlightCallOptions &call_options) {
  // Poll the server by re-authenticating with __discover__ until we receive
  // a valid bearer token (the server issues one after the user completes
  // the browser-based OAuth flow).
  const int max_attempts = 120; // 2 minutes at 1-second intervals
  const auto poll_interval = std::chrono::seconds(1);

  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    std::this_thread::sleep_for(poll_interval);

    FlightCallOptions poll_options;
    Result<std::pair<std::string, std::string>> result =
        client_.AuthenticateBasicToken(poll_options, "__discover__", "");

    if (result.ok()) {
      const auto &header_pair = result.ValueOrDie();
      if (!header_pair.second.empty()) {
        LOG_INFO("OAuth token received successfully.");
        return header_pair.second;
      }
    }
  }

  throw AuthenticationException(
      "OAuth authentication timed out waiting for browser login.");
}

} // namespace flight_sql
} // namespace driver
