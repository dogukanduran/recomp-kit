#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Transparently decodes packed assets written by the web importer:
// - "RCDF": 8-byte header ("RCDF" + u32le orig_size) + zlib-deflated payload
// - "RCWV": 16-byte header ("RCWV" + u32le samples_per_ch + u32le sample_rate + u16le channels + u16le 0)
//           followed by 4-byte initial IMA state per channel and 4-bit IMA ADPCM nibbles.
bool packed_asset_try_load(const std::string &path, std::vector<uint8_t> *out);
bool packed_asset_uncompressed_size(const std::string &path, uint64_t *out_size);

// Streaming RCWV decoder: zero upfront PCM allocation; decodes on-the-fly in read()/seek().
struct PackedWavStream {
    FILE *fp = nullptr;
    uint32_t samples_per_ch = 0;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint64_t total_size = 0;
    uint64_t pos = 0;
    uint32_t cur_sample = 0;
    long data_start = 0;
    int init_pred[2] = {0, 0};
    int init_index[2] = {0, 0};
    int pred[2] = {0, 0};
    int index[2] = {0, 0};
    bool has_pending_byte = false;
    uint8_t pending_byte = 0;
    uint8_t wav_hdr[44] = {};

    PackedWavStream() = default;
    ~PackedWavStream() { close(); }
    PackedWavStream(const PackedWavStream &) = delete;
    PackedWavStream &operator=(const PackedWavStream &) = delete;
    PackedWavStream(PackedWavStream &&o) noexcept;
    PackedWavStream &operator=(PackedWavStream &&o) noexcept;

    bool open(const std::string &path);
    void close();
    size_t read(uint8_t *dst, size_t n);
    bool seek(int64_t off, int whence);
};
