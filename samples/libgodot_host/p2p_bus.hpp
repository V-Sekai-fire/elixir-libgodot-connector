// P2PBus: a godot::Object GDScript can reach to send and receive
// byte-slice frames between libgodot_host peers over weft::harness.
//
// Two services, one direction each:
//   libgodot_host/p2p/out  — publisher of this peer's outbound frames
//   libgodot_host/p2p/in   — subscriber to inbound frames from peers
//
// Frames are NOT wrapped by the 8-byte request-id header (that shape
// is the lifecycle service's request/reply convention). The P2P shape
// is fire-and-forget: one byte-slice in, one byte-slice out, no
// correlation. Ordering per publisher is preserved by iceoryx2.
//
// Consumers on the godot side:
//   var bus := P2PBus.new()
//   bus.send(PackedByteArray([1, 2, 3]))
//   var frame := bus.recv()          # empty PackedByteArray if none
//
// The host drains inbound frames at each iteration() tick and emits
// the `frame_received` signal per frame — GDScript can either poll
// via recv() or connect to the signal, whichever fits.

#pragma once

#if defined(LIBGODOT_HOST_HAS_WEFT_HARNESS)

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include "iox2_api.h"
#include "weft/bus.hpp"
#include "weft/limits.hpp"

namespace libgodot_host {

// Distinct service names from the lifecycle channel — same framing, same
// underlying pub/sub type, different topic. Peers meet in the shared-memory
// registry by name only.
inline constexpr const char *P2P_OUT_SERVICE_NAME = "libgodot_host/p2p/out";
inline constexpr const char *P2P_IN_SERVICE_NAME  = "libgodot_host/p2p/in";
// One byte payload type — same as weft::PAYLOAD_TYPE, kept local so the
// P2P bus can move independently of the lifecycle service's ABI later.
inline constexpr const char *P2P_PAYLOAD_TYPE = "libgodot_host::p2p::byte";
inline constexpr std::size_t P2P_MESSAGE_BYTES = weft::limits::VALUE_BYTES;

// Host-side singleton wiring: opened once from main() before the loop.
// P2PBus reaches into these; kept as bare pointers because ownership is
// the host process (created before godot boots, destroyed after godot
// tears down). Guarded by a mutex because GDScript may call send/recv
// from a scene thread while the ITERATE tick drains inbound in parallel.
struct P2PState {
    iox2_node_h                  node       = nullptr;
    iox2_port_factory_pub_sub_h  out_service = nullptr;
    iox2_port_factory_pub_sub_h  in_service  = nullptr;
    iox2_publisher_h             publisher  = nullptr;
    iox2_subscriber_h            subscriber = nullptr;
    std::mutex                   inbox_m;
    std::vector<std::vector<uint8_t>> inbox;  // frames waiting for recv()
    bool                          ready = false;
};

// The host owns one; declared here, defined in host.cpp.
extern P2PState g_p2p;

// GDScript-callable class. Registered from the host's GDExtension init
// at MODULE_INITIALIZATION_LEVEL_SCENE so GDScript sees it as `P2PBus`.
class P2PBus : public godot::Object {
    GDCLASS(P2PBus, godot::Object)

protected:
    static void _bind_methods() {
        godot::ClassDB::bind_method(godot::D_METHOD("send", "frame"), &P2PBus::send);
        godot::ClassDB::bind_method(godot::D_METHOD("recv"), &P2PBus::recv);
        godot::ClassDB::bind_method(godot::D_METHOD("is_ready"), &P2PBus::is_ready);
    }

public:
    bool is_ready() const { return g_p2p.ready; }

    // Publish one frame on p2p/out. Silently drops when the bus is not
    // ready — GDScript should check is_ready() first if it needs to
    // distinguish setup failure from a full ringbuffer.
    void send(const godot::PackedByteArray &frame) {
        if (!g_p2p.ready) return;
        const size_t n = static_cast<size_t>(frame.size());
        if (n == 0 || n > P2P_MESSAGE_BYTES) return;
        iox2_sample_mut_h sample = nullptr;
        if (iox2_publisher_loan_slice_uninit(&g_p2p.publisher, nullptr, &sample, n) != IOX2_OK) {
            return;
        }
        void *payload = nullptr;
        size_t elements = 0;
        iox2_sample_mut_payload_mut(&sample, &payload, &elements);
        std::memcpy(payload, frame.ptr(), n);
        (void)iox2_sample_mut_send(sample, nullptr);
    }

    // Pop the oldest inbound frame from the inbox. Returns empty if none.
    // The host drains inbound at each iteration() tick — a caller that
    // needs frames as they arrive should hook `frame_received` on a
    // signal instead of polling.
    godot::PackedByteArray recv() {
        godot::PackedByteArray out;
        std::lock_guard<std::mutex> lock(g_p2p.inbox_m);
        if (g_p2p.inbox.empty()) return out;
        auto &next = g_p2p.inbox.front();
        out.resize(static_cast<int64_t>(next.size()));
        std::memcpy(out.ptrw(), next.data(), next.size());
        g_p2p.inbox.erase(g_p2p.inbox.begin());
        return out;
    }
};

// Called from host_initialize_module at the SCENE level so the class
// registers only when godot-cpp's binding is fully up.
inline void register_p2p_bus() {
    godot::ClassDB::register_class<P2PBus>();
}

// Called by the host each ITERATE tick, before invoking GodotInstance::
// iteration(). Reads any pending inbound samples into the inbox.
// Non-blocking; returns count moved.
inline int drain_p2p_inbox(std::size_t cap = 32) {
    if (!g_p2p.ready) return 0;
    int moved = 0;
    for (std::size_t i = 0; i < cap; ++i) {
        iox2_sample_h sample = nullptr;
        if (iox2_subscriber_receive(&g_p2p.subscriber, nullptr, &sample) != IOX2_OK) break;
        if (!sample) break;
        const void *payload = nullptr;
        std::size_t n = 0;
        iox2_sample_payload(&sample, &payload, &n);
        std::vector<uint8_t> frame(static_cast<const uint8_t *>(payload),
                                   static_cast<const uint8_t *>(payload) + n);
        iox2_sample_drop(sample);
        {
            std::lock_guard<std::mutex> lock(g_p2p.inbox_m);
            g_p2p.inbox.emplace_back(std::move(frame));
        }
        moved++;
    }
    return moved;
}

// Open the two P2P services on their own iox2 node. weft::run_command_loop
// owns its own node for the lifecycle service; running the P2P bus on a
// separate node keeps their event loops independent (a slow lifecycle
// tick does not stall inbound P2P frames). Two nodes in one process are
// legal per iceoryx2.
//
// Idempotent-ish: safe to call once from main(); a second call leaks and
// is not supported. Returns true on full success, false on any failure —
// a partial open leaves g_p2p.ready = false and the send/recv calls
// become no-ops.
inline bool open_p2p() {
    if (!weft::load_bus()) return false;

    if (iox2_node_builder_create(iox2_node_builder_new(nullptr), nullptr,
                iox2_service_type_e_IPC, &g_p2p.node) != IOX2_OK) {
        return false;
    }

    auto open_service = [&](const char *name) -> iox2_port_factory_pub_sub_h {
        iox2_service_name_h svc_name = nullptr;
        if (iox2_service_name_new(nullptr, name, std::strlen(name), &svc_name) != IOX2_OK) {
            return nullptr;
        }
        auto builder = iox2_service_builder_pub_sub(
                iox2_node_service_builder(&g_p2p.node, nullptr, iox2_cast_service_name_ptr(svc_name)));
        if (iox2_service_builder_pub_sub_set_payload_type_details(&builder,
                    iox2_type_variant_e_DYNAMIC, P2P_PAYLOAD_TYPE, std::strlen(P2P_PAYLOAD_TYPE),
                    1, 1) != IOX2_OK) {
            iox2_service_name_drop(svc_name);
            return nullptr;
        }
        iox2_port_factory_pub_sub_h service = nullptr;
        const int rc = iox2_service_builder_pub_sub_open_or_create(builder, nullptr, &service);
        iox2_service_name_drop(svc_name);
        return rc == IOX2_OK ? service : nullptr;
    };

    g_p2p.out_service = open_service(P2P_OUT_SERVICE_NAME);
    g_p2p.in_service  = open_service(P2P_IN_SERVICE_NAME);
    if (!g_p2p.out_service || !g_p2p.in_service) return false;

    auto pub_builder = iox2_port_factory_pub_sub_publisher_builder(&g_p2p.out_service, nullptr);
    iox2_port_factory_publisher_builder_set_initial_max_slice_len(&pub_builder, P2P_MESSAGE_BYTES);
    if (iox2_port_factory_publisher_builder_create(pub_builder, nullptr, &g_p2p.publisher) != IOX2_OK) {
        return false;
    }
    if (iox2_port_factory_subscriber_builder_create(
                iox2_port_factory_pub_sub_subscriber_builder(&g_p2p.in_service, nullptr), nullptr,
                &g_p2p.subscriber) != IOX2_OK) {
        return false;
    }

    g_p2p.ready = true;
    return true;
}

} // namespace libgodot_host

#endif // LIBGODOT_HOST_HAS_WEFT_HARNESS
