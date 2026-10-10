#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace usb_upper::iap {

using Bytes = std::vector<std::uint8_t>;
using Uid = std::array<std::uint8_t, 12>;
using SerialNumber = std::array<std::uint8_t, 16>;
using Nonce = std::array<std::uint8_t, 8>;

constexpr std::uint16_t kVid = 0x2E3C;
constexpr std::uint16_t kPid = 0x0001;
constexpr std::size_t kReportSize = 64;
constexpr std::size_t kParameterRecordSize = 36;
constexpr std::size_t kParameterRecordSizeLegacy = 28;
constexpr std::size_t kConnectPayloadSize = 42;
constexpr std::size_t kConnectPayloadSizeLegacy = 34;

constexpr std::uint8_t kCmdConnect = 0x01;
constexpr std::uint8_t kCmdHandshake = 0x02;
constexpr std::uint8_t kCmdProgramPacket = 0x03;
constexpr std::uint8_t kCmdReadParameter = 0x04;
constexpr std::uint8_t kCmdWriteParameter = 0x05;
constexpr std::uint8_t kCmdReadData = 0x06;
constexpr std::uint8_t kCmdWriteData = 0x07;
constexpr std::uint8_t kCmdReturnApp = 0x08;
constexpr std::uint8_t kCmdResponse = 0x80;

constexpr std::uint8_t kStatusFail = 0x00;
constexpr std::uint8_t kStatusOk = 0x01;
constexpr std::uint8_t kTargetApp = 0x01;
constexpr std::uint8_t kTargetJpg = 0x02;

/* ---- Flash 分区地址（与 Bootloader/Config/Boot_HC32F401B.h 一致） -------*/
constexpr std::uint32_t kAppBase = 0x08003000U;
constexpr std::uint32_t kAppSize = 0x00020000U;
constexpr std::uint32_t kParameterBase = 0x08023000U;
constexpr std::uint32_t kParameterRegionSize = 0x00001000U;
constexpr std::uint32_t kJpgBase = 0x08024000U;
constexpr std::uint32_t kJpgSize = 0x00007000U;
constexpr std::uint32_t kResBaseLegacy = 0x08023000U;
constexpr std::uint32_t kResSizeLegacy = 0x00014000U;
constexpr std::uint32_t kKeyRingBase = 0x0802B000U;
constexpr std::uint32_t kKeyRingSize = 0x00008000U;
constexpr std::uint32_t kUserDataBase = 0x08033000U;
constexpr std::uint32_t kUserDataSize = 0x0000A000U;

constexpr std::array<std::uint8_t, 8> kConnectionKey{
    0x16, 0x46, 0xF5, 0x37, 0x90, 0x70, 0x80, 0x19};

struct ProtocolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Response {
    std::uint8_t request_command = 0;
    std::uint8_t status = kStatusFail;
    Bytes payload;
};

/* 01 连接响应中的设备信息。身份字段 28B = SN(16) + 型号(4) + 硬件版本(4) + 固件版本(4)。 */
struct DeviceInfo {
    SerialNumber serial_number{};
    std::uint32_t model = 0;
    std::uint32_t hardware_version = 0;
    std::uint32_t firmware_version = 0;
    Uid uid{};
    std::uint16_t max_frame_size = 0;
    bool legacy_format = false; /* 旧版 Boot（34B 连接响应 + 28B 参数），否则新版（42B + 36B）。 */
};

/* 36B 参数记录，大端字节序。CRC16 校验前 34B。 */
struct ParameterRecord {
    SerialNumber serial_number{};
    std::uint32_t model = 0;
    std::uint32_t hardware_version = 0;
    std::uint32_t firmware_version = 0;
    std::uint32_t program_length = 0;
    std::uint16_t program_crc16 = 0;
    std::uint16_t parameter_crc16 = 0;
};

struct Image {
    std::uint8_t target = 0;
    Bytes data;
    Nonce nonce{};
    std::uint16_t program_crc16 = 0;
    std::uint32_t firmware_version = 0;
};

std::uint16_t crc16(const std::uint8_t* data, std::size_t size);
std::uint16_t crc16(const Bytes& data);

std::uint16_t read_be16(const std::uint8_t* p);
std::uint32_t read_be32(const std::uint8_t* p);
void write_be16(std::uint8_t* p, std::uint16_t value);
void write_be32(std::uint8_t* p, std::uint32_t value);

Bytes make_request(std::uint8_t command, const Bytes& data = {});
std::optional<Response> parse_response(const Bytes& frame);
std::optional<std::size_t> declared_frame_size(const Bytes& partial);

Bytes xtea_encrypt_block(const Uid& uid, const Nonce& block);
Bytes xtea_decrypt_block(const Uid& uid, const Bytes& block);
Bytes xtea_ctr_xor(const Uid& uid, const Nonce& nonce,
                  std::size_t offset, const Bytes& data);

Bytes make_connect_request();
Bytes make_app_enter_boot_request();
Bytes make_handshake_request(const Uid& uid, const Nonce& nonce,
                             std::uint16_t packet_count);
Bytes make_program_packet(std::uint16_t packet_number,
                          const Bytes& encrypted_data);
Bytes make_read_parameter_request();
Bytes make_write_parameter_request(const ParameterRecord& record, bool legacy = false);
Bytes make_read_data_request(std::uint32_t address, std::uint32_t length);
Bytes make_write_data_request(std::uint32_t address, const Bytes& data);
Bytes make_return_app_request();

DeviceInfo decode_device_info(const Response& response);
ParameterRecord decode_parameter(const Bytes& bytes);
Bytes encode_parameter(const ParameterRecord& record);

ParameterRecord make_local_parameter(const DeviceInfo& info,
                                     std::uint32_t model,
                                     std::uint32_t hardware_version,
                                     std::uint32_t firmware_version,
                                     const SerialNumber& serial_number,
                                     std::uint32_t program_length,
                                     std::uint16_t program_crc16);

bool validate_app_image(const Bytes& data, std::string* error = nullptr);
std::string uid_hex(const Uid& uid);
std::string serial_hex(const SerialNumber& serial_number);

/* hardware_version 最高位表示屏幕方向：0=横屏, 1=竖屏。低 31 位为版本号。 */
constexpr std::uint32_t kHardwarePortraitMask = 0x80000000U;
inline bool is_landscape(std::uint32_t hardware_version) {
    return (hardware_version & kHardwarePortraitMask) == 0U;
}
inline std::uint32_t hardware_version_number(std::uint32_t hardware_version) {
    return hardware_version & ~kHardwarePortraitMask;
}

/* 根据 model 和 hardware_version 生成设备显示名称，例如 "1号 V1.0" 或 "1号竖屏 V1.2"。 */
std::string format_device_name(std::uint32_t model, std::uint32_t hardware_version);

}  // namespace usb_upper::iap
