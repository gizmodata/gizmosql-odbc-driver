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
#include <odbcabstraction/utils.h>

#include <arrow/flight/client.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

// RapidJSON for parsing server responses
#include <rapidjson/document.h>

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <cstdlib>
// Boost.Beast / Asio for HTTP client on non-Windows
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#else
#include <cstdlib>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
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

#if !defined(_WIN32) && !defined(_WIN64)
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;
#endif

// ── OAuthDiscoveryMiddleware ──────────────────────────────────────────────────

OAuthDiscoveryMiddleware::OAuthDiscoveryMiddleware(
    OAuthDiscoveryMiddlewareFactory *factory)
    : factory_(factory) {}

void OAuthDiscoveryMiddleware::SendingHeaders(
    arrow::flight::AddCallHeaders * /*outgoing_headers*/) {}

void OAuthDiscoveryMiddleware::ReceivedHeaders(
    const arrow::flight::CallHeaders &incoming_headers) {
  auto it = incoming_headers.find("x-gizmosql-oauth-url");
  if (it != incoming_headers.end()) {
    factory_->SetDiscoveredUrl(std::string(it->second));
  }
}

void OAuthDiscoveryMiddleware::CallCompleted(const arrow::Status & /*status*/) {}

// ── OAuthDiscoveryMiddlewareFactory ──────────────────────────────────────────

void OAuthDiscoveryMiddlewareFactory::StartCall(
    const arrow::flight::CallInfo & /*info*/,
    std::unique_ptr<arrow::flight::ClientMiddleware> *middleware) {
  *middleware = std::unique_ptr<arrow::flight::ClientMiddleware>(
      new OAuthDiscoveryMiddleware(this));
}

std::string OAuthDiscoveryMiddlewareFactory::GetDiscoveredUrl() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return discovered_url_;
}

void OAuthDiscoveryMiddlewareFactory::SetDiscoveredUrl(
    const std::string &url) {
  std::lock_guard<std::mutex> lock(mutex_);
  discovered_url_ = url;
}

// ── Internal auth methods ────────────────────────────────────────────────────

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

/// Parse a URL string into scheme, host, port, and path components.
/// Supports http:// and https:// URLs.
struct ParsedUrl {
  std::string scheme;  // "http" or "https"
  std::string host;
  std::string port;    // string for tcp resolver
  std::string path;    // e.g., "/oauth/initiate"
};

ParsedUrl ParseUrl(const std::string &url) {
  ParsedUrl result;
  size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    throw AuthenticationException("Invalid URL (missing scheme): " + url);
  }
  result.scheme = url.substr(0, scheme_end);

  size_t host_start = scheme_end + 3;
  size_t path_start = url.find('/', host_start);
  std::string authority;
  if (path_start == std::string::npos) {
    authority = url.substr(host_start);
    result.path = "/";
  } else {
    authority = url.substr(host_start, path_start - host_start);
    result.path = url.substr(path_start);
  }

  size_t colon = authority.find(':');
  if (colon != std::string::npos) {
    result.host = authority.substr(0, colon);
    result.port = authority.substr(colon + 1);
  } else {
    result.host = authority;
    result.port = (result.scheme == "https") ? "443" : "80";
  }

  return result;
}

} // namespace

// ── FromProperties ───────────────────────────────────────────────────────────

std::unique_ptr<FlightSqlAuthMethod> FlightSqlAuthMethod::FromProperties(
    const std::unique_ptr<FlightClient> &client,
    const Connection::ConnPropertyMap &properties,
    std::shared_ptr<OAuthDiscoveryMiddlewareFactory> oauth_discovery) {

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

    // OAuth server port (default 31339)
    int oauth_server_port = 31339;
    auto it_oauth_port = properties.find(FlightSqlConnection::OAUTH_SERVER_PORT);
    if (it_oauth_port != properties.end()) {
      oauth_server_port = std::stoi(it_oauth_port->second);
    }

    // Disable certificate verification (default false)
    bool disable_cert_verification = false;
    auto it_disable_cert = properties.find(FlightSqlConnection::DISABLE_CERTIFICATE_VERIFICATION);
    if (it_disable_cert != properties.end()) {
      auto val = odbcabstraction::AsBool(it_disable_cert->second);
      if (val) {
        disable_cert_verification = *val;
      }
    }

    return std::unique_ptr<FlightSqlAuthMethod>(
        new OAuthAuthMethod(*client, host, port, use_encryption,
                            oauth_server_port, disable_cert_verification,
                            oauth_discovery));
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

// ── OAuthAuthMethod implementation ──────────────────────────────────────────

// File-based OAuth bearer token cache.
// Persists tokens across processes (Power BI Mashup containers) so that
// only the first connection triggers the browser-based OAuth flow.

static std::string GetCacheFilePath(const std::string &host, int port) {
  std::string temp_dir;
#if defined(_WIN32) || defined(_WIN64)
  const char *temp = std::getenv("TEMP");
  if (!temp) temp = std::getenv("TMP");
  if (!temp) temp = "C:\\Temp";
  temp_dir = temp;
#else
  temp_dir = "/tmp";
#endif
  return temp_dir + "/gizmosql-oauth-" + host + "-" + std::to_string(port) + ".token";
}

static bool ReadCachedToken(const std::string &host, int port,
                            std::pair<std::string, std::string> &token) {
  std::ifstream file(GetCacheFilePath(host, port));
  if (!file.is_open()) return false;

  std::string header_name, header_value;
  if (!std::getline(file, header_name) || !std::getline(file, header_value)) {
    return false;
  }
  if (header_name.empty() || header_value.empty()) return false;

  token = {header_name, header_value};
  return true;
}

static void WriteCachedToken(const std::string &host, int port,
                             const std::pair<std::string, std::string> &token) {
  std::ofstream file(GetCacheFilePath(host, port), std::ios::trunc);
  if (file.is_open()) {
    file << token.first << "\n" << token.second << "\n";
  }
}

static void RemoveCachedToken(const std::string &host, int port) {
  std::remove(GetCacheFilePath(host, port).c_str());
}

OAuthAuthMethod::OAuthAuthMethod(
    FlightClient &client, const std::string &host, int port,
    bool use_encryption, int oauth_server_port,
    bool disable_cert_verification,
    std::shared_ptr<OAuthDiscoveryMiddlewareFactory> discovery_factory)
    : client_(client), host_(host), port_(port),
      use_encryption_(use_encryption),
      oauth_server_port_(oauth_server_port),
      disable_cert_verification_(disable_cert_verification),
      discovery_factory_(std::move(discovery_factory)) {}

void OAuthAuthMethod::Authenticate(FlightSqlConnection &connection,
                                   FlightCallOptions &call_options) {
  // Try file-cached bearer token first — survives across Power BI
  // Mashup container processes so only the first connection triggers
  // the browser-based OAuth flow.
  std::pair<std::string, std::string> cached_token;
  if (ReadCachedToken(host_, port_, cached_token)) {
    // Validate the cached token with a lightweight RPC before trusting it.
    // If the token has expired or its signing key rotated, clear the cache
    // and fall through to the full OAuth flow instead of returning a stale token.
    FlightCallOptions probe_options;
    probe_options.headers.push_back(cached_token);
    arrow::Result<std::vector<arrow::flight::ActionType>> probe_result =
        client_.ListActions(probe_options);
    if (probe_result.ok()) {
      call_options.headers.push_back(cached_token);
      LOG_INFO("Cached OAuth bearer token validated for " + host_ + ":" +
               std::to_string(port_));
      return;
    }
    // Token is stale — clear cache and proceed with fresh OAuth flow
    LOG_INFO("Cached OAuth bearer token invalid for " + host_ + ":" +
             std::to_string(port_) + ": " + probe_result.status().ToString());
    RemoveCachedToken(host_, port_);
  }

  // Full OAuth flow (3 phases)
  std::string oauth_base_url = DiscoverOAuthUrl(connection);
  LOG_INFO("OAuth discovery returned base URL: " + oauth_base_url);

  std::string access_token = PerformHttpOAuthFlow(oauth_base_url);
  LOG_INFO("OAuth access token received from HTTP flow.");

  ExchangeToken(access_token, call_options);
  LOG_INFO("OAuth bearer token exchanged successfully.");

  // Cache the new bearer token to file for cross-process reuse
  WriteCachedToken(host_, port_, call_options.headers.back());
}

std::string OAuthAuthMethod::DiscoverOAuthUrl(
    FlightSqlConnection &connection) {
  // Send AuthenticateBasicToken("__discover__", "") to trigger the server
  // to send back the x-gizmosql-oauth-url response header.
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

  // The call may succeed or fail — either way, the middleware captures the header.
  if (!bearer_result.ok()) {
    std::string error_msg = bearer_result.status().ToString();
    // If the server says OAuth is not enabled, hard fail
    if (error_msg.find("OAuth is not enabled") != std::string::npos) {
      throw AuthenticationException(
          "OAuth is not enabled on the server. " + error_msg);
    }
  }

  // Check if the middleware captured the x-gizmosql-oauth-url header
  if (discovery_factory_) {
    std::string discovered = discovery_factory_->GetDiscoveredUrl();
    if (!discovered.empty()) {
      return discovered;
    }
  }

  // Fallback: construct the OAuth base URL from host and oauthServerPort
  std::string scheme = use_encryption_ ? "https" : "http";
  return scheme + "://" + host_ + ":" + std::to_string(oauth_server_port_);
}

std::string OAuthAuthMethod::PerformHttpOAuthFlow(
    const std::string &oauth_base_url) {
  // Step 1: GET {base}/oauth/initiate
  std::string initiate_url = oauth_base_url + "/oauth/initiate";
  std::string initiate_response = HttpGet(initiate_url);

  rapidjson::Document initiate_doc;
  initiate_doc.Parse(initiate_response.c_str());
  if (initiate_doc.HasParseError() ||
      !initiate_doc.HasMember("session_uuid") ||
      !initiate_doc.HasMember("auth_url")) {
    throw AuthenticationException(
        "Invalid response from OAuth initiate endpoint: " + initiate_response);
  }

  std::string session_uuid = initiate_doc["session_uuid"].GetString();
  std::string auth_url = initiate_doc["auth_url"].GetString();

  // Step 2: Open browser for user authentication
  LOG_INFO("Opening browser for OAuth authentication.");
  LaunchBrowser(auth_url);

  // Step 3: Poll GET {base}/oauth/token/{uuid} until complete or error
  std::string token_url =
      oauth_base_url + "/oauth/token/" + session_uuid;
  const int max_attempts = 300; // 5 minutes at 1-second intervals
  const auto poll_interval = std::chrono::seconds(1);

  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    std::this_thread::sleep_for(poll_interval);

    std::string token_response;
    try {
      token_response = HttpGet(token_url);
    } catch (const std::exception &e) {
      // Transient HTTP errors — keep polling
      LOG_INFO("OAuth token poll attempt " + std::to_string(attempt + 1) +
               " failed: " + e.what());
      continue;
    }

    rapidjson::Document token_doc;
    token_doc.Parse(token_response.c_str());
    if (token_doc.HasParseError() || !token_doc.HasMember("status")) {
      continue; // Malformed response — keep polling
    }

    std::string status = token_doc["status"].GetString();

    if (status == "complete") {
      if (!token_doc.HasMember("token")) {
        throw AuthenticationException(
            "OAuth token response has status=complete but no token field.");
      }
      return token_doc["token"].GetString();
    } else if (status == "error") {
      std::string error = token_doc.HasMember("error")
                              ? token_doc["error"].GetString()
                              : "unknown error";
      throw AuthenticationException("OAuth authentication failed: " + error);
    }
    // status == "pending" → continue polling
  }

  throw AuthenticationException(
      "OAuth authentication timed out waiting for browser login (5 minutes).");
}

// ── Platform-specific HTTP GET implementation ────────────────────────────────

#if defined(_WIN32) || defined(_WIN64)

// Windows: Dynamically load WinHTTP to avoid header conflicts with the
// vcpkg/cmake build environment. WinHTTP uses the Windows certificate store
// natively and avoids OpenSSL conflicts with the statically-linked copy
// used by gRPC/Arrow in this DLL.

// WinHTTP types (from winhttp.h) needed for dynamic loading
typedef LPVOID HINTERNET;
typedef WORD INTERNET_PORT;

// WinHTTP constants (from winhttp.h) needed for dynamic loading
#ifndef WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
#define WINHTTP_ACCESS_TYPE_DEFAULT_PROXY 0
#endif
#ifndef WINHTTP_FLAG_SECURE
#define WINHTTP_FLAG_SECURE 0x00800000
#endif
#ifndef WINHTTP_QUERY_STATUS_CODE
#define WINHTTP_QUERY_STATUS_CODE 19
#endif
#ifndef WINHTTP_QUERY_FLAG_NUMBER
#define WINHTTP_QUERY_FLAG_NUMBER 0x20000000
#endif
#ifndef WINHTTP_OPTION_SECURITY_FLAGS
#define WINHTTP_OPTION_SECURITY_FLAGS 31
#endif
#ifndef SECURITY_FLAG_IGNORE_UNKNOWN_CA
#define SECURITY_FLAG_IGNORE_UNKNOWN_CA 0x00000100
#endif
#ifndef SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE
#define SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE 0x00000200
#endif
#ifndef SECURITY_FLAG_IGNORE_CERT_CN_INVALID
#define SECURITY_FLAG_IGNORE_CERT_CN_INVALID 0x00001000
#endif
#ifndef SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
#define SECURITY_FLAG_IGNORE_CERT_DATE_INVALID 0x00002000
#endif
#ifndef WINHTTP_HEADER_NAME_BY_INDEX
#define WINHTTP_HEADER_NAME_BY_INDEX NULL
#endif
#ifndef WINHTTP_NO_HEADER_INDEX
#define WINHTTP_NO_HEADER_INDEX NULL
#endif

// WinHTTP function pointer types
using PFN_WinHttpOpen = HINTERNET(WINAPI *)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
using PFN_WinHttpConnect = HINTERNET(WINAPI *)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
using PFN_WinHttpOpenRequest = HINTERNET(WINAPI *)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
using PFN_WinHttpSetTimeouts = BOOL(WINAPI *)(HINTERNET, int, int, int, int);
using PFN_WinHttpSetOption = BOOL(WINAPI *)(HINTERNET, DWORD, LPVOID, DWORD);
using PFN_WinHttpSendRequest = BOOL(WINAPI *)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
using PFN_WinHttpReceiveResponse = BOOL(WINAPI *)(HINTERNET, LPVOID);
using PFN_WinHttpQueryHeaders = BOOL(WINAPI *)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
using PFN_WinHttpQueryDataAvailable = BOOL(WINAPI *)(HINTERNET, LPDWORD);
using PFN_WinHttpReadData = BOOL(WINAPI *)(HINTERNET, LPVOID, DWORD, LPDWORD);
using PFN_WinHttpCloseHandle = BOOL(WINAPI *)(HINTERNET);

std::string OAuthAuthMethod::HttpGet(const std::string &url) {
  // Dynamically load winhttp.dll
  HMODULE hWinHttp = LoadLibraryA("winhttp.dll");
  if (!hWinHttp) {
    throw AuthenticationException(
        "Failed to load winhttp.dll (error " +
        std::to_string(GetLastError()) + ")");
  }

  auto pWinHttpOpen = (PFN_WinHttpOpen)GetProcAddress(hWinHttp, "WinHttpOpen");
  auto pWinHttpConnect = (PFN_WinHttpConnect)GetProcAddress(hWinHttp, "WinHttpConnect");
  auto pWinHttpOpenRequest = (PFN_WinHttpOpenRequest)GetProcAddress(hWinHttp, "WinHttpOpenRequest");
  auto pWinHttpSetTimeouts = (PFN_WinHttpSetTimeouts)GetProcAddress(hWinHttp, "WinHttpSetTimeouts");
  auto pWinHttpSetOption = (PFN_WinHttpSetOption)GetProcAddress(hWinHttp, "WinHttpSetOption");
  auto pWinHttpSendRequest = (PFN_WinHttpSendRequest)GetProcAddress(hWinHttp, "WinHttpSendRequest");
  auto pWinHttpReceiveResponse = (PFN_WinHttpReceiveResponse)GetProcAddress(hWinHttp, "WinHttpReceiveResponse");
  auto pWinHttpQueryHeaders = (PFN_WinHttpQueryHeaders)GetProcAddress(hWinHttp, "WinHttpQueryHeaders");
  auto pWinHttpQueryDataAvailable = (PFN_WinHttpQueryDataAvailable)GetProcAddress(hWinHttp, "WinHttpQueryDataAvailable");
  auto pWinHttpReadData = (PFN_WinHttpReadData)GetProcAddress(hWinHttp, "WinHttpReadData");
  auto pWinHttpCloseHandle = (PFN_WinHttpCloseHandle)GetProcAddress(hWinHttp, "WinHttpCloseHandle");

  if (!pWinHttpOpen || !pWinHttpConnect || !pWinHttpOpenRequest ||
      !pWinHttpSendRequest || !pWinHttpReceiveResponse ||
      !pWinHttpQueryHeaders || !pWinHttpQueryDataAvailable ||
      !pWinHttpReadData || !pWinHttpCloseHandle) {
    FreeLibrary(hWinHttp);
    throw AuthenticationException("Failed to resolve WinHTTP function pointers");
  }

  ParsedUrl parsed = ParseUrl(url);
  bool is_https = (parsed.scheme == "https");
  int port = std::stoi(parsed.port);

  std::wstring whost(parsed.host.begin(), parsed.host.end());
  std::wstring wpath(parsed.path.begin(), parsed.path.end());

  HINTERNET hSession = pWinHttpOpen(
      L"GizmoSQL-ODBC/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      NULL, NULL, 0);
  if (!hSession) {
    DWORD err = GetLastError();
    FreeLibrary(hWinHttp);
    throw AuthenticationException(
        "WinHttpOpen failed (error " + std::to_string(err) + ")");
  }

  HINTERNET hConnect = pWinHttpConnect(hSession, whost.c_str(),
                                        static_cast<INTERNET_PORT>(port), 0);
  if (!hConnect) {
    DWORD err = GetLastError();
    pWinHttpCloseHandle(hSession);
    FreeLibrary(hWinHttp);
    throw AuthenticationException(
        "WinHttpConnect to " + parsed.host + ":" + parsed.port +
        " failed (error " + std::to_string(err) + ")");
  }

  DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET hRequest = pWinHttpOpenRequest(
      hConnect, L"GET", wpath.c_str(), NULL, NULL, NULL, flags);
  if (!hRequest) {
    DWORD err = GetLastError();
    pWinHttpCloseHandle(hConnect);
    pWinHttpCloseHandle(hSession);
    FreeLibrary(hWinHttp);
    throw AuthenticationException(
        "WinHttpOpenRequest failed (error " + std::to_string(err) + ")");
  }

  if (pWinHttpSetTimeouts) {
    pWinHttpSetTimeouts(hRequest, 10000, 10000, 10000, 10000);
  }

  if (is_https && disable_cert_verification_ && pWinHttpSetOption) {
    DWORD security_flags =
        SECURITY_FLAG_IGNORE_UNKNOWN_CA |
        SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE |
        SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
        SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
    pWinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                      &security_flags, sizeof(security_flags));
  }

  BOOL bResult = pWinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0);
  if (!bResult) {
    DWORD err = GetLastError();
    pWinHttpCloseHandle(hRequest);
    pWinHttpCloseHandle(hConnect);
    pWinHttpCloseHandle(hSession);
    FreeLibrary(hWinHttp);
    throw AuthenticationException(
        "WinHttpSendRequest to " + url + " failed (error " +
        std::to_string(err) + ")");
  }

  bResult = pWinHttpReceiveResponse(hRequest, NULL);
  if (!bResult) {
    DWORD err = GetLastError();
    pWinHttpCloseHandle(hRequest);
    pWinHttpCloseHandle(hConnect);
    pWinHttpCloseHandle(hSession);
    FreeLibrary(hWinHttp);
    throw AuthenticationException(
        "WinHttpReceiveResponse from " + url + " failed (error " +
        std::to_string(err) + ")");
  }

  DWORD status_code = 0;
  DWORD status_size = sizeof(status_code);
  pWinHttpQueryHeaders(hRequest,
                       WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                       (LPCWSTR)WINHTTP_HEADER_NAME_BY_INDEX,
                       &status_code, &status_size,
                       (LPDWORD)WINHTTP_NO_HEADER_INDEX);
  if (status_code != 200) {
    pWinHttpCloseHandle(hRequest);
    pWinHttpCloseHandle(hConnect);
    pWinHttpCloseHandle(hSession);
    FreeLibrary(hWinHttp);
    throw AuthenticationException(
        "HTTP GET " + url + " returned status " + std::to_string(status_code));
  }

  std::string body;
  DWORD bytes_available = 0;
  do {
    bytes_available = 0;
    pWinHttpQueryDataAvailable(hRequest, &bytes_available);
    if (bytes_available > 0) {
      std::vector<char> buf(bytes_available);
      DWORD bytes_read = 0;
      pWinHttpReadData(hRequest, buf.data(), bytes_available, &bytes_read);
      body.append(buf.data(), bytes_read);
    }
  } while (bytes_available > 0);

  pWinHttpCloseHandle(hRequest);
  pWinHttpCloseHandle(hConnect);
  pWinHttpCloseHandle(hSession);
  FreeLibrary(hWinHttp);

  return body;
}

#else

// macOS / Linux: Use Boost.Beast HTTP client
std::string OAuthAuthMethod::HttpGet(const std::string &url) {
  ParsedUrl parsed = ParseUrl(url);
  bool is_https = (parsed.scheme == "https");

  net::io_context ioc;
  tcp::resolver resolver(ioc);
  auto const results = resolver.resolve(parsed.host, parsed.port);

  if (is_https) {
    ssl::context ctx(ssl::context::tlsv12_client);
    if (disable_cert_verification_) {
      ctx.set_verify_mode(ssl::verify_none);
    } else {
      ctx.set_default_verify_paths();
      ctx.set_verify_mode(ssl::verify_peer);
    }

    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

    // Set SNI hostname
    if (!SSL_set_tlsext_host_name(stream.native_handle(),
                                   parsed.host.c_str())) {
      beast::error_code ec{static_cast<int>(::ERR_get_error()),
                           net::error::get_ssl_category()};
      throw beast::system_error{ec};
    }

    beast::get_lowest_layer(stream).connect(results);
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
    stream.handshake(ssl::stream_base::client);

    http::request<http::empty_body> req{http::verb::get, parsed.path, 11};
    req.set(http::field::host, parsed.host);
    req.set(http::field::user_agent, "GizmoSQL-ODBC/1.0");

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    if (res.result() != http::status::ok) {
      throw AuthenticationException(
          "HTTP GET " + url + " returned status " +
          std::to_string(static_cast<int>(res.result())));
    }

    // Graceful SSL shutdown (ignore errors — server may close first)
    beast::error_code ec;
    stream.shutdown(ec);

    return res.body();
  } else {
    // Plain HTTP
    beast::tcp_stream stream(ioc);
    stream.connect(results);
    stream.expires_after(std::chrono::seconds(10));

    http::request<http::empty_body> req{http::verb::get, parsed.path, 11};
    req.set(http::field::host, parsed.host);
    req.set(http::field::user_agent, "GizmoSQL-ODBC/1.0");

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    if (res.result() != http::status::ok) {
      throw AuthenticationException(
          "HTTP GET " + url + " returned status " +
          std::to_string(static_cast<int>(res.result())));
    }

    // Graceful shutdown
    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    return res.body();
  }
}

#endif

void OAuthAuthMethod::ExchangeToken(
    const std::string &access_token,
    FlightCallOptions &call_options) {
  // Exchange the OAuth access token via Flight Handshake:
  // username="token", password=<access_token>
  Result<std::pair<std::string, std::string>> bearer_result =
      client_.AuthenticateBasicToken({}, "token", access_token);

  if (!bearer_result.ok()) {
    throw AuthenticationException(
        "Failed to exchange OAuth token via Flight Handshake: " +
        bearer_result.status().ToString());
  }

  // Push the resulting bearer header (authorization: Bearer ...) into call_options
  call_options.headers.push_back(bearer_result.ValueOrDie());
}

void OAuthAuthMethod::ClearCachedToken(const std::string &host, int port) {
  RemoveCachedToken(host, port);
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

} // namespace flight_sql
} // namespace driver
