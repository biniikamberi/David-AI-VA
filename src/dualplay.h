#pragma once
// Plays a WAV file on the default speakers AND on the VB-CABLE virtual mic at the same time.
#include <string>
#include <vector>
#include <fstream>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include <iostream>
#include <cstdint>
#include <cstring>

struct PlayState {
    const int16_t* data = nullptr;
    size_t frames = 0;
    uint32_t channels = 1;
    std::atomic<size_t> pos{0};
};

static void play_callback(ma_device* dev, void* out, const void*, ma_uint32 frameCount) {
    PlayState* st = static_cast<PlayState*>(dev->pUserData);
    int16_t* o = static_cast<int16_t*>(out);
    size_t pos = st->pos.load();
    size_t avail = pos < st->frames ? st->frames - pos : 0;
    size_t n = std::min<size_t>(avail, frameCount);
    if (n) std::memcpy(o, st->data + pos * st->channels, n * st->channels * sizeof(int16_t));
    if (n < frameCount) std::memset(o + n * st->channels, 0, (frameCount - n) * st->channels * sizeof(int16_t));
    st->pos.store(pos + n);
}

static bool load_wav(const std::string& path, std::vector<int16_t>& pcm, uint32_t& sr, uint32_t& ch) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char hdr[12];
    f.read(hdr, 12);
    if (!f || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) return false;
    bool haveFmt = false;
    uint16_t bits = 0;
    while (f) {
        char id[4];
        uint32_t size = 0;
        f.read(id, 4);
        f.read(reinterpret_cast<char*>(&size), 4);
        if (!f) break;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            std::vector<char> b(size < 16 ? 16 : size);
            f.read(b.data(), size);
            uint16_t channels = 0; uint32_t rate = 0;
            std::memcpy(&channels, b.data() + 2, 2);
            std::memcpy(&rate, b.data() + 4, 4);
            std::memcpy(&bits, b.data() + 14, 2);
            ch = channels; sr = rate; haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (!haveFmt || bits != 16) return false;
            std::streampos here = f.tellg();
            f.seekg(0, std::ios::end);
            size_t remaining = size_t(f.tellg() - here);
            f.seekg(here);
            size_t bytes = (size == 0 || size > remaining) ? remaining : size;
            pcm.resize(bytes / 2);
            f.read(reinterpret_cast<char*>(pcm.data()), pcm.size() * 2);
            return true;
        } else {
            f.seekg(size + (size & 1), std::ios::cur);
        }
    }
    return false;
}

static void play_wav_dual(const std::string& path) {
    std::vector<int16_t> pcm;
    uint32_t sr = 0, ch = 0;
    if (!load_wav(path, pcm, sr, ch) || ch == 0 || pcm.empty()) {
        std::cout << "[Couldn't read the voice file]\n";
        return;
    }

    static ma_context ctx;
    static bool ctxReady = false, cableFound = false, tried = false;
    static ma_device_id cableId;
    if (!tried) {
        tried = true;
        ctxReady = (ma_context_init(NULL, 0, NULL, &ctx) == MA_SUCCESS);
        if (ctxReady) {
            ma_device_info* playInfos; ma_uint32 playCount;
            ma_device_info* capInfos;  ma_uint32 capCount;
            if (ma_context_get_devices(&ctx, &playInfos, &playCount, &capInfos, &capCount) == MA_SUCCESS) {
                for (ma_uint32 i = 0; i < playCount; i++) {
                    if (std::string(playInfos[i].name).find("CABLE Input") != std::string::npos) {
                        cableId = playInfos[i].id;
                        cableFound = true;
                        break;
                    }
                }
            }
        }
        std::cout << (cableFound ? "[VB-CABLE found: DAVID also speaks into the virtual mic]\n"
                                 : "[VB-CABLE not found: speakers only]\n");
    }
    if (!ctxReady) { std::cout << "[Audio system failed to start]\n"; return; }

    PlayState a, b;
    a.data = b.data = pcm.data();
    a.frames = b.frames = pcm.size() / ch;
    a.channels = b.channels = ch;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = ch;
    cfg.sampleRate = sr;
    cfg.dataCallback = play_callback;

    ma_device speakers, cable;
    bool spkOk = false, cabOk = false;

    cfg.pUserData = &a;
    cfg.playback.pDeviceID = NULL;                     // default speakers/headphones
    spkOk = (ma_device_init(&ctx, &cfg, &speakers) == MA_SUCCESS);

    if (cableFound) {
        cfg.pUserData = &b;
        cfg.playback.pDeviceID = &cableId;             // virtual mic
        cabOk = (ma_device_init(&ctx, &cfg, &cable) == MA_SUCCESS);
    }

    if (spkOk) ma_device_start(&speakers);
    if (cabOk) ma_device_start(&cable);

    double maxSeconds = double(a.frames) / double(sr) + 3.0;
    auto t0 = std::chrono::steady_clock::now();
    while ((spkOk && a.pos.load() < a.frames) || (cabOk && b.pos.load() < b.frames)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > maxSeconds) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));   // let the tail finish playing

    if (spkOk) ma_device_uninit(&speakers);
    if (cabOk) ma_device_uninit(&cable);
}
