/*
 * Copyright 2024 Redpanda Data, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#include "debug_bundle_service.h"

#include "bytes/iostream.h"
#include "config/configuration.h"
#include "config/node_config.h"
#include "container/fragmented_vector.h"
#include "crypto/crypto.h"
#include "crypto/types.h"
#include "debug_bundle/error.h"
#include "debug_bundle/metadata.h"
#include "debug_bundle/types.h"
#include "debug_bundle/utils.h"
#include "ssx/future-util.h"
#include "storage/kvstore.h"
#include "utils/external_process.h"

#include <seastar/core/fstream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/shard_id.hh>
#include <seastar/core/sstring.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/process.hh>
#include <seastar/util/variant_utils.hh>

#include <boost/algorithm/string/join.hpp>
#include <fmt/format.h>
#include <re2/re2.h>

#include <variant>

using namespace std::chrono_literals;

namespace debug_bundle {
static ss::logger lg{"debug-bundle-service"};

namespace {
constexpr std::string_view output_variable = "--output";
constexpr std::string_view verbose_variable = "--verbose";
constexpr std::string_view username_variable = "-Xuser";
constexpr std::string_view password_variable = "-Xpass";
constexpr std::string_view sasl_mechanism_variable = "-Xsasl.mechanism";
constexpr std::string_view controller_logs_size_limit_variable
  = "--controller-logs-size-limit";
constexpr std::string_view cpu_profiler_wait_variable = "--cpu-profiler-wait";
constexpr std::string_view logs_since_variable = "--logs-since";
constexpr std::string_view logs_size_limit_variable = "--logs-size-limit";
constexpr std::string_view logs_until_variable = "--logs-until";
constexpr std::string_view metrics_interval_variable = "--metrics-interval";
constexpr std::string_view partition_variable = "--partition";
constexpr std::string_view tls_enabled_variable = "-Xtls.enabled";
constexpr std::string_view tls_insecure_skip_verify_variable
  = "-Xtls.insecure_skip_verify";
constexpr std::string_view k8s_namespace_variable = "--namespace";

bool contains_sensitive_info(const ss::sstring& arg) {
    if (arg.find(password_variable) != ss::sstring::npos) {
        return true;
    }
    return false;
}
void print_arguments(const std::vector<ss::sstring>& args) {
    auto msg = boost::algorithm::join_if(args, " ", [](const ss::sstring& arg) {
        return !contains_sensitive_info(arg);
    });
    vlog(lg.debug, "Starting RPK debug bundle: {}", msg);
}

std::string form_debug_bundle_file_name(job_id_t job_id) {
    return fmt::format("{}.zip", job_id);
}

std::string form_process_output_file_name(job_id_t job_id) {
    return fmt::format("{}.out", job_id);
}

std::filesystem::path form_debug_bundle_file_path(
  const std::filesystem::path& base_path, job_id_t job_id) {
    return base_path / form_debug_bundle_file_name(job_id);
}

std::filesystem::path form_process_output_file_path(
  const std::filesystem::path& base_path, job_id_t job_id) {
    return base_path / form_process_output_file_name(job_id);
}

bool is_valid_rfc1123(std::string_view ns) {
    // Regular expression for RFC1123 hostname validation
    constexpr std::string_view rfc1123_pattern
      = R"(^([a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?))";

    // Validate the hostname against the regular expression using RE2
    return RE2::FullMatch(ns, RE2(rfc1123_pattern));
}

bool is_valid_k8s_namespace(std::string_view ns) {
    constexpr auto max_ns_length = 63;
    return !ns.empty() && ns.size() <= max_ns_length && is_valid_rfc1123(ns);
}

std::filesystem::path form_debug_bundle_storage_directory() {
    const auto& debug_bundle_dir
      = config::shard_local_cfg().debug_bundle_storage_dir();

    // Either return the storage directory or the data directory appended with
    // "debug-bundle"
    return debug_bundle_dir.value_or(
      config::node().data_directory.value().path
      / service::debug_bundle_dir_name);
}

ss::future<> write_file(std::string_view path, iobuf buf) {
    auto file = co_await ss::open_file_dma(
      path, ss::open_flags::create | ss::open_flags::rw);
    auto h = ss::defer([file]() mutable { ssx::background = file.close(); });
    auto istrm = make_iobuf_input_stream(std::move(buf));
    auto ostrm = co_await ss::make_file_output_stream(file);
    co_await ss::copy(istrm, ostrm);
    co_await ostrm.flush();
}

bool was_run_successful(ss::experimental::process::wait_status wait_status) {
    auto* exited = std::get_if<ss::experimental::process::wait_exited>(
      &wait_status);
    return exited != nullptr && exited->exit_code == 0;
}
} // namespace

struct service::output_handler {
    using consumption_result_type =
      typename ss::input_stream<char>::consumption_result_type;
    using stop_consuming_type =
      typename consumption_result_type::stop_consuming_type;
    using tmp_buf = stop_consuming_type::tmp_buf;

    chunked_vector<ss::sstring>& output_buffer;

    ss::future<consumption_result_type> operator()(tmp_buf buf) {
        output_buffer.emplace_back(ss::sstring{buf.begin(), buf.end()});
        co_return ss::continue_consuming{};
    }
};

class service::debug_bundle_process {
public:
    debug_bundle_process(
      job_id_t job_id,
      std::unique_ptr<external_process::external_process> rpk_process,
      std::filesystem::path output_file_path,
      std::filesystem::path process_output_file_path)
      : _job_id(job_id)
      , _rpk_process(std::move(rpk_process))
      , _output_file_path(std::move(output_file_path))
      , _process_output_file_path(std::move(process_output_file_path))
      , _created_time(clock::now()) {
        _rpk_process->set_stdout_consumer(
          output_handler{.output_buffer = _cout});
        _rpk_process->set_stderr_consumer(
          output_handler{.output_buffer = _cerr});
    }

    debug_bundle_process() = delete;
    debug_bundle_process(debug_bundle_process&&) = default;
    debug_bundle_process& operator=(debug_bundle_process&&) = default;
    debug_bundle_process(const debug_bundle_process&) = delete;
    debug_bundle_process& operator=(const debug_bundle_process&) = delete;

    ~debug_bundle_process() {
        if (_rpk_process) {
            vassert(
              !_rpk_process->is_running(),
              "Destroying process struct without waiting for process to "
              "finish");
        }
    }

    ss::future<> terminate(std::chrono::milliseconds timeout) {
        if (_rpk_process) {
            return _rpk_process->terminate(timeout);
        }
        return ss::now();
    }

    ss::future<ss::experimental::process::wait_status> wait() {
        vassert(
          _rpk_process != nullptr,
          "RPK process should be created if calling wait()");
        try {
            co_return _wait_result.emplace(co_await _rpk_process->wait());
        } catch (const std::exception& e) {
            _wait_result.emplace(ss::experimental::process::wait_exited{1});
            co_return ss::coroutine::return_exception(e);
        }
    }

    debug_bundle_status process_status() const {
        if (_wait_result.has_value()) {
            return ss::visit(
              _wait_result.value(),
              [](ss::experimental::process::wait_exited e) {
                  if (e.exit_code == 0) {
                      return debug_bundle_status::success;
                  } else {
                      return debug_bundle_status::error;
                  }
              },
              [](ss::experimental::process::wait_signaled) {
                  return debug_bundle_status::error;
              });
        }
        return debug_bundle_status::running;
    }

    job_id_t job_id() const { return _job_id; }
    const std::filesystem::path& output_file_path() const {
        return _output_file_path;
    }
    const std::filesystem::path& process_output_file_path() const {
        return _process_output_file_path;
    }
    const chunked_vector<ss::sstring>& cout() const { return _cout; }
    const chunked_vector<ss::sstring>& cerr() const { return _cerr; }
    clock::time_point created_time() const { return _created_time; }
    ss::experimental::process::wait_status get_wait_result() const {
        vassert(_wait_result.has_value(), "wait_result must have been set");
        return _wait_result.value();
    }

private:
    job_id_t _job_id;
    std::unique_ptr<external_process::external_process> _rpk_process;
    std::optional<ss::experimental::process::wait_status> _wait_result;
    std::filesystem::path _output_file_path;
    std::filesystem::path _process_output_file_path;
    chunked_vector<ss::sstring> _cout;
    chunked_vector<ss::sstring> _cerr;
    clock::time_point _created_time;
};

service::service(storage::kvstore* kvstore)
  : _kvstore(kvstore)
  , _debug_bundle_dir(form_debug_bundle_storage_directory())
  , _debug_bundle_storage_dir_binding(
      config::shard_local_cfg().debug_bundle_storage_dir.bind())
  , _rpk_path_binding(config::shard_local_cfg().rpk_path.bind())
  , _process_control_mutex("debug_bundle_service::process_control") {
    _debug_bundle_storage_dir_binding.watch([this] {
        _debug_bundle_dir = form_debug_bundle_storage_directory();
        lg.debug("Changed debug bundle directory to {}", _debug_bundle_dir);
    });
}

service::~service() noexcept = default;

ss::future<> service::start() {
    if (ss::this_shard_id() != service_shard) {
        co_return;
    }

    if (!co_await ss::file_exists(_rpk_path_binding().native())) {
        lg.error(
          "Current specified RPK location {} does not exist!  Debug "
          "bundle creation is not available until this is fixed!",
          _rpk_path_binding().native());
    }

    lg.debug("Service started");
}

ss::future<> service::stop() {
    lg.debug("Service stopping");
    if (ss::this_shard_id() == service_shard) {
        if (is_running()) {
            try {
                co_await _rpk_process->terminate(1s);
            } catch (const std::exception& e) {
                lg.warn(
                  "Failed to terminate running process while stopping service: "
                  "{}",
                  e.what());
            }
        }
    }
    co_await _gate.close();
}

ss::future<result<void>> service::initiate_rpk_debug_bundle_collection(
  job_id_t job_id, debug_bundle_parameters params) {
    auto hold = _gate.hold();
    if (ss::this_shard_id() != service_shard) {
        co_return co_await container().invoke_on(
          service_shard,
          [job_id, params = std::move(params)](service& s) mutable {
              return s.initiate_rpk_debug_bundle_collection(
                job_id, std::move(params));
          });
    }
    auto units = co_await _process_control_mutex.get_units();
    if (!co_await ss::file_exists(_rpk_path_binding().native())) {
        co_return error_info(
          error_code::rpk_binary_not_present,
          fmt::format("{} not present", _rpk_path_binding().native()));
    }

    if (_rpk_process) {
        // Must wait for both the process to no longer be running and for the
        // wait result to be populated
        if (is_running()) {
            co_return error_info(
              error_code::debug_bundle_process_running,
              "Debug process already running");
        }
    }

    try {
        co_await cleanup_previous_run();
    } catch (const std::exception& e) {
        co_return error_info(
          error_code::internal_error,
          fmt::format("Failed to clean up previous run: {}", e.what()));
    }

    // Make a copy of it now and use it throughout the initialize process
    // Protects against a situation where the config gets changed while setting
    // up the initialization parameters
    auto output_dir = _debug_bundle_dir;

    if (!co_await ss::file_exists(output_dir.native())) {
        try {
            co_await ss::recursive_touch_directory(output_dir.native());
        } catch (const std::exception& e) {
            co_return error_info(
              error_code::internal_error,
              fmt::format(
                "Failed to create debug bundle directory {}: {}",
                output_dir,
                e.what()));
        }
    }

    auto debug_bundle_file_path = form_debug_bundle_file_path(
      output_dir, job_id);
    auto process_output_path = form_process_output_file_path(
      output_dir, job_id);

    auto args_res = build_rpk_arguments(
      debug_bundle_file_path.native(), std::move(params));
    if (!args_res.has_value()) {
        co_return args_res.assume_error();
    }
    auto args = std::move(args_res.assume_value());
    if (lg.is_enabled(ss::log_level::debug)) {
        print_arguments(args);
    }

    try {
        _rpk_process = std::make_unique<debug_bundle_process>(
          job_id,
          co_await external_process::external_process::create_external_process(
            std::move(args)),
          std::move(debug_bundle_file_path),
          std::move(process_output_path));
    } catch (const std::exception& e) {
        _rpk_process.reset();
        co_return error_info(
          error_code::internal_error,
          fmt::format("Starting rpk debug bundle failed: {}", e.what()));
    }

    // Kick off the wait by waiting for the process to finish and then emplacing
    // the result
    ssx::spawn_with_gate(_gate, [this, job_id]() {
        return _rpk_process->wait()
          .then([this, job_id](auto) {
              auto hold = _gate.hold();
              return _process_control_mutex.get_units()
                .then([this, job_id](auto units) {
                    return handle_wait_result(job_id).finally(
                      [units = std::move(units)] {});
                })
                .finally([hold = std::move(hold)] {});
          })
          .handle_exception([](const std::exception_ptr& e) {
              lg.error("wait() failed while running rpk debug bundle: {}", e);
          });
    });

    co_return outcome::success();
}

ss::future<result<void>> service::cancel_rpk_debug_bundle(job_id_t job_id) {
    auto hold = _gate.hold();
    if (ss::this_shard_id() != service_shard) {
        co_return co_await container().invoke_on(
          service_shard,
          [job_id](service& s) { return s.cancel_rpk_debug_bundle(job_id); });
    }
    auto units = co_await _process_control_mutex.get_units();
    auto status = process_status();
    if (!status.has_value()) {
        co_return error_info(error_code::debug_bundle_process_never_started);
    } else if (!is_running()) {
        co_return error_info(error_code::debug_bundle_process_not_running);
    }

    vassert(
      _rpk_process,
      "_rpk_process should be populated if the process has been executed");

    if (job_id != _rpk_process->job_id()) {
        co_return error_info(error_code::job_id_not_recognized);
    }

    try {
        co_await _rpk_process->terminate(1s);
    } catch (const std::system_error& e) {
        if (
          e.code() == external_process::error_code::process_already_completed) {
            co_return error_info(error_code::debug_bundle_process_not_running);
        }
        co_return (error_info(error_code::internal_error, e.what()));
    } catch (const std::exception& e) {
        co_return error_info(error_code::internal_error, e.what());
    }

    co_return outcome::success();
}

ss::future<result<debug_bundle_status_data>>
service::rpk_debug_bundle_status() {
    auto hold = _gate.hold();
    if (ss::this_shard_id() != service_shard) {
        co_return co_await container().invoke_on(service_shard, [](service& s) {
            return s.rpk_debug_bundle_status();
        });
    }
    auto status = process_status();
    if (!status.has_value()) {
        co_return error_info(error_code::debug_bundle_process_never_started);
    }

    vassert(
      _rpk_process,
      "_rpk_process should be populated if the process has been executed");

    auto& output_file = _rpk_process->output_file_path().native();

    std::optional<size_t> file_size;
    if (status.value() == debug_bundle_status::success) {
        try {
            file_size.emplace(co_await ss::file_size(output_file));
        } catch (const std::exception& e) {
            co_return error_info(
              error_code::internal_error,
              fmt::format(
                "Failed to get file size for debug bundle file {}: {}",
                output_file,
                e.what()));
        }
    }

    co_return debug_bundle_status_data{
      .job_id = _rpk_process->job_id(),
      .status = status.value(),
      .created_timestamp = _rpk_process->created_time(),
      .file_name = _rpk_process->output_file_path().filename().native(),
      .file_size = file_size,
      .cout = _rpk_process->cout().copy(),
      .cerr = _rpk_process->cerr().copy()};
}

ss::future<result<std::filesystem::path>>
service::rpk_debug_bundle_path(job_id_t job_id) {
    if (ss::this_shard_id() != service_shard) {
        co_return co_await container().invoke_on(
          service_shard,
          [job_id](service& s) { return s.rpk_debug_bundle_path(job_id); });
    }
    auto units = co_await _process_control_mutex.get_units();
    auto status = process_status();
    if (!status.has_value()) {
        co_return error_info(error_code::debug_bundle_process_never_started);
    }
    switch (status.value()) {
    case debug_bundle_status::running:
        co_return error_info(error_code::debug_bundle_process_running);
    case debug_bundle_status::success:
        break;
    case debug_bundle_status::error:
        co_return error_info(error_code::process_failed);
    }
    if (job_id != _rpk_process->job_id()) {
        co_return error_info(error_code::job_id_not_recognized);
    }
    if (!co_await ss::file_exists(_rpk_process->output_file_path().native())) {
        co_return error_info(
          error_code::internal_error,
          fmt::format(
            "Debug bundle file {} not found",
            _rpk_process->output_file_path().native()));
    }
    co_return _rpk_process->output_file_path();
}

ss::future<result<void>> service::delete_rpk_debug_bundle(job_id_t job_id) {
    if (ss::this_shard_id() != service_shard) {
        co_return co_await container().invoke_on(
          service_shard,
          [job_id](service& s) { return s.delete_rpk_debug_bundle(job_id); });
    }
    auto units = co_await _process_control_mutex.get_units();
    auto status = process_status();
    if (!status.has_value()) {
        co_return error_info(error_code::debug_bundle_process_never_started);
    }
    switch (status.value()) {
    case debug_bundle_status::running:
        co_return error_info(error_code::debug_bundle_process_running);
    case debug_bundle_status::success:
    case debug_bundle_status::error:
        // Attempt the removal of the file even if the process errored out just
        // in case the file was created
        break;
    }
    if (_rpk_process->job_id() != job_id) {
        co_return error_info(error_code::job_id_not_recognized);
    }
    try {
        if (co_await ss::file_exists(
              _rpk_process->output_file_path().native())) {
            co_await ss::remove_file(_rpk_process->output_file_path().native());
        }
    } catch (const std::exception& e) {
        co_return error_info(
          error_code::internal_error,
          fmt::format(
            "Failed to delete debug bundle file {}: {}",
            _rpk_process->output_file_path(),
            e.what()));
    }
    co_return outcome::success();
}

result<std::vector<ss::sstring>> service::build_rpk_arguments(
  std::string_view debug_bundle_file_path, debug_bundle_parameters params) {
    std::vector<ss::sstring> rv{
      _rpk_path_binding().native(), "debug", "bundle"};
    rv.emplace_back(output_variable);
    rv.emplace_back(debug_bundle_file_path);
    rv.emplace_back(verbose_variable);
    if (params.authn_options.has_value()) {
        ss::visit(
          params.authn_options.value(),
          [&rv](const scram_creds& creds) mutable {
              rv.emplace_back(
                ssx::sformat("{}={}", username_variable, creds.username));
              rv.emplace_back(
                ssx::sformat("{}={}", password_variable, creds.password));
              rv.emplace_back(ssx::sformat(
                "{}={}", sasl_mechanism_variable, creds.mechanism));
          });
    }
    if (params.controller_logs_size_limit_bytes.has_value()) {
        rv.emplace_back(controller_logs_size_limit_variable);
        rv.emplace_back(
          ssx::sformat("{}B", params.controller_logs_size_limit_bytes.value()));
    }
    if (params.cpu_profiler_wait_seconds.has_value()) {
        rv.emplace_back(cpu_profiler_wait_variable);
        rv.emplace_back(ssx::sformat(
          "{}s", params.cpu_profiler_wait_seconds.value().count()));
    }
    if (params.logs_since.has_value()) {
        rv.emplace_back(logs_since_variable);
        rv.emplace_back(ssx::sformat("{}", params.logs_since.value()));
    }
    if (params.logs_size_limit_bytes.has_value()) {
        rv.emplace_back(logs_size_limit_variable);
        rv.emplace_back(
          ssx::sformat("{}B", params.logs_size_limit_bytes.value()));
    }
    if (params.logs_until.has_value()) {
        rv.emplace_back(logs_until_variable);
        rv.emplace_back(ssx::sformat("{}", params.logs_until.value()));
    }
    if (params.metrics_interval_seconds.has_value()) {
        rv.emplace_back(metrics_interval_variable);
        rv.emplace_back(
          ssx::sformat("{}s", params.metrics_interval_seconds.value().count()));
    }
    if (params.partition.has_value()) {
        rv.emplace_back(partition_variable);
        rv.emplace_back(
          ssx::sformat("{}", fmt::join(params.partition.value(), " ")));
    }
    if (params.tls_enabled.has_value()) {
        rv.emplace_back(
          ssx::sformat("{}={}", tls_enabled_variable, *params.tls_enabled));
    }
    if (params.tls_insecure_skip_verify.has_value()) {
        rv.emplace_back(ssx::sformat(
          "{}={}",
          tls_insecure_skip_verify_variable,
          *params.tls_insecure_skip_verify));
    }
    if (params.k8s_namespace.has_value()) {
        if (!is_valid_k8s_namespace(params.k8s_namespace.value()())) {
            return error_info(
              error_code::invalid_parameters, "Invalid k8s namespace name");
        }
        rv.emplace_back(k8s_namespace_variable);
        rv.emplace_back(*params.k8s_namespace);
    }

    return rv;
}

ss::future<> service::cleanup_previous_run() const {
    if (_rpk_process == nullptr) {
        co_return;
    }

    auto& debug_bundle_file = _rpk_process->output_file_path().native();
    auto& process_output_file
      = _rpk_process->process_output_file_path().native();
    if (co_await ss::file_exists(debug_bundle_file)) {
        vlog(
          lg.debug,
          "Cleaning up previous debug bundle run {}",
          debug_bundle_file);
        co_await ss::remove_file(debug_bundle_file);
    }

    if (co_await ss::file_exists(process_output_file)) {
        vlog(
          lg.debug,
          "Cleaning up previous process output run {}",
          process_output_file);
        co_await ss::remove_file(process_output_file);
    }

    co_await _kvstore->remove(
      storage::kvstore::key_space::debug_bundle,
      bytes::from_string(debug_bundle_metadata_key));
}

ss::future<> service::set_metadata(job_id_t job_id) {
    auto& debug_bundle_file = _rpk_process->output_file_path();
    auto& process_output_file = _rpk_process->process_output_file_path();
    auto run_successful = was_run_successful(_rpk_process->get_wait_result());

    bytes sha256_checksum{};
    if (run_successful) {
        if (!co_await ss::file_exists(debug_bundle_file.native())) {
            vlog(
              lg.warn,
              "Debug bundle file {} does not exist post successful run, cannot "
              "set "
              "metadata",
              debug_bundle_file);
            co_return;
        }
        sha256_checksum = co_await calculate_sha256_sum(
          debug_bundle_file.native());
    }

    metadata md(
      _rpk_process->created_time(),
      job_id,
      debug_bundle_file,
      process_output_file,
      std::move(sha256_checksum),
      _rpk_process->get_wait_result());

    iobuf buf;
    serde::write(buf, std::move(md));

    vlog(lg.debug, "Emplacing metadata into keystore for job {}", job_id);

    co_await _kvstore->put(
      storage::kvstore::key_space::debug_bundle,
      bytes::from_string(debug_bundle_metadata_key),
      std::move(buf));

    auto remove_kvstore_on_err = ss::defer([this] {
        ssx::background = _kvstore->remove(
          storage::kvstore::key_space::debug_bundle,
          bytes::from_string(debug_bundle_metadata_key));
    });

    process_output po{
      .cout = _rpk_process->cout().copy(), .cerr = _rpk_process->cerr().copy()};
    iobuf po_buf;
    serde::write(po_buf, std::move(po));

    vlog(
      lg.debug,
      "Writing process output to {} for job {}",
      process_output_file,
      job_id);

    try {
        co_await write_file(process_output_file.native(), std::move(po_buf));
        vlog(
          lg.debug,
          "Successfully wrote process output to file to {}",
          process_output_file.native());
        remove_kvstore_on_err.cancel();
    } catch (const std::exception& e) {
        vlog(
          lg.warn,
          "Failed to write process output to file {} for job {}: {}",
          process_output_file,
          job_id,
          e.what());
    }
}

std::optional<debug_bundle_status> service::process_status() const {
    if (_rpk_process == nullptr) {
        return std::nullopt;
    }
    return _rpk_process->process_status();
}

bool service::is_running() const {
    if (_rpk_process == nullptr) {
        return false;
    }
    return _rpk_process->process_status() == debug_bundle_status::running;
}

ss::future<> service::handle_wait_result(job_id_t job_id) {
    vlog(lg.debug, "Wait completed for job {}", job_id);
    // This ensures in the extremely unlikely case where this
    // continuation is called after another debug bundle has been
    // initiated, that we are accessing a present and valid
    // _rpk_process with the same job id
    if (!_rpk_process || _rpk_process->job_id() != job_id) {
        vlog(
          lg.debug,
          "Unable to enqueue metadata for job {}, "
          "another process already started",
          job_id());
        co_return;
    }
    try {
        co_return co_await set_metadata(job_id);
    } catch (const std::exception& e) {
        vlog(
          lg.warn, "Failed to set metadata for job {}: {}", job_id, e.what());
        co_return;
    }
}

} // namespace debug_bundle
