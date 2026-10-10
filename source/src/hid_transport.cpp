#include "hid_transport.hpp"

#include <hidapi.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace usb_upper {
namespace {

void ensure_hidapi_initialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        // hidapi is process-global and remains initialized for the lifetime
        // of the upper computer.  This avoids one transport instance ending
        // a session still used by another GUI worker.
        (void)hid_init();
    });
}

hid_device* as_device(void* handle) {
    return static_cast<hid_device*>(handle);
}

}  // namespace

HidTransport::~HidTransport() { close(); }

std::vector<std::string> HidTransport::enumerate_paths(std::uint16_t vid,
                                                        std::uint16_t pid) {
    ensure_hidapi_initialized();
    std::vector<std::string> paths;
    hid_device_info* devices = hid_enumerate(vid, pid);
    for (hid_device_info* item = devices; item != nullptr; item = item->next) {
        if (item->path != nullptr) paths.emplace_back(item->path);
    }
    hid_free_enumeration(devices);
    return paths;
}

bool HidTransport::open(std::uint16_t vid, std::uint16_t pid) {
    ensure_hidapi_initialized();
    close();
    for (const auto& path : enumerate_paths(vid, pid)) {
        hid_device* device = hid_open_path(path.c_str());
        if (device == nullptr) continue;

        // The current HID descriptor is 64 bytes plus the report-ID byte.
        // Keep the value explicit because hidapi intentionally exposes the
        // report lengths through the descriptor-specific backends only.
        handle_ = device;
        input_report_length_ = kReportSize + 1U;
        output_report_length_ = kReportSize + 1U;
        return true;
    }
    return false;
}

void HidTransport::close() {
    if (handle_ != nullptr) {
        hid_close(as_device(handle_));
        handle_ = nullptr;
    }
}

bool HidTransport::is_open() const { return handle_ != nullptr; }

bool HidTransport::write(const Bytes& payload) {
    if (!is_open() || payload.size() > kReportSize) return false;

    std::vector<unsigned char> report(output_report_length_, 0U);
    const std::size_t count = std::min<std::size_t>(
        payload.size(), output_report_length_ - 1U);
    std::copy_n(payload.begin(), count, report.begin() + 1);
    const int written = hid_write(as_device(handle_), report.data(),
                                  report.size());
    return written > 0;
}

bool HidTransport::read(Bytes* payload, std::uint32_t timeout_ms) {
    if (payload == nullptr || !is_open()) return false;

    std::vector<unsigned char> report(input_report_length_, 0U);
    const int received = hid_read_timeout(as_device(handle_), report.data(),
                                          report.size(),
                                          static_cast<int>(timeout_ms));
    if (received <= 0) return false;

    const std::size_t start = (received == static_cast<int>(kReportSize + 1U) &&
                               report[0] == 0U) ? 1U : 0U;
    const std::size_t count = std::min<std::size_t>(
        kReportSize, static_cast<std::size_t>(received) - start);
    payload->assign(report.begin() + start, report.begin() + start + count);
    return true;
}

}  // namespace usb_upper
