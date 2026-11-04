#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include "httplib.h"
#include "json.hpp"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "dualplay.h"

using json = nlohmann::json;

static const char* SYSTEM_PROMPT =
    "You are DAVID, a sharp, witty voice assistant with your own personality and opinions. "
    "When asked what you think, pick a side and say why in plain words. "
    "Never say you are an AI unless if i ask you what u are thats the only part u can say it, never say you have no opinions or preferences, and never refuse to choose. "
    "For social situations and decisions, give honest advice with a clear recommendation, and disagree with the user when you think they are wrong. "
    "If you do not know a fact either say so instead of guessing or try to atleast find a correaltion discusion to it "
    "Speak casually like a smart friend. Keep answers clear and good, maximum 40 words, because your replies are spoken aloud. "
    "Never use markdown, lists, or emojis.";

// ---------- microphone capture ----------
static std::mutex g_mtx;
static std::deque<int16_t> g_buf;
static std::atomic<bool> g_deaf{false};   // true while DAVID talks, so he doesn't hear himself

static void mic_callback(ma_device*, void*, const void* input, ma_uint32 frames) {
    if (g_deaf.load() || !input) return;
    const int16_t* s = static_cast<const int16_t*>(input);
    std::lock_guard<std::mutex> lk(g_mtx);
    g_buf.insert(g_buf.end(), s, s + frames);
}

static bool pop_chunk(std::vector<int16_t>& out, size_t n) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_buf.size() < n) return false;
    out.assign(g_buf.begin(), g_buf.begin() + n);
    g_buf.erase(g_buf.begin(), g_buf.begin() + n);
    return true;
}

static void clear_buf() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_buf.clear();
}

static double rms(const std::vector<int16_t>& v) {
    if (v.empty()) return 0.0;
    double s = 0;
    for (auto x : v) s += double(x) * double(x);
    return std::sqrt(s / double(v.size()));
}

// Waits for you to speak, records until you stop. Returns empty if nothing
// was heard within waitSeconds (0 = wait forever).
static std::vector<int16_t> hear_phrase(double threshold, double waitSeconds) {
    const size_t CHUNK = 320;            // 20 ms at 16 kHz
    const size_t END_SILENCE = 30;       // 0.8 s of quiet ends the phrase
    const size_t MIN_SPEECH = 15;        // ignore sounds shorter than 0.3 s
    const size_t PRE_ROLL = 10;          // keep 0.2 s before speech starts
    const size_t MAX_SAMPLES = 16000 * 15;

    std::vector<int16_t> chunk, phrase;
    std::deque<std::vector<int16_t>> pre;
    bool speaking = false;
    size_t silent = 0, speechChunks = 0;
    auto start = std::chrono::steady_clock::now();

    while (true) {
        if (!pop_chunk(chunk, CHUNK)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!speaking && waitSeconds > 0) {
                double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                if (el > waitSeconds) return {};
            }
            continue;
        }
        bool loud = rms(chunk) > threshold;
        if (!speaking) {
            pre.push_back(chunk);
            if (pre.size() > PRE_ROLL) pre.pop_front();
            if (loud) {
                speaking = true;
                speechChunks = 1;
                silent = 0;
                for (auto& c : pre) phrase.insert(phrase.end(), c.begin(), c.end());
                pre.clear();
            }
        } else {
            phrase.insert(phrase.end(), chunk.begin(), chunk.end());
            if (loud) { speechChunks++; silent = 0; } else { silent++; }
            if (silent >= END_SILENCE || phrase.size() > MAX_SAMPLES) {
                if (speechChunks >= MIN_SPEECH) return phrase;
                phrase.clear();          // just a click or cough, start over
                speaking = false;
                speechChunks = 0;
                silent = 0;
            }
        }
    }
}

// ---------- speech to text (whisper.cpp) ----------
static void write_wav(const std::string& path, const std::vector<int16_t>& pcm) {
    std::ofstream f(path, std::ios::binary);
    uint32_t dataBytes = uint32_t(pcm.size() * 2);
    uint32_t riffSize = 36 + dataBytes, fmtSize = 16, sr = 16000, byteRate = sr * 2;
    uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
    f.write("RIFF", 4);  f.write(reinterpret_cast<char*>(&riffSize), 4);
    f.write("WAVE", 4);  f.write("fmt ", 4);
    f.write(reinterpret_cast<char*>(&fmtSize), 4);
    f.write(reinterpret_cast<char*>(&fmt), 2);
    f.write(reinterpret_cast<char*>(&ch), 2);
    f.write(reinterpret_cast<char*>(&sr), 4);
    f.write(reinterpret_cast<char*>(&byteRate), 4);
    f.write(reinterpret_cast<char*>(&align), 2);
    f.write(reinterpret_cast<char*>(&bits), 2);
    f.write("data", 4);
    f.write(reinterpret_cast<char*>(&dataBytes), 4);
    f.write(reinterpret_cast<const char*>(pcm.data()), dataBytes);
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// Removes things like [BLANK_AUDIO] and (silence) that Whisper prints for noise.
static std::string clean_transcript(const std::string& raw) {
    std::string out;
    int depth = 0;
    for (char c : raw) {
        if (c == '[' || c == '(') { depth++; continue; }
        if (c == ']' || c == ')') { if (depth > 0) depth--; continue; }
        if (depth > 0) continue;
        out += (c == '\n' || c == '\r') ? ' ' : c;
    }
    return trim(out);
}

static std::string transcribe(const std::vector<int16_t>& pcm) {
    write_wav("D:\\DAVID\\heard.wav", pcm);
    std::remove("D:\\DAVID\\heard.txt");
    std::system(
        "D:\\DAVID\\whisper\\whisper-cli.exe "
        "-m D:\\DAVID\\whisper\\ggml-base.en.bin "
        "-f D:\\DAVID\\heard.wav -nt -np -t 4 -bs 1 -bo 1 -nf "
        "> D:\\DAVID\\heard.txt 2> NUL");
    std::ifstream f("D:\\DAVID\\heard.txt", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return clean_transcript(ss.str());
}

// ---------- wake word ----------
static bool find_wake(const std::string& text, std::string& rest) {
    std::string low = text;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    for (const char* w : {"david", "davit"}) {
        size_t p = low.find(w);
        if (p != std::string::npos) {
            rest = text.substr(p + std::strlen(w));
            size_t i = 0;
            while (i < rest.size() && (std::isspace((unsigned char)rest[i]) || std::ispunct((unsigned char)rest[i]))) i++;
            rest = rest.substr(i);
            return true;
        }
    }
    return false;
}

static int word_count(const std::string& s) {
    std::istringstream is(s);
    std::string w;
    int n = 0;
    while (is >> w) n++;
    return n;
}

// ---------- text to speech (Piper) ----------
static void speak(const std::string& text) {
    g_deaf = true;
    std::string clean;
    for (char c : text) {
        if (c == '*' || c == '#' || c == '`') continue;
        clean += (c == '\n' || c == '\r') ? ' ' : c;
    }
    {
        std::ofstream f("D:\\DAVID\\reply.txt", std::ios::binary);
        f << clean;
    }
    int rc = std::system(
        "D:\\DAVID\\piper\\piper.exe "
        "--model D:\\DAVID\\voices\\en_US-lessac-medium.onnx "
        "--output_file D:\\DAVID\\reply.wav "
        "< D:\\DAVID\\reply.txt > NUL 2>&1");
    if (rc == 0) play_wav_dual("D:\\DAVID\\reply.wav");
    else std::cout << "[Piper failed to generate speech]\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    clear_buf();
    g_deaf = false;
}

// ---------- the AI brain (llama-server) ----------
static void ask(httplib::Client& cli, json& messages, const std::string& question) {
    std::cout << "You: " << question << "\n";
    messages.push_back({{"role", "user"}, {"content", question}});

    json body = {{"messages", messages}, {"max_tokens", 100}, {"temperature", 0.7}};
    auto res = cli.Post("/v1/chat/completions", body.dump(), "application/json");

    if (!res || res->status != 200) {
        std::cout << "[Can't reach llama-server. Is it running on port 8080?]\n";
        messages.erase(messages.size() - 1);
        return;
    }
    try {
        json j = json::parse(res->body);
        std::string reply = j["choices"][0]["message"]["content"];
        std::cout << "DAVID: " << reply << "\n";
        messages.push_back({{"role", "assistant"}, {"content", reply}});
        speak(reply);
    } catch (const std::exception& e) {
        std::cout << "[Couldn't read the reply: " << e.what() << "]\n";
        messages.erase(messages.size() - 1);
        return;
    }
    while (messages.size() > 11) {
        messages.erase(messages.begin() + 1, messages.begin() + 3);
    }
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    httplib::Client cli("127.0.0.1", 8080);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(120);

    json messages = json::array();
    messages.push_back({{"role", "system"}, {"content", SYSTEM_PROMPT}});

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format = ma_format_s16;
    cfg.capture.channels = 1;
    cfg.sampleRate = 16000;
    cfg.dataCallback = mic_callback;

    ma_device dev;
    if (ma_device_init(NULL, &cfg, &dev) != MA_SUCCESS) {
        std::cout << "[Couldn't open the microphone. Check Windows microphone privacy settings.]\n";
        return 1;
    }
    ma_device_start(&dev);

    std::cout << "Calibrating microphone... stay quiet for 2 seconds.\n";
    clear_buf();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::vector<int16_t> all;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        all.assign(g_buf.begin(), g_buf.end());
        g_buf.clear();
    }
    if (all.empty()) std::cout << "[Warning: the microphone gave no audio at all.]\n";
    double noise = rms(all);
    double threshold = std::max(200.0, noise * 3.0);
    if (argc > 1) threshold = std::atof(argv[1]);
    std::cout << "Noise level: " << noise << "  |  Speech threshold: " << threshold << "\n";

    std::cout << "\nDAVID is listening. Say \"Hey David\" and then your question. Ctrl+C to quit.\n";

    while (true) {
        auto pcm = hear_phrase(threshold, 0);
        std::string text = transcribe(pcm);
        if (text.empty()) continue;
        std::cout << "[heard] " << text << "\n";

        std::string rest;
        if (!find_wake(text, rest)) continue;

        if (word_count(rest) >= 2) {            // "Hey David, what time is it"
            ask(cli, messages, rest);
            continue;
        }

        speak("Yes?");                           // just "Hey David", so wait for the question
        auto q = hear_phrase(threshold, 8.0);
        if (q.empty()) { std::cout << "[no question heard]\n"; continue; }
        std::string qt = transcribe(q);
        if (qt.empty()) continue;
        ask(cli, messages, qt);
    }

    ma_device_uninit(&dev);
    return 0;
}




