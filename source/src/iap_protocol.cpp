#include "iap_protocol.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace usb_upper::iap {
namespace {

constexpr std::uint32_t kDelta = 0x9E3779B9U;

std::array<std::uint32_t, 4> make_key(const Uid& uid) {
    return {read_be32(uid.data()), read_be32(uid.data() + 4U),
            read_be32(uid.data() + 8U), 0U};
}

void xtea_encrypt_words(std::uint32_t& v0, std::uint32_t& v1,
                        const std::array<std::uint32_t, 4>& key) {
    std::uint32_t sum = 0U;
    for (unsigned round = 0; round < 32U; ++round) {
        v0 += ((((v1 << 4U) ^ (v1 >> 5U)) + v1) ^
               (sum + key[sum & 3U]));
        sum += kDelta;
        v1 += ((((v0 << 4U) ^ (v0 >> 5U)) + v0) ^
               (sum + key[(sum >> 11U) & 3U]));
    }
}

void xtea_decrypt_words(std::uint32_t& v0, std::uint32_t& v1,
                        const std::array<std::uint32_t, 4>& key) {
    std::uint32_t sum = 0xC6EF3720U;
    for (unsigned round = 0; round < 32U; ++round) {
        v1 -= ((((v0 << 4U) ^ (v0 >> 5U)) + v0) ^
               (sum + key[(sum >> 11U) & 3U]));
        sum -= kDelta;
        v0 -= ((((v1 << 4U) ^ (v1 >> 5U)) + v1) ^
               (sum + key[sum & 3U]));
    }
}

std::string bytes_hex(const std::uint8_t* data, std::size_t size) {
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        stream << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return stream.str();
}

std::uint32_t read_le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

}  // namespace

std::uint16_t read_be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8U) |
                                      p[1]);
}

std::uint32_t read_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24U) |
           (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) |
           p[3];
}

void write_be16(std::uint8_t* p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value >> 8U);
    p[1] = static_cast<std::uint8_t>(value);
}

void write_be32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value >> 24U);
    p[1] = static_cast<std::uint8_t>(value >> 16U);
    p[2] = static_cast<std::uint8_t>(value >> 8U);
    p[3] = static_cast<std::uint8_t>(value);
}

std::uint16_t crc16(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0xFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8U;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            crc = (crc & 0x8000U) != 0U ?
                static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U) :
                static_cast<std::uint16_t>(crc << 1U);
        }
    }
    return crc;
}

std::uint16_t crc16(const Bytes& data) {
    return crc16(data.data(), data.size());
}

Bytes make_request(std::uint8_t command, const Bytes& data) {
    const std::size_t total = 5U + data.size();
    if (total > 0xFFFFU) throw ProtocolError("request is too large");
    Bytes frame(total);
    frame[0] = command;
    write_be16(&frame[1], static_cast<std::uint16_t>(total));
    std::copy(data.begin(), data.end(), frame.begin() + 3);
    write_be16(&frame[total - 2U], crc16(frame.data(), total - 2U));
    return frame;
}

std::optional<std::size_t> declared_frame_size(const Bytes& partial) {
    if (partial.size() < 3U) return std::nullopt;
    const std::size_t size = read_be16(&partial[1]);
    if (size < 5U) return std::nullopt;
    return size;
}

std::optional<Response> parse_response(const Bytes& frame) {
    if (frame.size() < 7U || frame[0] != kCmdResponse) return std::nullopt;
    const std::size_t total = read_be16(&frame[1]);
    if (total != frame.size() || total < 7U) return std::nullopt;
    if (crc16(frame.data(), total - 2U) != read_be16(&frame[total - 2U])) {
        return std::nullopt;
    }
    Response response;
    response.request_command = frame[3];
    response.status = frame[4];
    response.payload.assign(frame.begin() + 5, frame.end() - 2);
    return response;
}

Bytes xtea_encrypt_block(const Uid& uid, const Nonce& block) {
    std::uint32_t v0 = read_be32(block.data());
    std::uint32_t v1 = read_be32(block.data() + 4U);
    xtea_encrypt_words(v0, v1, make_key(uid));
    Bytes output(8U);
    write_be32(output.data(), v0);
    write_be32(output.data() + 4U, v1);
    return output;
}

Bytes xtea_decrypt_block(const Uid& uid, const Bytes& block) {
    if (block.size() != 8U) throw ProtocolError("XTEA block must be 8 bytes");
    std::uint32_t v0 = read_be32(block.data());
    std::uint32_t v1 = read_be32(block.data() + 4U);
    xtea_decrypt_words(v0, v1, make_key(uid));
    Bytes output(8U);
    write_be32(output.data(), v0);
    write_be32(output.data() + 4U, v1);
    return output;
}

Bytes xtea_ctr_xor(const Uid& uid, const Nonce& nonce,
                  std::size_t offset, const Bytes& data) {
    Bytes result = data;
    std::uint64_t counter = 0U;
    for (std::uint8_t value : nonce) counter = (counter << 8U) | value;
    counter += offset / 8U;
    std::size_t stream_offset = offset % 8U;
    std::size_t cursor = 0U;
    while (cursor < result.size()) {
        Nonce counter_bytes{};
        for (std::size_t i = 0; i < counter_bytes.size(); ++i) {
            counter_bytes[i] = static_cast<std::uint8_t>(
                counter >> (8U * (counter_bytes.size() - 1U - i)));
        }
        const Bytes stream = xtea_encrypt_block(uid, counter_bytes);
        const std::size_t count = std::min<std::size_t>(
            8U - stream_offset, result.size() - cursor);
        for (std::size_t i = 0; i < count; ++i) {
            result[cursor + i] ^= stream[stream_offset + i];
        }
        cursor += count;
        ++counter;
        stream_offset = 0U;
    }
    return result;
}

Bytes make_connect_request() {
    return make_request(kCmdConnect,
                        Bytes(kConnectionKey.begin(), kConnectionKey.end()));
}

Bytes make_app_enter_boot_request() {
    return make_request(kCmdHandshake);
}

Bytes make_handshake_request(const Uid& uid, const Nonce& nonce,
                             std::uint16_t packet_count) {
    Bytes data(nonce.begin(), nonce.end());
    const Bytes encrypted = xtea_encrypt_block(uid, nonce);
    data.insert(data.end(), encrypted.begin(), encrypted.end());
    data.push_back(static_cast<std::uint8_t>(packet_count >> 8U));
    data.push_back(static_cast<std::uint8_t>(packet_count));
    return make_request(kCmdHandshake, data);
}

Bytes make_program_packet(std::uint16_t packet_number,
                          const Bytes& encrypted_data) {
    Bytes data(2U);
    write_be16(data.data(), packet_number);
    data.insert(data.end(), encrypted_data.begin(), encrypted_data.end());
    return make_request(kCmdProgramPacket, data);
}

Bytes make_read_parameter_request() {
    return make_request(kCmdReadParameter);
}

/* 参数记录 36B：SN(16) + 型号(4) + 硬件版本(4) + 固件版本(4) +
 * App长度(4) + App CRC16(2) + 参数 CRC16(2，校验前 34B)。 */
Bytes encode_parameter(const ParameterRecord& record) {
    Bytes result(kParameterRecordSize, 0U);
    std::copy(record.serial_number.begin(), record.serial_number.end(),
              result.begin());
    write_be32(&result[16], record.model);
    write_be32(&result[20], record.hardware_version);
    write_be32(&result[24], record.firmware_version);
    write_be32(&result[28], record.program_length);
    write_be16(&result[32], record.program_crc16);
    const std::uint16_t param_crc = crc16(result.data(), 34U);
    write_be16(&result[34], param_crc);
    return result;
}

/* 参数记录：新版 36B (SN16+型号4+硬件版本4+固件版本4+App长度4+AppCRC16+参数CRC16)，
 * 旧版 28B (硬件ID4+软件ID4+设备ID4+SN8+App长度4+AppCRC16+SN_CRC16)。自动兼容。 */
ParameterRecord decode_parameter(const Bytes& bytes) {
    ParameterRecord result;
    if (bytes.size() == kParameterRecordSize) {
        std::copy_n(bytes.begin(), result.serial_number.size(),
                    result.serial_number.begin());
        result.model = read_be32(&bytes[16]);
        result.hardware_version = read_be32(&bytes[20]);
        result.firmware_version = read_be32(&bytes[24]);
        result.program_length = read_be32(&bytes[28]);
        result.program_crc16 = read_be16(&bytes[32]);
        result.parameter_crc16 = read_be16(&bytes[34]);
    } else if (bytes.size() == kParameterRecordSizeLegacy) {
        result.hardware_version = read_be32(&bytes[0]);
        result.firmware_version = read_be32(&bytes[4]);
        result.model = read_be32(&bytes[8]);
        std::copy_n(bytes.begin() + 12, 8U,
                    result.serial_number.begin());
        result.program_length = read_be32(&bytes[20]);
        result.program_crc16 = read_be16(&bytes[24]);
        result.parameter_crc16 = read_be16(&bytes[26]);
    } else {
        throw ProtocolError("parameter record size mismatch");
    }
    return result;
}

/* 旧版 28B 参数：硬件ID4+软件ID4+设备ID4+SN8+App长度4+AppCRC16+SNCRC16。
 * 新版 36B 参数：SN16+型号4+硬件版本4+固件版本4+App长度4+AppCRC16+参数CRC16。 */
Bytes make_write_parameter_request(const ParameterRecord& record, bool legacy) {
    if (legacy) {
        Bytes result(kParameterRecordSizeLegacy, 0U);
        write_be32(&result[0], record.hardware_version);
        write_be32(&result[4], record.firmware_version);
        write_be32(&result[8], record.model);
        std::copy_n(record.serial_number.begin(), 8U, result.begin() + 12);
        write_be32(&result[20], record.program_length);
        write_be16(&result[24], record.program_crc16);
        const std::uint16_t sn_crc = crc16(result.data() + 12, 8U);
        write_be16(&result[26], sn_crc);
        return make_request(kCmdWriteParameter, result);
    }
    return make_request(kCmdWriteParameter, encode_parameter(record));
}

Bytes make_read_data_request(std::uint32_t address, std::uint32_t length) {
    Bytes data(8U);
    write_be32(&data[0], address);
    write_be32(&data[4], length);
    return make_request(kCmdReadData, data);
}

Bytes make_write_data_request(std::uint32_t address, const Bytes& data) {
    Bytes payload(4U);
    write_be32(payload.data(), address);
    payload.insert(payload.end(), data.begin(), data.end());
    return make_request(kCmdWriteData, payload);
}

Bytes make_return_app_request() {
    return make_request(kCmdReturnApp);
}

/* 01 连接响应：新版 42B (SN16+型号4+硬件版本4+固件版本4+UID12+容量2)，
 * 旧版 34B (硬件ID4+软件ID4+设备ID4+SN8+UID12+容量2)。自动兼容。 */
DeviceInfo decode_device_info(const Response& response) {
    if (response.request_command != kCmdConnect ||
        response.status != kStatusOk) {
        throw ProtocolError("invalid device connection response");
    }
    const std::size_t size = response.payload.size();
    DeviceInfo info;
    if (size == kConnectPayloadSize) {
        std::copy_n(response.payload.begin(), info.serial_number.size(),
                    info.serial_number.begin());
        info.model = read_be32(&response.payload[16]);
        info.hardware_version = read_be32(&response.payload[20]);
        info.firmware_version = read_be32(&response.payload[24]);
        std::copy_n(response.payload.begin() + 28, info.uid.size(),
                    info.uid.begin());
        info.max_frame_size = read_be16(&response.payload[40]);
    } else if (size == kConnectPayloadSizeLegacy) {
        info.legacy_format = true;
        info.hardware_version = read_be32(&response.payload[0]);
        info.firmware_version = read_be32(&response.payload[4]);
        info.model = read_be32(&response.payload[8]);
        std::copy_n(response.payload.begin() + 12, 8U,
                    info.serial_number.begin());
        std::copy_n(response.payload.begin() + 20, info.uid.size(),
                    info.uid.begin());
        info.max_frame_size = read_be16(&response.payload[32]);
    } else {
        throw ProtocolError("invalid device connection response");
    }
    return info;
}

ParameterRecord make_local_parameter(const DeviceInfo& info,
                                     std::uint32_t model,
                                     std::uint32_t hardware_version,
                                     std::uint32_t firmware_version,
                                     const SerialNumber& serial_number,
                                     std::uint32_t program_length,
                                     std::uint16_t program_crc16) {
    (void)info;
    ParameterRecord record;
    record.serial_number = serial_number;
    record.model = model;
    record.hardware_version = hardware_version;
    record.firmware_version = firmware_version;
    record.program_length = program_length;
    record.program_crc16 = program_crc16;
    /* 参数 CRC16 校验前 34B，在 encode_parameter 中自动计算。 */
    Bytes encoded = encode_parameter(record);
    record.parameter_crc16 = read_be16(&encoded[34]);
    return record;
}

bool validate_app_image(const Bytes& data, std::string* error) {
    if (data.size() < 8U || data.size() > kAppSize) {
        if (error != nullptr) *error = "App image size is out of range";
        return false;
    }
    const std::uint32_t msp = read_le32(data.data());
    const std::uint32_t reset = read_le32(data.data() + 4U) & ~1U;
    if (msp < 0x20000000U || msp >= 0x20028000U) {
        if (error != nullptr) *error = "App initial MSP is outside SRAM";
        return false;
    }
    if (reset < kAppBase || reset >= kAppBase + kAppSize) {
        if (error != nullptr) *error = "App Reset_Handler is outside App";
        return false;
    }
    return true;
}

std::string uid_hex(const Uid& uid) {
    return bytes_hex(uid.data(), uid.size());
}

std::string serial_hex(const SerialNumber& serial_number) {
    return bytes_hex(serial_number.data(), serial_number.size());
}

std::string format_device_name(std::uint32_t model, std::uint32_t hardware_version) {
    std::ostringstream oss;
    oss << model << "号";
    if (!is_landscape(hardware_version)) {
        oss << "竖屏";
    }
    const std::uint32_t ver = hardware_version_number(hardware_version);
    const unsigned major = (ver >> 8) & 0xFFU;
    const unsigned minor = ver & 0xFFU;
    oss << " V" << major << "." << minor;
    return oss.str();
}

}  // namespace usb_upper::iap
