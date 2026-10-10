#pragma once

#include "hid_transport.hpp"
#include "iap_protocol.hpp"

#include <atomic>
#include <cstdint>
#include <functional>

namespace usb_upper::iap {

class Client {
public:
    using LogCallback = std::function<void(const std::string&)>;
    using ProgressCallback = std::function<void(std::size_t, std::size_t)>;

    explicit Client(HidTransport* transport);

    DeviceInfo connect(std::uint32_t timeout_ms = 1000);
    void enter_boot(std::uint32_t timeout_ms = 3000);

    /* JPG 读写：07 写入 JPG 区，06 读取 JPG 区。 */
    void write_jpg(std::uint32_t address, const Bytes& data,
                   const DeviceInfo& info,
                   LogCallback log = {}, ProgressCallback progress = {},
                   const std::atomic_bool* cancel = nullptr);
    Bytes read_jpg(std::uint32_t address, std::size_t length,
                   const DeviceInfo& info,
                   std::uint32_t timeout_ms = 1500);

    void return_app(std::uint32_t timeout_ms = 3000);

private:
    Bytes read_frame(std::uint32_t timeout_ms);
    void send_frame(const Bytes& frame);
    Response wait_response(std::uint8_t request_command,
                           std::uint32_t timeout_ms);
    Response exchange(const Bytes& request, std::uint8_t request_command,
                      std::uint32_t timeout_ms);
    static void require_ok(const Response& response,
                           std::uint8_t request_command);
    static bool cancelled(const std::atomic_bool* cancel);

    HidTransport* transport_ = nullptr;
};

}  // namespace usb_upper::iap
