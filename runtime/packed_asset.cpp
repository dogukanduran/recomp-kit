#include "packed_asset.h"
#include "../third_party/miniz/miniz.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

namespace {

uint32_t read_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t read_u16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
void write_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}
void write_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static const int k_ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int k_ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

inline int16_t ima_decode_nibble(int nibble, int &pred, int &index) {
    int step = k_ima_step_table[index];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) pred -= diff;
    else pred += diff;
    if (pred > 32767) pred = 32767;
    else if (pred < -32768) pred = -32768;
    index += k_ima_index_table[nibble & 15];
    if (index < 0) index = 0;
    else if (index > 88) index = 88;
    return (int16_t)pred;
}

void build_wav_header(uint8_t w[44], uint32_t samples_per_ch, uint32_t sample_rate, uint16_t channels) {
    uint32_t pcm_bytes = samples_per_ch * channels * 2;
    std::memcpy(w + 0, "RIFF", 4);
    write_u32le(w + 4, 36 + pcm_bytes);
    std::memcpy(w + 8, "WAVEfmt ", 8);
    write_u32le(w + 16, 16);
    write_u16le(w + 20, 1);
    write_u16le(w + 22, channels);
    write_u32le(w + 24, sample_rate);
    write_u32le(w + 28, sample_rate * channels * 2);
    write_u16le(w + 32, channels * 2);
    write_u16le(w + 34, 16);
    std::memcpy(w + 36, "data", 4);
    write_u32le(w + 40, pcm_bytes);
}

bool decode_rcdf(FILE *fp, uint32_t orig_size, std::vector<uint8_t> *out) {
    if (fseek(fp, 0, SEEK_END) != 0) return false;
    long total = ftell(fp);
    if (total <= 8) return false;
    size_t comp_size = (size_t)(total - 8);
    if (fseek(fp, 8, SEEK_SET) != 0) return false;

    std::vector<uint8_t> comp(comp_size);
    if (fread(comp.data(), 1, comp_size, fp) != comp_size) return false;

    out->resize(orig_size);
    mz_ulong dest_len = (mz_ulong)orig_size;
    int rc = mz_uncompress(out->data(), &dest_len, comp.data(), (mz_ulong)comp_size);
    if (rc != MZ_OK || dest_len != orig_size) {
        out->clear();
        return false;
    }
    return true;
}

bool decode_rcwv(FILE *fp, const uint8_t hdr[16], std::vector<uint8_t> *out) {
    uint32_t samples_per_ch = read_u32le(hdr + 4);
    uint32_t sample_rate = read_u32le(hdr + 8);
    uint16_t channels = read_u16le(hdr + 12);
    if ((channels != 1 && channels != 2) || sample_rate == 0 || samples_per_ch == 0) return false;

    if (fseek(fp, 0, SEEK_END) != 0) return false;
    long total = ftell(fp);
    long data_start = 16 + (long)channels * 4;
    if (total < data_start) return false;

    if (fseek(fp, 16, SEEK_SET) != 0) return false;
    int pred[2] = {0, 0};
    int index[2] = {0, 0};
    for (uint16_t ch = 0; ch < channels; ++ch) {
        uint8_t st[4];
        if (fread(st, 1, 4, fp) != 4) return false;
        pred[ch] = (int16_t)read_u16le(st);
        index[ch] = std::clamp((int)st[2], 0, 88);
    }

    size_t nibble_bytes = (size_t)(total - data_start);
    std::vector<uint8_t> packed(nibble_bytes);
    if (nibble_bytes > 0 && fread(packed.data(), 1, nibble_bytes, fp) != nibble_bytes) return false;

    uint32_t pcm_bytes = samples_per_ch * channels * 2;
    out->resize(44 + pcm_bytes);
    build_wav_header(out->data(), samples_per_ch, sample_rate, channels);

    int16_t *dst = (int16_t *)(out->data() + 44);
    if (channels == 1) {
        dst[0] = (int16_t)pred[0];
        uint32_t s = 1;
        for (size_t i = 0; i < nibble_bytes && s < samples_per_ch; ++i) {
            uint8_t b = packed[i];
            dst[s++] = ima_decode_nibble(b & 0x0f, pred[0], index[0]);
            if (s < samples_per_ch)
                dst[s++] = ima_decode_nibble((b >> 4) & 0x0f, pred[0], index[0]);
        }
    } else {
        dst[0] = (int16_t)pred[0];
        dst[1] = (int16_t)pred[1];
        uint32_t s = 1;
        for (size_t i = 0; i < nibble_bytes && s < samples_per_ch; ++i) {
            uint8_t b = packed[i];
            dst[s * 2 + 0] = ima_decode_nibble(b & 0x0f, pred[0], index[0]);
            dst[s * 2 + 1] = ima_decode_nibble((b >> 4) & 0x0f, pred[1], index[1]);
            ++s;
        }
    }
    return true;
}

constexpr size_t k_cache_budget = 24u * 1024u * 1024u;
struct CacheEntry {
    std::string path;
    std::vector<uint8_t> data;
};
std::mutex g_cache_m;
std::list<CacheEntry> g_cache_lru;
std::unordered_map<std::string, std::list<CacheEntry>::iterator> g_cache_map;
size_t g_cache_bytes = 0;

bool cache_get(const std::string &path, std::vector<uint8_t> *out) {
    std::lock_guard<std::mutex> lock(g_cache_m);
    auto it = g_cache_map.find(path);
    if (it == g_cache_map.end()) return false;
    g_cache_lru.splice(g_cache_lru.begin(), g_cache_lru, it->second);
    *out = it->second->data;
    return true;
}

void cache_put(const std::string &path, const std::vector<uint8_t> &data) {
    if (data.size() > k_cache_budget / 2) return;
    std::lock_guard<std::mutex> lock(g_cache_m);
    auto it = g_cache_map.find(path);
    if (it != g_cache_map.end()) {
        g_cache_bytes -= it->second->data.size();
        g_cache_lru.erase(it->second);
        g_cache_map.erase(it);
    }
    while (g_cache_bytes + data.size() > k_cache_budget && !g_cache_lru.empty()) {
        auto &back = g_cache_lru.back();
        g_cache_bytes -= back.data.size();
        g_cache_map.erase(back.path);
        g_cache_lru.pop_back();
    }
    g_cache_lru.push_front(CacheEntry{path, data});
    g_cache_map[path] = g_cache_lru.begin();
    g_cache_bytes += data.size();
}

} // namespace

bool packed_asset_try_load(const std::string &path, std::vector<uint8_t> *out) {
    if (path.empty() || !out) return false;
    if (cache_get(path, out)) return true;
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    uint8_t hdr[16] = {};
    size_t n = std::fread(hdr, 1, sizeof(hdr), fp);
    bool ok = false;
    if (n >= 8 && std::memcmp(hdr, "RCDF", 4) == 0) {
        ok = decode_rcdf(fp, read_u32le(hdr + 4), out);
    } else if (n == 16 && std::memcmp(hdr, "RCWV", 4) == 0) {
        ok = decode_rcwv(fp, hdr, out);
    }
    std::fclose(fp);
    if (ok) cache_put(path, *out);
    return ok;
}

bool packed_asset_uncompressed_size(const std::string &path, uint64_t *out_size) {
    if (path.empty() || !out_size) return false;
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    uint8_t hdr[16] = {};
    size_t n = std::fread(hdr, 1, sizeof(hdr), fp);
    std::fclose(fp);
    if (n >= 8 && std::memcmp(hdr, "RCDF", 4) == 0) {
        *out_size = read_u32le(hdr + 4);
        return true;
    }
    if (n == 16 && std::memcmp(hdr, "RCWV", 4) == 0) {
        uint64_t samples = read_u32le(hdr + 4);
        uint64_t channels = read_u16le(hdr + 12);
        if ((channels == 1 || channels == 2) && samples > 0) {
            *out_size = 44 + samples * channels * 2;
            return true;
        }
    }
    return false;
}

PackedWavStream::PackedWavStream(PackedWavStream &&o) noexcept {
    *this = std::move(o);
}

PackedWavStream &PackedWavStream::operator=(PackedWavStream &&o) noexcept {
    if (this != &o) {
        close();
        fp = o.fp;
        o.fp = nullptr;
        samples_per_ch = o.samples_per_ch;
        sample_rate = o.sample_rate;
        channels = o.channels;
        total_size = o.total_size;
        pos = o.pos;
        cur_sample = o.cur_sample;
        data_start = o.data_start;
        std::memcpy(init_pred, o.init_pred, sizeof(init_pred));
        std::memcpy(init_index, o.init_index, sizeof(init_index));
        std::memcpy(pred, o.pred, sizeof(pred));
        std::memcpy(index, o.index, sizeof(index));
        has_pending_byte = o.has_pending_byte;
        pending_byte = o.pending_byte;
        std::memcpy(wav_hdr, o.wav_hdr, sizeof(wav_hdr));
    }
    return *this;
}

bool PackedWavStream::open(const std::string &path) {
    close();
    if (path.empty()) return false;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t hdr[16] = {};
    if (std::fread(hdr, 1, 16, f) != 16 || std::memcmp(hdr, "RCWV", 4) != 0) {
        std::fclose(f);
        return false;
    }
    samples_per_ch = read_u32le(hdr + 4);
    sample_rate = read_u32le(hdr + 8);
    channels = read_u16le(hdr + 12);
    if ((channels != 1 && channels != 2) || sample_rate == 0 || samples_per_ch == 0) {
        std::fclose(f);
        return false;
    }
    for (uint16_t ch = 0; ch < channels; ++ch) {
        uint8_t st[4];
        if (std::fread(st, 1, 4, f) != 4) {
            std::fclose(f);
            return false;
        }
        init_pred[ch] = (int16_t)read_u16le(st);
        init_index[ch] = std::clamp((int)st[2], 0, 88);
        pred[ch] = init_pred[ch];
        index[ch] = init_index[ch];
    }
    fp = f;
    data_start = 16 + (long)channels * 4;
    total_size = 44 + (uint64_t)samples_per_ch * channels * 2;
    pos = 0;
    cur_sample = 0;
    has_pending_byte = false;
    build_wav_header(wav_hdr, samples_per_ch, sample_rate, channels);
    return true;
}

void PackedWavStream::close() {
    if (fp) {
        std::fclose(fp);
        fp = nullptr;
    }
}

bool PackedWavStream::seek(int64_t off, int whence) {
    int64_t base = whence == 1 ? (int64_t)pos : whence == 2 ? (int64_t)total_size : 0;
    int64_t next = base + off;
    if (next < 0) return false;
    pos = (uint64_t)next;
    return true;
}

size_t PackedWavStream::read(uint8_t *dst, size_t n) {
    if (!fp || pos >= total_size || n == 0) return 0;
    size_t want = (size_t)std::min<uint64_t>(n, total_size - pos);
    size_t written = 0;

    if (pos < 44) {
        size_t take = std::min<size_t>(want, (size_t)(44 - pos));
        std::memcpy(dst, wav_hdr + pos, take);
        pos += take;
        written += take;
        dst += take;
        want -= take;
    }
    if (want == 0) return written;

    uint32_t frame_bytes = channels * 2;
    uint32_t target_sample = (uint32_t)((pos - 44) / frame_bytes);
    if (target_sample < cur_sample) {
        std::fseek(fp, data_start, SEEK_SET);
        pred[0] = init_pred[0];
        pred[1] = init_pred[1];
        index[0] = init_index[0];
        index[1] = init_index[1];
        cur_sample = 0;
        has_pending_byte = false;
    }

    auto next_sample = [&](int16_t out[2]) {
        if (cur_sample == 0) {
            out[0] = (int16_t)pred[0];
            out[1] = (int16_t)pred[1];
            ++cur_sample;
            return;
        }
        if (channels == 1) {
            if (!has_pending_byte) {
                int c = std::fgetc(fp);
                pending_byte = (c == EOF) ? 0 : (uint8_t)c;
                has_pending_byte = true;
                out[0] = ima_decode_nibble(pending_byte & 0x0f, pred[0], index[0]);
            } else {
                has_pending_byte = false;
                out[0] = ima_decode_nibble((pending_byte >> 4) & 0x0f, pred[0], index[0]);
            }
        } else {
            int c = std::fgetc(fp);
            uint8_t b = (c == EOF) ? 0 : (uint8_t)c;
            out[0] = ima_decode_nibble(b & 0x0f, pred[0], index[0]);
            out[1] = ima_decode_nibble((b >> 4) & 0x0f, pred[1], index[1]);
        }
        ++cur_sample;
    };

    int16_t cur_frame[2] = {0, 0};
    while (cur_sample <= target_sample) {
        next_sample(cur_frame);
    }

    while (written < want + written && pos < total_size) {
        size_t byte_in_frame = (size_t)((pos - 44) % frame_bytes);
        uint8_t frame_buf[4];
        write_u16le(frame_buf, (uint16_t)cur_frame[0]);
        if (channels == 2) write_u16le(frame_buf + 2, (uint16_t)cur_frame[1]);
        size_t take = std::min<size_t>(want, frame_bytes - byte_in_frame);
        std::memcpy(dst, frame_buf + byte_in_frame, take);
        dst += take;
        pos += take;
        written += take;
        want -= take;
        if (want == 0) break;
        if ((pos - 44) % frame_bytes == 0 && cur_sample < samples_per_ch) {
            next_sample(cur_frame);
        }
    }
    return written;
}
