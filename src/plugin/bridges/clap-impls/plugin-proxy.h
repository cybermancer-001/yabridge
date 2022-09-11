// yabridge: a Wine plugin bridge
// Copyright (C) 2020-2022 Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <future>
#include <vector>

#include <clap/plugin.h>
#include <rigtorp/MPMCQueue.h>
#include <function2/function2.hpp>

#include "../../common/serialization/clap/plugin.h"

// Forward declaration to avoid circular includes
class ClapPluginBridge;

/**
 * A proxy for a `clap_plugin`.
 */
class clap_plugin_proxy {
   public:
    /**
     * A function that can be called on the host's main thread through a
     * combination of `clap_host::request_callback()` and
     * `clap_plugin::on_main_thread()`. This is identical to
     * `fu2::unique_function<void()>` except that it doesn't throw and is
     * noexcept move assignable. That is a requirement for using these in the
     * MPMC queue.
     */
    using HostCallback = fu2::
        function_base<true, false, fu2::capacity_default, false, true, void()>;

    /**
     * Construct a proxy for a plugin that has already been created on the Wine
     * side. This is done in our `clap_plugin_factory::create()` implementation.
     * The instance ID lets us link calls the host makes on a plugin object to a
     * Windows CLAP plugin running under the Wine plugin host.
     */
    clap_plugin_proxy(ClapPluginBridge& bridge,
                      size_t instance_id,
                      clap::plugin::Descriptor descriptor,
                      const clap_host_t* host);

    clap_plugin_proxy(const clap_plugin_proxy&) = delete;
    clap_plugin_proxy& operator=(const clap_plugin_proxy&) = delete;
    clap_plugin_proxy(clap_plugin_proxy&&) = delete;
    clap_plugin_proxy& operator=(clap_plugin_proxy&&) = delete;

    /**
     * Get a `clap_plugin` vtable that can be passed to the host when creating a
     * plugin instance.
     */
    inline const clap_plugin_t* plugin_vtable() const {
        return &plugin_vtable_;
    }

    /**
     * The instance ID of the plugin instance this proxy belongs to.
     */
    inline size_t instance_id() const { return instance_id_; }

    static bool CLAP_ABI plugin_init(const struct clap_plugin* plugin);
    static void CLAP_ABI plugin_destroy(const struct clap_plugin* plugin);
    static bool CLAP_ABI plugin_activate(const struct clap_plugin* plugin,
                                         double sample_rate,
                                         uint32_t min_frames_count,
                                         uint32_t max_frames_count);
    static void CLAP_ABI plugin_deactivate(const struct clap_plugin* plugin);
    static bool CLAP_ABI
    plugin_start_processing(const struct clap_plugin* plugin);
    static void CLAP_ABI
    plugin_stop_processing(const struct clap_plugin* plugin);
    static void CLAP_ABI plugin_reset(const struct clap_plugin* plugin);
    static clap_process_status CLAP_ABI
    plugin_process(const struct clap_plugin* plugin,
                   const clap_process_t* process);
    static const void* CLAP_ABI
    plugin_get_extension(const struct clap_plugin* plugin, const char* id);
    static void CLAP_ABI
    plugin_on_main_thread(const struct clap_plugin* plugin);

    /**
     * Asynchronously run a function on the host's main thread, returning the
     * result as a future.
     */
    template <std::invocable F>
    std::future<std::invoke_result_t<F>> run_on_main_thread(F&& fn) {
        using Result = std::invoke_result_t<F>;

        std::promise<Result> response_promise{};
        std::future<Result> response_future = response_promise.get_future();
        pending_callbacks_.push(HostCallback(
            [fn = std::forward<F>(fn),
             response_promise = std::move(response_promise)]() mutable {
                if constexpr (std::is_void_v<Result>) {
                    fn();
                    response_promise.set_value();
                } else {
                    response_promise.set_value(fn());
                }
            }));

        // In theory the host will now call `clap_plugin::on_main_thread()`,
        // where we can pop the function from the queue
        host_->request_callback(host_);

        return response_future;
    }

    /**
     * The `clap_host_t*` passed when creating the instance. Any callbacks made
     * by the proxied plugin instance must go through ere.
     */
    const clap_host_t* host_;

   private:
    ClapPluginBridge& bridge_;
    size_t instance_id_;
    clap::plugin::Descriptor descriptor_;

    /**
     * The vtable for `clap_plugin`, requires that this object is never moved or
     * copied. We'll use the host data pointer instead of placing this vtable at
     * the start of the struct and directly casting the `clap_plugin_t*`.
     */
    const clap_plugin_t plugin_vtable_;

    /**
     * The extensions supported by the bridged plugin. Set after a successful
     * `clap_plugin::init()` call. We'll allow the host to query these same
     * extensions from our plugin proxy.
     */
    clap::plugin::SupportedPluginExtensions supported_extensions_;

    /**
     * Pending callbacks that must be sent to the host on the main thread. If a
     * socket needs to make a main thread function call, it will
     */
    rigtorp::MPMCQueue<HostCallback> pending_callbacks_;
};