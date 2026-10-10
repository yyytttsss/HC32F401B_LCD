#pragma once

#include "iap_protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace usb_upper {

using Bytes = iap::Bytes;
constexpr std::uint16_t kVid = iap::kVid;
constexpr std::uint16_t kPid = iap::kPid;
constexpr std::size_t kReportSize = iap::kReportSize;

// Cross-platform HID transport.  The protocol layer does not depend on the
// operating system; hidapi supplies the Windows/macOS/Linux backend.
class HidTransport {
public:
    HidTransport() = default;
    ~HidTransport();
    HidTransport(const HidTransport&) = delete;
    HidTransport& operator=(const HidTransport&) = delete;

    static std::vector<std::string> enumerate_paths(
        std::uint16_t vid = kVid, std::uint16_t pid = kPid);
    bool open(std::uint16_t vid = kVid, std::uint16_t pid = kPid);
    void close();
    bool is_open() const;
    bool write(const Bytes& payload);
    bool read(Bytes* payload, std::uint32_t timeout_ms);

private:
    void* handle_ = nullptr;
    std::uint32_t input_report_length_ = kReportSize + 1U;
    std::uint32_t output_report_length_ = kReportSize + 1U;
};

}  // namespace usb_upper
