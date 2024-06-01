/*
 * Copyright 2020 Redpanda Data, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#pragma once

#include "base/seastarx.h"

#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>
#include <seastar/net/tls.hh>

#include <yaml-cpp/yaml.h>

#include <optional>

namespace config {

struct key_cert {
    ss::sstring key_file;
    ss::sstring cert_file;

    bool operator==(const key_cert& rhs) const {
        return key_file == rhs.key_file && cert_file == rhs.cert_file;
    }

    friend std::ostream& operator<<(std::ostream& o, const key_cert& c);
};

class tls_config {
public:
    tls_config()
      : _key_cert(std::nullopt)
      , _truststore_file(std::nullopt)
      , _crl_file(std::nullopt) {}

    tls_config(
      bool enabled,
      std::optional<key_cert> key_cert,
      std::optional<ss::sstring> truststore,
      std::optional<ss::sstring> crl,
      bool require_client_auth)
      : _enabled(enabled)
      , _key_cert(std::move(key_cert))
      , _truststore_file(std::move(truststore))
      , _crl_file(std::move(crl))
      , _require_client_auth(require_client_auth) {}

    bool is_enabled() const { return _enabled; }

    const std::optional<key_cert>& get_key_cert_files() const {
        return _key_cert;
    }

    const std::optional<ss::sstring>& get_truststore_file() const {
        return _truststore_file;
    }

    const std::optional<ss::sstring>& get_crl_file() const { return _crl_file; }

    bool get_require_client_auth() const { return _require_client_auth; }

    ss::future<std::optional<ss::tls::credentials_builder>>

    get_credentials_builder() const&;

    ss::future<std::optional<ss::tls::credentials_builder>>
    get_credentials_builder() &&;

    static std::optional<ss::sstring> validate(const tls_config& c);

    bool operator==(const tls_config& rhs) const = default;

    friend std::ostream&
    operator<<(std::ostream& o, const config::tls_config& c);

private:
    bool _enabled{false};
    std::optional<key_cert> _key_cert;
    std::optional<ss::sstring> _truststore_file;
    std::optional<ss::sstring> _crl_file;
    bool _require_client_auth{false};
};

} // namespace config

namespace YAML {

template<>
struct convert<config::tls_config> {
    static Node encode(const config::tls_config& rhs);

    static std::optional<ss::sstring>
    read_optional(const Node& node, const ss::sstring& key);

    static bool decode(const Node& node, config::tls_config& rhs);
};

} // namespace YAML
