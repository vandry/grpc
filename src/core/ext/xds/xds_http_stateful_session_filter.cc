//
// Copyright 2022 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <grpc/support/port_platform.h>

#include "src/core/ext/xds/xds_http_stateful_session_filter.h"

#include <stdint.h>

#include <string>
#include <utility>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/variant.h"
#include "envoy/config/core/v3/extension.upb.h"
#include "envoy/extensions/filters/http/stateful_session/v3/stateful_session.upb.h"
#include "envoy/extensions/filters/http/stateful_session/v3/stateful_session.upbdefs.h"
#include "envoy/extensions/http/stateful_session/cookie/v3/cookie.upb.h"
#include "envoy/extensions/http/stateful_session/cookie/v3/cookie.upbdefs.h"
#include "envoy/type/http/v3/cookie.upb.h"
#include "envoy/type/v3/percent.upb.h"
#include "google/protobuf/wrappers.upb.h"
#include "upb/def.h"

#include <grpc/status.h>

#include "src/core/ext/filters/stateful_session/stateful_session_filter.h"
#include "src/core/ext/filters/stateful_session/stateful_session_service_config_parser.h"
#include "src/core/ext/xds/upb_utils.h"
#include "src/core/ext/xds/xds_common_types.h"
#include "src/core/ext/xds/xds_http_filters.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/status_util.h"
#include "src/core/lib/gprpp/time.h"
#include "src/core/lib/gprpp/validation_errors.h"
#include "src/core/lib/json/json.h"
#include "src/core/lib/transport/status_conversion.h"

namespace grpc_core {

absl::string_view XdsHttpStatefulSessionFilter::ConfigProtoName() const {
  return "envoy.extensions.filters.http.stateful_session.v3.StatefulSession";
}

absl::string_view XdsHttpStatefulSessionFilter::OverrideConfigProtoName()
    const {
  return "envoy.extensions.filters.http.stateful_session.v3"
         ".StatefulSessionPerRoute";
}

void XdsHttpStatefulSessionFilter::PopulateSymtab(upb_DefPool* symtab) const {
  envoy_extensions_filters_http_stateful_session_v3_StatefulSession_getmsgdef(
      symtab);
  envoy_extensions_filters_http_stateful_session_v3_StatefulSessionPerRoute_getmsgdef(
      symtab);
  envoy_extensions_http_stateful_session_cookie_v3_CookieBasedSessionState_getmsgdef(
      symtab);
}

namespace {

Json::Object ValidateStatefulSession(
    const XdsResourceType::DecodeContext& context,
    const envoy_extensions_filters_http_stateful_session_v3_StatefulSession*
        stateful_session,
    ValidationErrors* errors) {
  ValidationErrors::ScopedField field(errors, ".session_state");
  const auto* session_state =
      envoy_extensions_filters_http_stateful_session_v3_StatefulSession_session_state(
          stateful_session);
  if (session_state == nullptr) {
    errors->AddError("field not present");
    return {};
  }
  ValidationErrors::ScopedField field2(errors, ".typed_config");
  const auto* typed_config =
      envoy_config_core_v3_TypedExtensionConfig_typed_config(session_state);
  auto extension = ExtractXdsExtension(context, typed_config, errors);
  if (!extension.has_value()) return {};
  if (extension->type !=
      "envoy.extensions.http.stateful_session.cookie.v3"
      ".CookieBasedSessionState") {
    errors->AddError("unsupported session state type");
    return {};
  }
  absl::string_view* serialized_session_state =
      absl::get_if<absl::string_view>(&extension->value);
  if (serialized_session_state == nullptr) {
    errors->AddError("could not parse session state config");
    return {};
  }
  auto* cookie_state =
      envoy_extensions_http_stateful_session_cookie_v3_CookieBasedSessionState_parse(
          serialized_session_state->data(), serialized_session_state->size(),
          context.arena);
  if (cookie_state == nullptr) {
    errors->AddError("could not parse session state config");
    return {};
  }
  ValidationErrors::ScopedField field3(errors, ".cookie");
  const auto* cookie =
      envoy_extensions_http_stateful_session_cookie_v3_CookieBasedSessionState_cookie(
          cookie_state);
  if (cookie == nullptr) {
    errors->AddError("field not present");
    return {};
  }
  Json::Object cookie_config;
  // name
  std::string cookie_name =
      UpbStringToStdString(envoy_type_http_v3_Cookie_name(cookie));
  if (cookie_name.empty()) {
    ValidationErrors::ScopedField field(errors, ".name");
    errors->AddError("field not present");
  }
  cookie_config["name"] = std::move(cookie_name);
  // ttl
  {
    ValidationErrors::ScopedField field(errors, ".ttl");
    const auto* duration = envoy_type_http_v3_Cookie_ttl(cookie);
    if (duration != nullptr) {
      Duration ttl = ParseDuration(duration, errors);
      cookie_config["ttl"] = ttl.ToJsonString();
    }
  }
  // path
  std::string path =
      UpbStringToStdString(envoy_type_http_v3_Cookie_path(cookie));
  if (!path.empty()) cookie_config["path"] = std::move(path);
  return cookie_config;
}

}  // namespace

absl::optional<XdsHttpFilterImpl::FilterConfig>
XdsHttpStatefulSessionFilter::GenerateFilterConfig(
    const XdsResourceType::DecodeContext& context, XdsExtension extension,
    ValidationErrors* errors) const {
  absl::string_view* serialized_filter_config =
      absl::get_if<absl::string_view>(&extension.value);
  if (serialized_filter_config == nullptr) {
    errors->AddError("could not parse stateful session filter config");
    return absl::nullopt;
  }
  auto* stateful_session =
      envoy_extensions_filters_http_stateful_session_v3_StatefulSession_parse(
          serialized_filter_config->data(), serialized_filter_config->size(),
          context.arena);
  if (stateful_session == nullptr) {
    errors->AddError("could not parse stateful session filter config");
    return absl::nullopt;
  }
  return FilterConfig{
      ConfigProtoName(),
      ValidateStatefulSession(context, stateful_session, errors)};
}

absl::optional<XdsHttpFilterImpl::FilterConfig>
XdsHttpStatefulSessionFilter::GenerateFilterConfigOverride(
    const XdsResourceType::DecodeContext& context, XdsExtension extension,
    ValidationErrors* errors) const {
  absl::string_view* serialized_filter_config =
      absl::get_if<absl::string_view>(&extension.value);
  if (serialized_filter_config == nullptr) {
    errors->AddError("could not parse stateful session filter override config");
    return absl::nullopt;
  }
  auto* stateful_session_per_route =
      envoy_extensions_filters_http_stateful_session_v3_StatefulSessionPerRoute_parse(
          serialized_filter_config->data(), serialized_filter_config->size(),
          context.arena);
  if (stateful_session_per_route == nullptr) {
    errors->AddError("could not parse stateful session filter override config");
    return absl::nullopt;
  }
  Json::Object config;
  if (!envoy_extensions_filters_http_stateful_session_v3_StatefulSessionPerRoute_disabled(
          stateful_session_per_route)) {
    ValidationErrors::ScopedField field(errors, ".stateful_session");
    const auto* stateful_session =
        envoy_extensions_filters_http_stateful_session_v3_StatefulSessionPerRoute_stateful_session(
            stateful_session_per_route);
    if (stateful_session == nullptr) {
      errors->AddError("field not present");
    } else {
      config = ValidateStatefulSession(context, stateful_session, errors);
    }
  }
  return FilterConfig{OverrideConfigProtoName(), Json(std::move(config))};
}

const grpc_channel_filter* XdsHttpStatefulSessionFilter::channel_filter()
    const {
  return &StatefulSessionFilter::kFilter;
}

ChannelArgs XdsHttpStatefulSessionFilter::ModifyChannelArgs(
    const ChannelArgs& args) const {
  return args.Set(GRPC_ARG_PARSE_STATEFUL_SESSION_METHOD_CONFIG, 1);
}

absl::StatusOr<XdsHttpFilterImpl::ServiceConfigJsonEntry>
XdsHttpStatefulSessionFilter::GenerateServiceConfig(
    const FilterConfig& hcm_filter_config,
    const FilterConfig* filter_config_override) const {
  Json config = filter_config_override != nullptr
                    ? filter_config_override->config
                    : hcm_filter_config.config;
  return ServiceConfigJsonEntry{"stateful_session", config.Dump()};
}

}  // namespace grpc_core
