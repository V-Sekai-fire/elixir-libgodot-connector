// weft_client_nif: a small NIF wrapping the weft::harness client side.
//
// The BEAM stays off the hot path — this NIF is only for the low-frequency
// lifecycle channel (elixir tells libgodot_host to create/start/iterate/
// destroy an engine instance). Framing is the same as the server side
// (2-contract/bus / weft::harness): DYNAMIC-payload pub/sub, 8-byte
// request-id prefix, opaque body.
//
// Exposed NIFs (dirty scheduler because iceoryx2 receive can block on
// iox2_node_wait):
//
//   open()                                 -> {ok, ref} | {error, reason}
//   call(ref, body_binary, timeout_ms)     -> {ok, reply_binary} | {error, reason}
//   close(ref)                             -> :ok
//
// Compiled but not exercised yet — iceoryx2 must be reachable via
// WEFT_ICEORYX2_PATH at runtime for open/0 to succeed. The smoke test
// suite for this NIF is a follow-up commit once an iceoryx2 build is
// staged in the workspace.

#include <erl_nif.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include "weft/bus.hpp"
#include "weft/command.hpp"

// The bus C ABI is reached through the dlsym table weft::load_bus fills in;
// nothing here links iceoryx2, exactly like the server side.
#include "iox2_api.h"

namespace {

struct WeftClient {
    iox2_node_h node = nullptr;
    iox2_port_factory_pub_sub_h cmd_service = nullptr;
    iox2_port_factory_pub_sub_h reply_service = nullptr;
    iox2_publisher_h publisher = nullptr;
    iox2_subscriber_h subscriber = nullptr;
    std::atomic<std::uint64_t> next_request_id{1};
};

ErlNifResourceType *WEFT_CLIENT_RES = nullptr;

void weft_client_dtor(ErlNifEnv *, void *obj) {
    auto *c = static_cast<WeftClient *>(obj);
    if (c->publisher) iox2_publisher_drop(c->publisher);
    if (c->subscriber) iox2_subscriber_drop(c->subscriber);
    if (c->cmd_service) iox2_port_factory_pub_sub_drop(c->cmd_service);
    if (c->reply_service) iox2_port_factory_pub_sub_drop(c->reply_service);
    if (c->node) iox2_node_drop(c->node);
    // Placement destructor call for the atomic member.
    c->~WeftClient();
}

int on_load(ErlNifEnv *env, void **, ERL_NIF_TERM) {
    WEFT_CLIENT_RES = enif_open_resource_type(env, nullptr, "weft_client",
            weft_client_dtor, ERL_NIF_RT_CREATE, nullptr);
    return WEFT_CLIENT_RES ? 0 : 1;
}

ERL_NIF_TERM err(ErlNifEnv *env, const char *tag) {
    return enif_make_tuple2(env, enif_make_atom(env, "error"), enif_make_atom(env, tag));
}

iox2_port_factory_pub_sub_h open_service(iox2_node_h *node, const char *name) {
    iox2_service_name_h svc_name = nullptr;
    if (iox2_service_name_new(nullptr, name, std::strlen(name), &svc_name) != IOX2_OK) {
        return nullptr;
    }
    auto builder = iox2_service_builder_pub_sub(
            iox2_node_service_builder(node, nullptr, iox2_cast_service_name_ptr(svc_name)));
    if (iox2_service_builder_pub_sub_set_payload_type_details(&builder,
                iox2_type_variant_e_DYNAMIC, weft::PAYLOAD_TYPE, std::strlen(weft::PAYLOAD_TYPE),
                1, 1) != IOX2_OK) {
        iox2_service_name_drop(svc_name);
        return nullptr;
    }
    iox2_port_factory_pub_sub_h service = nullptr;
    const int rc = iox2_service_builder_pub_sub_open_or_create(builder, nullptr, &service);
    iox2_service_name_drop(svc_name);
    return rc == IOX2_OK ? service : nullptr;
}

ERL_NIF_TERM nif_open(ErlNifEnv *env, int, const ERL_NIF_TERM *) {
    if (!weft::load_bus()) return err(env, "bus_unreachable");
    iox2_set_log_level_from_env_or(iox2_log_level_e_ERROR);

    auto *c = static_cast<WeftClient *>(enif_alloc_resource(WEFT_CLIENT_RES, sizeof(WeftClient)));
    new (c) WeftClient();

    if (iox2_node_builder_create(iox2_node_builder_new(nullptr), nullptr,
                iox2_service_type_e_IPC, &c->node) != IOX2_OK) {
        enif_release_resource(c);
        return err(env, "no_node");
    }
    c->cmd_service = open_service(&c->node, weft::COMMAND_SERVICE_NAME);
    c->reply_service = open_service(&c->node, weft::REPLY_SERVICE_NAME);
    if (!c->cmd_service || !c->reply_service) {
        enif_release_resource(c);
        return err(env, "no_service");
    }

    auto pub_builder = iox2_port_factory_pub_sub_publisher_builder(&c->cmd_service, nullptr);
    iox2_port_factory_publisher_builder_set_initial_max_slice_len(&pub_builder, weft::MESSAGE_BYTES);
    if (iox2_port_factory_publisher_builder_create(pub_builder, nullptr, &c->publisher) != IOX2_OK) {
        enif_release_resource(c);
        return err(env, "no_publisher");
    }
    if (iox2_port_factory_subscriber_builder_create(
                iox2_port_factory_pub_sub_subscriber_builder(&c->reply_service, nullptr), nullptr,
                &c->subscriber) != IOX2_OK) {
        enif_release_resource(c);
        return err(env, "no_subscriber");
    }

    ERL_NIF_TERM handle = enif_make_resource(env, c);
    enif_release_resource(c);
    return enif_make_tuple2(env, enif_make_atom(env, "ok"), handle);
}

ERL_NIF_TERM nif_call(ErlNifEnv *env, int argc, const ERL_NIF_TERM *argv) {
    if (argc != 3) return enif_make_badarg(env);
    WeftClient *c = nullptr;
    if (!enif_get_resource(env, argv[0], WEFT_CLIENT_RES, (void **)&c)) return enif_make_badarg(env);
    ErlNifBinary body;
    if (!enif_inspect_binary(env, argv[1], &body)) return enif_make_badarg(env);
    unsigned int timeout_ms = 0;
    if (!enif_get_uint(env, argv[2], &timeout_ms)) return enif_make_badarg(env);
    if (body.size + weft::HEADER_BYTES > weft::MESSAGE_BYTES) return err(env, "body_too_large");

    const std::uint64_t request_id = c->next_request_id.fetch_add(1, std::memory_order_relaxed);

    // Loan + write + send the command.
    iox2_sample_mut_h out = nullptr;
    if (iox2_publisher_loan_slice_uninit(&c->publisher, nullptr, &out,
                weft::HEADER_BYTES + body.size) != IOX2_OK) {
        return err(env, "loan_failed");
    }
    void *out_payload = nullptr;
    size_t out_n = 0;
    iox2_sample_mut_payload_mut(&out, &out_payload, &out_n);
    weft::command_write_header(static_cast<unsigned char *>(out_payload), request_id);
    std::memcpy(static_cast<unsigned char *>(out_payload) + weft::HEADER_BYTES, body.data, body.size);
    if (iox2_sample_mut_send(out, nullptr) != IOX2_OK) return err(env, "send_failed");

    // Wait for the reply with our request_id, dropping stragglers.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        iox2_sample_h reply = nullptr;
        if (iox2_subscriber_receive(&c->subscriber, nullptr, &reply) != IOX2_OK) {
            return err(env, "receive_failed");
        }
        if (!reply) {
            if (std::chrono::steady_clock::now() >= deadline) return err(env, "timeout");
            // Idle wait; 1ms so a fast round-trip does not sit for the full 10ms
            // command_publisher.cpp uses (elixir will typically ask at a much
            // lower rate than the bus round-trip time).
            (void)iox2_node_wait(&c->node, 0, 1'000'000);
            continue;
        }
        const void *rp = nullptr;
        size_t rn = 0;
        iox2_sample_payload(&reply, &rp, &rn);
        if (rn < weft::HEADER_BYTES) {
            iox2_sample_drop(reply);
            continue;
        }
        const std::uint64_t rid = weft::command_read_header(static_cast<const unsigned char *>(rp));
        if (rid != request_id) {
            iox2_sample_drop(reply);
            continue;
        }
        const size_t reply_body = rn - weft::HEADER_BYTES;
        ErlNifBinary out_bin;
        if (!enif_alloc_binary(reply_body, &out_bin)) {
            iox2_sample_drop(reply);
            return err(env, "alloc_failed");
        }
        std::memcpy(out_bin.data, static_cast<const unsigned char *>(rp) + weft::HEADER_BYTES, reply_body);
        iox2_sample_drop(reply);
        return enif_make_tuple2(env, enif_make_atom(env, "ok"), enif_make_binary(env, &out_bin));
    }
}

ERL_NIF_TERM nif_close(ErlNifEnv *env, int, const ERL_NIF_TERM *argv) {
    WeftClient *c = nullptr;
    if (!enif_get_resource(env, argv[0], WEFT_CLIENT_RES, (void **)&c)) return enif_make_badarg(env);
    // The dtor runs when the last reference drops; nothing else to do here.
    (void)c;
    return enif_make_atom(env, "ok");
}

ErlNifFunc funcs[] = {
    {"open", 0, nif_open, 0},
    {"call", 3, nif_call, ERL_NIF_DIRTY_JOB_IO_BOUND},
    {"close", 1, nif_close, 0},
};

} // namespace

ERL_NIF_INIT(Elixir.Weft.Client.NIF, funcs, on_load, nullptr, nullptr, nullptr)
