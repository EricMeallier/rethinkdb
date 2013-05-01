#include "rdb_protocol/datum_stream.hpp"

#include <map>

#include "clustering/administration/metadata.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/term.hpp"
#include "rdb_protocol/val.hpp"

namespace ql {

// DATUM_STREAM_T
counted_t<datum_stream_t> datum_stream_t::slice(size_t l, size_t r) {
    return make_counted<slice_datum_stream_t>(env, l, r, this->counted_from_this());
}
counted_t<datum_stream_t> datum_stream_t::zip() {
    return make_counted<zip_datum_stream_t>(env, this->counted_from_this());
}

counted_t<const datum_t> datum_stream_t::next() {
    // This is a hook for unit tests to change things mid-query.
    DEBUG_ONLY_CODE(env->do_eval_callback());
    env->throw_if_interruptor_pulsed();
    try {
        return next_impl();
    } catch (const datum_exc_t &e) {
        rfail("%s", e.what());
        unreachable();
    }
}

// SAMRSI: Fixed this batched stuff.
std::vector<counted_t<const datum_t> > datum_stream_t::next_batch() {
    env->throw_if_interruptor_pulsed();
    try {
        std::vector<counted_t<const datum_t> > batch;
        for (;;) {
            counted_t<const datum_t> datum = next_impl();
            if (datum.has()) {
                batch.push_back(datum);
            }
            if (!datum.has() || batch.size() == MAX_BATCH_SIZE) {
                return batch;
            }
        }
    } catch (const datum_exc_t &e) {
        rfail("%s", e.what());
        unreachable();
    }
}

counted_t<const datum_t> eager_datum_stream_t::count() {
    int64_t i = 0;
    for (;;) {
        counted_t<const datum_t> value = next();
        if (!value.has()) { break; }
        ++i;
    }
    return make_counted<datum_t>(static_cast<double>(i));
}

counted_t<const datum_t> eager_datum_stream_t::reduce(counted_t<val_t> base_val, counted_t<func_t> f) {
    counted_t<const datum_t> base = base_val.has() ? base_val->as_datum() : next();
    rcheck(base.has(), "Cannot reduce over an empty stream with no base.");

    while (counted_t<const datum_t> rhs = next()) {
        base = f->call(base, rhs)->as_datum();
    }
    return base;
}

counted_t<const datum_t> eager_datum_stream_t::gmr(
    counted_t<func_t> group, counted_t<func_t> map, counted_t<const datum_t> base, counted_t<func_t> reduce) {
    wire_datum_map_t wd_map;
    while (counted_t<const datum_t> el = next()) {
        counted_t<const datum_t> el_group = group->call(el)->as_datum();
        counted_t<const datum_t> el_map = map->call(el)->as_datum();
        if (!wd_map.has(el_group)) {
            wd_map.set(el_group, base.has() ? reduce->call(base, el_map)->as_datum() : el_map);
        } else {
            wd_map.set(el_group, reduce->call(wd_map.get(el_group), el_map)->as_datum());
        }
    }
    return wd_map.to_arr();
}

counted_t<datum_stream_t> eager_datum_stream_t::filter(counted_t<func_t> f) {
    return make_counted<filter_datum_stream_t>(env, f, this->counted_from_this());
}
counted_t<datum_stream_t> eager_datum_stream_t::map(counted_t<func_t> f) {
    return make_counted<map_datum_stream_t>(env, f, this->counted_from_this());
}
counted_t<datum_stream_t> eager_datum_stream_t::concatmap(counted_t<func_t> f) {
    return make_counted<concatmap_datum_stream_t>(env, f, this->counted_from_this());
}

counted_t<const datum_t> eager_datum_stream_t::as_array() {
    scoped_ptr_t<datum_t> arr(new datum_t(datum_t::R_ARRAY));
    while (counted_t<const datum_t> d = next()) {
        arr->add(d);
    }
    return counted_t<const datum_t>(arr.release());
}

// LAZY_DATUM_STREAM_T
lazy_datum_stream_t::lazy_datum_stream_t(
    env_t *env, bool use_outdated, namespace_repo_t<rdb_protocol_t>::access_t *ns_access,
    const pb_rcheckable_t *bt_src)
    : datum_stream_t(env, bt_src),
      json_stream(new query_language::batched_rget_stream_t(
                      *ns_access, env->interruptor, key_range_t::universe(),
                      env->get_all_optargs(), use_outdated))
{ }

lazy_datum_stream_t::lazy_datum_stream_t(
    env_t *env, bool use_outdated, namespace_repo_t<rdb_protocol_t>::access_t *ns_access,
    counted_t<const datum_t> pval, const std::string &sindex_id,
    const pb_rcheckable_t *bt_src)
    : datum_stream_t(env, bt_src),
      json_stream(new query_language::batched_rget_stream_t(
                      *ns_access, env->interruptor, sindex_id,
                      env->get_all_optargs(), use_outdated, pval, pval))
{ }

lazy_datum_stream_t::lazy_datum_stream_t(const lazy_datum_stream_t *src)
    : datum_stream_t(src->env, src), json_stream(src->json_stream) { }

counted_t<datum_stream_t> lazy_datum_stream_t::map(counted_t<func_t> f) {
    scoped_ptr_t<lazy_datum_stream_t> out(new lazy_datum_stream_t(this));
    out->json_stream = json_stream->add_transformation(
        rdb_protocol_details::transform_variant_t(map_wire_func_t(env, f)),
        env, query_language::scopes_t(), query_language::backtrace_t());
    return counted_t<datum_stream_t>(out.release());
}

counted_t<datum_stream_t> lazy_datum_stream_t::concatmap(counted_t<func_t> f) {
    scoped_ptr_t<lazy_datum_stream_t> out(new lazy_datum_stream_t(this));
    out->json_stream = json_stream->add_transformation(
        rdb_protocol_details::transform_variant_t(concatmap_wire_func_t(env, f)),
        env, query_language::scopes_t(), query_language::backtrace_t());  // SAMRSI: Make sure next passes the same scopes and backtrace value.
    return counted_t<datum_stream_t>(out.release());
}
counted_t<datum_stream_t> lazy_datum_stream_t::filter(counted_t<func_t> f) {
    scoped_ptr_t<lazy_datum_stream_t> out(new lazy_datum_stream_t(this));
    out->json_stream = json_stream->add_transformation(
        rdb_protocol_details::transform_variant_t(filter_wire_func_t(env, f)),
        env, query_language::scopes_t(), query_language::backtrace_t());  // SAMRSI: Make sure next passes the same scopes and backtrace value here, too.
    return counted_t<datum_stream_t>(out.release());
}

// This applies a terminal to the JSON stream, evaluates it, and pulls out the
// shard data.
rdb_protocol_t::rget_read_response_t::result_t lazy_datum_stream_t::run_terminal(const rdb_protocol_details::terminal_variant_t &t) {
    return json_stream->apply_terminal(t,
                                       env,
                                       query_language::scopes_t(),
                                       query_language::backtrace_t());
}

counted_t<const datum_t> lazy_datum_stream_t::count() {
    rdb_protocol_t::rget_read_response_t::result_t res = run_terminal(count_wire_func_t());
    wire_datum_t *wire_datum = boost::get<wire_datum_t>(&res);
    r_sanity_check(wire_datum != NULL);
    return wire_datum->compile(env);
}

counted_t<const datum_t> lazy_datum_stream_t::reduce(counted_t<val_t> base_val, counted_t<func_t> f) {
    rdb_protocol_t::rget_read_response_t::result_t res =
        run_terminal(reduce_wire_func_t(env, f));

    if (wire_datum_t *wire_datum = boost::get<wire_datum_t>(&res)) {
        counted_t<const datum_t> datum = wire_datum->compile(env);
        if (base_val.has()) {
            return f->call(base_val->as_datum(), datum)->as_datum();
        } else {
            return datum;
        }
    } else {
        r_sanity_check(boost::get<rdb_protocol_t::rget_read_response_t::empty_t>(&res));
        if (base_val.has()) {
            return base_val->as_datum();
        } else {
            rfail("Cannot reduce over an empty stream with no base.");
        }
    }
}

counted_t<const datum_t> lazy_datum_stream_t::gmr(
    counted_t<func_t> g, counted_t<func_t> m, counted_t<const datum_t> base, counted_t<func_t> r) {
    rdb_protocol_t::rget_read_response_t::result_t res =
        json_stream->apply_terminal(
            rdb_protocol_details::terminal_variant_t(gmr_wire_func_t(env, g, m, r)),
            env, query_language::scopes_t(), query_language::backtrace_t());
    wire_datum_map_t *dm = boost::get<wire_datum_map_t>(&res);
    r_sanity_check(dm);
    dm->compile(env);
    counted_t<const datum_t> dm_arr = dm->to_arr();
    if (!base.has()) {
        return dm_arr;
    } else {
        wire_datum_map_t map;

        for (size_t f = 0; f < dm_arr->size(); ++f) {
            counted_t<const datum_t> key = dm_arr->get(f)->get("group");
            counted_t<const datum_t> val = dm_arr->get(f)->get("reduction");
            r_sanity_check(!map.has(key));
            map.set(key, r->call(base, val)->as_datum());
        }
        return map.to_arr();
    }
}

counted_t<const datum_t> lazy_datum_stream_t::next_impl() {
    boost::shared_ptr<scoped_cJSON_t> json = json_stream->next();
    return json ? make_counted<datum_t>(json, env) : counted_t<datum_t>();
}

// ARRAY_DATUM_STREAM_T
array_datum_stream_t::array_datum_stream_t(env_t *env, counted_t<const datum_t> _arr,
                                           const pb_rcheckable_t *backtrace_source)
    : eager_datum_stream_t(env, backtrace_source), index(0), arr(_arr) { }

counted_t<const datum_t> array_datum_stream_t::next_impl() {
    counted_t<const datum_t> datum = arr->get(index, NOTHROW);
    if (!datum.has()) {
        return counted_t<const datum_t>();
    } else {
        ++index;
        return datum;
    }
}

// MAP_DATUM_STREAM_T
counted_t<const datum_t> map_datum_stream_t::next_impl() {
    counted_t<const datum_t> arg = source->next();
    if (!arg.has()) {
        return counted_t<const datum_t>();
    } else {
        return f->call(arg)->as_datum();
    }
}

// FILTER_DATUM_STREAM_T
counted_t<const datum_t> filter_datum_stream_t::next_impl() {
    for (;;) {
        counted_t<const datum_t> arg = source->next();

        if (!arg.has()) {
            return counted_t<const datum_t>();
        }

        if (f->filter_call(arg)) {
            return arg;
        }
    }
}

// CONCATMAP_DATUM_STREAM_T
counted_t<const datum_t> concatmap_datum_stream_t::next_impl() {
    for (;;) {
        if (!subsource.has()) {
            counted_t<const datum_t> arg = source->next();
            if (!arg.has()) {
                return counted_t<const datum_t>();
            }
            subsource = f->call(arg)->as_seq();
        }

        counted_t<const datum_t> datum = subsource->next();
        if (datum.has()) {
            return datum;
        }

        subsource.reset();
    }
}

// SLICE_DATUM_STREAM_T
slice_datum_stream_t::slice_datum_stream_t(env_t *env, size_t _left, size_t _right,
                                           counted_t<datum_stream_t> _src)
    : wrapper_datum_stream_t(env, _src), index(0),
      left(_left), right(_right) { }

counted_t<const datum_t> slice_datum_stream_t::next_impl() {
    if (left > right || index > right) {
        return counted_t<const datum_t>();
    }

    while (index < left) {
        counted_t<const datum_t> discard = src_stream()->next();
        if (!discard.has()) {
            return counted_t<const datum_t>();
        }
        ++index;
    }

    counted_t<const datum_t> datum = src_stream()->next();
    if (datum.has()) {
        ++index;
    }
    return datum;
}

// ZIP_DATUM_STREAM_T
zip_datum_stream_t::zip_datum_stream_t(env_t *env, counted_t<datum_stream_t> _src)
    : wrapper_datum_stream_t(env, _src) { }

counted_t<const datum_t> zip_datum_stream_t::next_impl() {
    counted_t<const datum_t> datum = src_stream()->next();
    if (!datum.has()) {
        return counted_t<const datum_t>();
    }

    counted_t<const datum_t> left = datum->get("left", NOTHROW);
    counted_t<const datum_t> right = datum->get("right", NOTHROW);
    rcheck(left.has(), "ZIP can only be called on the result of a join.");
    return right.has() ? left->merge(right) : left;
}


// UNION_DATUM_STREAM_T
counted_t<const datum_t> union_datum_stream_t::next_impl() {
    for (; streams_index < streams.size(); ++streams_index) {
        counted_t<const datum_t> datum = streams[streams_index]->next();
        if (datum.has()) {
            return datum;
        }
    }
    return counted_t<const datum_t>();
}

} // namespace ql
