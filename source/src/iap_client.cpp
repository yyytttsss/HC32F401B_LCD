#include "iap_client.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace usb_upper::iap {
namespace {

constexpr std::uint32_t kConnectTimeoutMs = 1000;
constexpr std::uint32_t kHandshakeTimeoutMs = 1000;
constexpr std::uint32_t kDataWriteTimeoutMs = 3000;
constexpr std::uint32_t kDataReadTimeoutMs = 1500;

std::uint32_t remaining_ms(std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    return static_cast<std::uint32_t>(std::max<std::int64_t>(1, remaining));
}

void log_line(const Client::LogCallback& callback, const std::string& line) {
    if (callback) callback(line);
}

}  // namespace

Client::Client(HidTransport* transport) : transport_(transport) {
    if (transport_ == nullptr) throw ProtocolError("HID transport is null");
}

bool Client::cancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load();
}

void Client::send_frame(const Bytes& frame) {
    if (frame.empty()) throw ProtocolError("cannot send empty frame");
    for (std::size_t offset = 0; offset < frame.size(); offset += kReportSize) {
        const std::size_t count = std::min<std::size_t>(
            kReportSize, frame.size() - offset);
        Bytes chunk(frame.begin() + offset, frame.begin() + offset + count);
        bool ok = false;
        for (int attempt = 0; attempt < 5 && !ok; ++attempt) {
            ok = transport_->write(chunk);
            if (!ok) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(2 + attempt * 3));
            }
        }
        if (!ok) {
            throw ProtocolError(offset == 0U ? "HID send failed" :
                                               "HID partial send failed");
        }
    }
}

Bytes Client::read_frame(std::uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    Bytes frame;
    std::size_t expected = 0U;
    while (std::chrono::steady_clock::now() < deadline) {
        Bytes report;
        if (!transport_->read(&report, remaining_ms(deadline))) {
            if (!transport_->is_open()) throw ProtocolError("device disconnected");
            continue;
        }
        if (report.size() < 3U) continue;
        if (frame.empty()) {
            const auto declared = declared_frame_size(report);
            if (!declared.has_value() || *declared > 0xFFFFU) continue;
            expected = *declared;
            if (expected > 0xFFFFU) throw ProtocolError("invalid frame length");
        }
        const std::size_t needed = expected - frame.size();
        const std::size_t count = std::min(needed, report.size());
        frame.insert(frame.end(), report.begin(), report.begin() + count);
        if (frame.size() == expected) return frame;
    }
    throw ProtocolError("device response timeout");
}

Response Client::wait_response(std::uint8_t request_command,
                                std::uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto response = parse_response(read_frame(remaining_ms(deadline)));
        if (response.has_value() &&
            response->request_command == request_command) {
            return *response;
        }
    }
    throw ProtocolError("unexpected device response");
}

Response Client::exchange(const Bytes& request, std::uint8_t request_command,
                          std::uint32_t timeout_ms) {
    send_frame(request);
    return wait_response(request_command, timeout_ms);
}

void Client::require_ok(const Response& response,
                        std::uint8_t request_command) {
    if (response.request_command != request_command ||
        response.status != kStatusOk || !response.payload.empty()) {
        std::ostringstream message;
        message << "Boot rejected command 0x" << std::hex
                << static_cast<unsigned>(request_command);
        throw ProtocolError(message.str());
    }
}

DeviceInfo Client::connect(std::uint32_t timeout_ms) {
    if (!transport_->is_open() && !transport_->open(kVid, kPid)) {
        throw ProtocolError("HID device not found");
    }
    const Response response = exchange(make_connect_request(), kCmdConnect,
                                       timeout_ms == 0U ? kConnectTimeoutMs : timeout_ms);
    return decode_device_info(response);
}

void Client::enter_boot(std::uint32_t timeout_ms) {
    if (!transport_->is_open() && !transport_->open(kVid, kPid)) {
        throw ProtocolError("HID device not found");
    }
    const Response response = exchange(make_app_enter_boot_request(),
                                       kCmdHandshake, timeout_ms);
    require_ok(response, kCmdHandshake);
    transport_->close();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void Client::write_jpg(std::uint32_t address, const Bytes& data,
                        const DeviceInfo& info, LogCallback log,
                        ProgressCallback progress,
                        const std::atomic_bool* cancel) {
    if (data.empty() || info.max_frame_size < 10U) {
        throw ProtocolError("invalid JPG write or device capacity");
    }
    const std::size_t payload_max = info.max_frame_size - 9U;
    std::size_t offset = 0U;
    while (offset < data.size()) {
        if (cancelled(cancel)) throw ProtocolError("write cancelled");
        const std::size_t count = std::min(payload_max, data.size() - offset);
        const Bytes chunk(data.begin() + offset, data.begin() + offset + count);
        require_ok(exchange(make_write_data_request(
                                address + static_cast<std::uint32_t>(offset), chunk),
                            kCmdWriteData, kDataWriteTimeoutMs),
                   kCmdWriteData);
        offset += count;
        if (progress) progress(offset, data.size());
        log_line(log, "JPG 写入 " + std::to_string(offset) + "/" +
                       std::to_string(data.size()));
    }
}

Bytes Client::read_jpg(std::uint32_t address, std::size_t length,
                       const DeviceInfo& info, std::uint32_t timeout_ms) {
    if (length == 0U) return {};
    if (!transport_->is_open()) throw ProtocolError("HID device is closed");
    if (info.max_frame_size < 8U) {
        throw ProtocolError("invalid device frame capacity");
    }
    const std::size_t max_payload = info.max_frame_size - 7U;
    Bytes result;
    const auto effective_timeout = timeout_ms == 0U ? kDataReadTimeoutMs : timeout_ms;
    while (result.size() < length) {
        const std::size_t count = std::min(max_payload, length - result.size());
        const Response response = exchange(
            make_read_data_request(address + static_cast<std::uint32_t>(result.size()),
                                   static_cast<std::uint32_t>(count)),
            kCmdReadData, effective_timeout);
        if (response.status != kStatusOk || response.payload.size() != count) {
            throw ProtocolError("JPG read failed");
        }
        result.insert(result.end(), response.payload.begin(), response.payload.end());
    }
    return result;
}

void Client::return_app(std::uint32_t timeout_ms) {
    send_frame(make_return_app_request());
    try {
        const std::uint32_t response_window =
            std::min<std::uint32_t>(timeout_ms, 300U);
        const Response response = wait_response(kCmdReturnApp, response_window);
        require_ok(response, kCmdReturnApp);
        return;
    } catch (const ProtocolError& error) {
        const std::string message = error.what();
        if ((message != "device response timeout") &&
            (message != "device disconnected")) {
            throw;
        }
        transport_->close();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        bool disconnected = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const bool present = !HidTransport::enumerate_paths(kVid, kPid).empty();
            if (!present) {
                disconnected = true;
            } else if (disconnected) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        throw;
    }
}

}  // namespace usb_upper::iap
