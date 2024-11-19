/*
 * Copyright 2024 Redpanda Data, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/redpanda-data/redpanda/blob/master/licenses/rcl.md
 */
#include "datalake/record_translator.h"

#include "base/vlog.h"
#include "datalake/conversion_outcome.h"
#include "datalake/logger.h"
#include "datalake/record_schema_resolver.h"
#include "datalake/table_definition.h"
#include "datalake/values_avro.h"
#include "datalake/values_protobuf.h"
#include "iceberg/avro_utils.h"
#include "iceberg/datatypes.h"
#include "iceberg/values.h"

#include <avro/Generic.hh>
#include <avro/GenericDatum.hh>

namespace datalake {

namespace {

struct value_translating_visitor {
    // Buffer ready to be parsed, e.g. no schema ID or protobuf offsets.
    iobuf parsable_buf;
    const iceberg::field_type& type;

    ss::future<optional_value_outcome>
    operator()(const wrapped_protobuf_descriptor& d) {
        return deserialize_protobuf(std::move(parsable_buf), d.descriptor);
    }
    ss::future<optional_value_outcome> operator()(const avro::ValidSchema& s) {
        auto value = co_await deserialize_avro(std::move(parsable_buf), s);
        if (value.has_error()) {
            co_return optional_value_outcome(value.error());
        }
        co_return std::move(value.value());
    }
};

std::optional<size_t> get_redpanda_idx(const iceberg::struct_type& val_type) {
    for (size_t i = 0; i < val_type.fields.size(); ++i) {
        if (val_type.fields[i]->name == rp_struct_name) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

std::ostream& operator<<(std::ostream& o, const record_translator::errc& e) {
    switch (e) {
    case record_translator::errc::translation_error:
        return o << "record_translator::errc::translation_error";
    }
}

record_type
record_translator::build_type(std::optional<resolved_type> val_type) {
    auto ret_type = schemaless_struct_type();
    std::optional<schema_identifier> val_id;
    if (val_type.has_value()) {
        val_id = std::move(val_type->id);
        auto& struct_type = std::get<iceberg::struct_type>(val_type->type);
        for (auto& field : struct_type.fields) {
            if (field->name == rp_struct_name) {
                // To avoid collisions, move user fields named "redpanda" into
                // the nested "redpanda" system field.
                auto& system_fields = std::get<iceberg::struct_type>(
                  ret_type.fields[0]->type);
                // Use the next id of the system defaults.
                system_fields.fields.emplace_back(iceberg::nested_field::create(
                  6, "data", field->required, std::move(field->type)));
                continue;
            }
            // Add the extra user-defined fields.
            ret_type.fields.emplace_back(std::move(field));
        }
    }
    return record_type{
      .comps = record_schema_components{
          .key_identifier = std::nullopt,
          .val_identifier = std::move(val_id),
      },
      .type = std::move(ret_type),
    };
}

ss::future<checked<iceberg::struct_value, record_translator::errc>>
record_translator::translate_data(
  kafka::offset o,
  iobuf key,
  const std::optional<resolved_type>& val_type,
  iobuf parsable_val,
  model::timestamp ts) {
    auto ret_data = iceberg::struct_value{};
    auto system_data = std::make_unique<iceberg::struct_value>();
    system_data->fields.emplace_back(iceberg::long_value(o));
    // NOTE: Kafka uses milliseconds, Iceberg uses microseconds.
    system_data->fields.emplace_back(
      iceberg::timestamp_value(ts.value() * 1000));
    system_data->fields.emplace_back(iceberg::binary_value{std::move(key)});
    if (val_type.has_value()) {
        // Fill in the internal value field.
        system_data->fields.emplace_back(std::nullopt);
        ret_data.fields.emplace_back(std::move(system_data));

        auto translated_val = co_await std::visit(
          value_translating_visitor{std::move(parsable_val), val_type->type},
          val_type->schema);
        if (translated_val.has_error()) {
            vlog(
              datalake_log.error,
              "Error converting buffer: {}",
              translated_val.error());
            // TODO: metric for data translation errors.
            // Either needs to drop the data or send it to a dead-letter queue.
            co_return errc::translation_error;
        }

        auto redpanda_field_idx = get_redpanda_idx(
          std::get<iceberg::struct_type>(val_type->type));
        // Unwrap the struct fields.
        auto& val_struct = std::get<std::unique_ptr<iceberg::struct_value>>(
          translated_val.value().value());
        for (size_t i = 0; i < val_struct->fields.size(); ++i) {
            auto& field = val_struct->fields[i];
            if (redpanda_field_idx == i) {
                // To avoid collisions, move user fields named "redpanda" into
                // the nested "redpanda" system field.
                auto& system_vals
                  = std::get<std::unique_ptr<iceberg::struct_value>>(
                    ret_data.fields[0].value());
                system_vals->fields.emplace_back(std::move(field));
                continue;
            }
            ret_data.fields.emplace_back(std::move(field));
        }
    } else {
        system_data->fields.emplace_back(
          iceberg::binary_value{std::move(parsable_val)});

        // Just add the system fields.
        ret_data.fields.emplace_back(std::move(system_data));
    }
    co_return ret_data;
}

} // namespace datalake
