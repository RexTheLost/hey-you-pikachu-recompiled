// Hey You, Pikachu! (USA) — recompiled PC build entry point.
//
// Usage: HeyYouPikachuRecompiled.exe [--rom <path to Hey You, Pikachu! (USA).z64>]
// The ROM is validated (XXH3 hash) and stored next to the exe on first run, so
// later runs need no argument. The game starts immediately (no launcher UI).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <vector>

#include "SDL.h"
#include "SDL_syswm.h"

#ifdef _WIN32
#include <Windows.h>
#include <timeapi.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#pragma comment(lib, "Dbghelp.lib")
#include <commdlg.h>
#pragma comment(lib, "Comdlg32.lib")
#endif

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"

#include "hyp_audio.h"
#include "hyp_control.h"
#include "hyp_game.h"
#include "hyp_input.h"
#include "hyp_menu.h"
#include "hyp_motion.h"
#include "hyp_render.h"
#include "hyp_settings.h"
#include "hyp_trace.h"
#include "hyp_vru.h"

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
#ifdef __ANDROID__
namespace hyp::android { void install_crash_report(const std::filesystem::path& data_dir); }
#endif
gpr get_entrypoint_address();

extern RspUcodeFunc n_aspMain;

namespace {
    const std::u8string game_id = u8"hyp.n64.us.1.0";

    [[noreturn]] void exit_error(const char* msg) {
        fprintf(stderr, "%s\n", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Hey You, Pikachu! Recompiled", msg, nullptr);
        std::exit(EXIT_FAILURE);
    }

    std::filesystem::path program_dir() {
#ifdef _WIN32
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path();
#elif defined(__ANDROID__)
        // The app's private storage (the Java side puts the ROM and the voice model there).
        return std::filesystem::path(SDL_AndroidGetInternalStoragePath());
#else
        return std::filesystem::current_path();
#endif
    }

    // ---- Graphics / window ----

    SDL_Window* window = nullptr;

#ifdef __ANDROID__
    // App going to the background / coming back. Called on the thread that posts the event
    // (Android's UI thread), right away: the event loop itself is blocked while paused.
    int SDLCALL on_app_event(void*, SDL_Event* event);
#endif

    ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
        std::printf("[start] SDL video init\n");
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "1");
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1"); // a stray back swipe must not close the game
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
            exit_error(SDL_GetError());
        }
#ifdef __ANDROID__
        SDL_AddEventWatch(on_app_event, nullptr);
#endif
        return {};
    }

    ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
        uint32_t flags = SDL_WINDOW_RESIZABLE;
#ifndef _WIN32
        flags |= SDL_WINDOW_VULKAN; // RT64 makes its Vulkan surface from the SDL window
#endif
#ifdef __ANDROID__
        flags |= SDL_WINDOW_FULLSCREEN;
#endif
        if (std::getenv("HYP_ON_TOP") != nullptr) {
            flags |= SDL_WINDOW_ALWAYS_ON_TOP; // test runs: keep the window capturable
        }
        window = SDL_CreateWindow("Hey You, Pikachu! Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  1280, 960, flags);
        if (window == nullptr) {
            exit_error(SDL_GetError());
        }
        SDL_SysWMinfo wm_info;
        SDL_VERSION(&wm_info.version);
        SDL_GetWindowWMInfo(window, &wm_info);
        std::printf("[start] window %p created\n", (void*)window);
        hyp::menu::attach(window);
#ifdef _WIN32
        return ultramodern::renderer::WindowHandle{ wm_info.info.win.window, GetCurrentThreadId() };
#else
        return window;
#endif
    }

    // Test aid: HYP_VRU_SAY="word@seconds[,word@seconds...]" says words at fixed
    // times after launch (seconds since the window was created).
    void run_scripted_speech() {
        static std::vector<std::pair<double, std::string>> script = [] {
            std::vector<std::pair<double, std::string>> s;
            if (const char* env = std::getenv("HYP_VRU_SAY")) {
                std::string spec = env;
                size_t pos = 0;
                while (pos < spec.size()) {
                    size_t end = spec.find(',', pos);
                    std::string item = spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                    size_t at = item.rfind('@');
                    if (at != std::string::npos) s.emplace_back(std::atof(item.c_str() + at + 1), item.substr(0, at));
                    if (end == std::string::npos) break;
                    pos = end + 1;
                }
            }
            return s;
        }();
        static const auto start = std::chrono::steady_clock::now();
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        for (auto it = script.begin(); it != script.end();) {
            if (t >= it->first) {
                bool ok = hyp::vru::say_word(it->second);
                std::printf("[vru] scripted say '%s' at %.1fs: %s\n", it->second.c_str(), t, ok ? "queued" : "not in dictionary");
                it = script.erase(it);
            } else {
                ++it;
            }
        }
    }

    void update_gfx(void*) {
        run_scripted_speech();
        hyp::control::poll();
        // Show what the microphone heard in the title bar for a few seconds.
        {
            static std::string shown;
            std::string heard = hyp::vru::mic_last_heard(4.0);
            if (heard != shown && window != nullptr) {
                shown = heard;
                std::string title = "Hey You, Pikachu! Recompiled";
                if (!heard.empty()) title += "   —   heard: " + heard;
                SDL_SetWindowTitle(window, title.c_str());
            }
        }
        static bool picker_opened = false;
        if (!picker_opened && std::getenv("HYP_PICKER") != nullptr) {
            picker_opened = true; // test aid: open the word picker at launch
            hyp::vru::toggle_picker();
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    ultramodern::quit();
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                case SDL_CONTROLLERDEVICEREMOVED:
                    hyp::input::handle_controller_event(event);
                    break;
                case SDL_KEYDOWN:
                    if (event.key.keysym.scancode == SDL_SCANCODE_TAB && !event.key.repeat) {
                        hyp::vru::toggle_picker();
                    }
                    if (event.key.keysym.scancode == SDL_SCANCODE_F11 ||
                        (event.key.keysym.scancode == SDL_SCANCODE_RETURN && (event.key.keysym.mod & KMOD_ALT))) {
                        hyp::menu::toggle_fullscreen();
                    }
                    if (event.key.keysym.scancode == SDL_SCANCODE_F1) {
                        hyp::menu::show_controls();
                    }
                    if (event.key.keysym.scancode == SDL_SCANCODE_F2 && !event.key.repeat) {
                        hyp::motion::toggle_window();
                    }
                    break;
            }
        }
    }

    // ---- Audio (adapted from Zelda64Recomp, GPL-3.0) ----

    SDL_AudioCVT audio_convert;
    SDL_AudioDeviceID audio_device = 0;
    std::mutex audio_device_mutex; // the menu reopens the device on the window thread
    std::atomic<float> audio_volume = 1.0f;
    uint32_t sample_rate = 48000;
    uint32_t output_sample_rate = 48000;
    constexpr uint32_t input_channels = 2;
    uint32_t output_channels = 2;
    constexpr uint32_t duplicated_input_frames = 4;
    uint32_t discarded_output_frames;
    constexpr uint32_t bytes_per_frame = input_channels * sizeof(float);

    // Debug: HYP_AUDIO_DUMP=<file.raw> appends the game's output (16-bit stereo, L/R
    // after the channel fix, at the game's sample rate) as raw PCM.
    void dump_audio(const int16_t* audio_data, size_t sample_count) {
        static FILE* f = std::getenv("HYP_AUDIO_DUMP") ? std::fopen(std::getenv("HYP_AUDIO_DUMP"), "wb") : nullptr;
        if (f == nullptr) return;
        for (size_t i = 0; i + 1 < sample_count; i += 2) {
            int16_t lr[2] = { audio_data[i + 1], audio_data[i + 0] };
            std::fwrite(lr, sizeof(lr), 1, f);
        }
        std::fflush(f);
    }

    void queue_samples(int16_t* audio_data, size_t sample_count) {
        dump_audio(audio_data, sample_count);
        if (hyp_trace_enabled()) {
            // per-second production stats: buffers, frames, buffer size range
            static auto window_start = std::chrono::steady_clock::now();
            static uint32_t calls = 0, frames = 0, min_frames = ~0u, max_frames = 0;
            calls++;
            frames += (uint32_t)(sample_count / 2);
            min_frames = std::min(min_frames, (uint32_t)(sample_count / 2));
            max_frames = std::max(max_frames, (uint32_t)(sample_count / 2));
            auto now = std::chrono::steady_clock::now();
            if (now - window_start >= std::chrono::seconds(1)) {
                std::printf("[audio] %u buffers/s, %u frames/s (rate %u), per buffer %u..%u, queued %u B\n",
                            calls, frames, sample_rate, min_frames, max_frames, SDL_GetQueuedAudioSize(audio_device));
                calls = frames = max_frames = 0;
                min_frames = ~0u;
                window_start = now;
            }
        }
        static std::vector<float> swap_buffer;
        static std::array<float, duplicated_input_frames * input_channels> duplicated_sample_buffer;

        size_t resampled_sample_count = sample_count + duplicated_input_frames * input_channels;
        size_t max_sample_count = std::max(resampled_sample_count, resampled_sample_count * audio_convert.len_mult);
        if (max_sample_count > swap_buffer.size()) {
            swap_buffer.resize(max_sample_count);
        }
        for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
            swap_buffer[i] = duplicated_sample_buffer[i];
        }
        // Swap channels to undo the address xor from endianness handling.
        const float scale = audio_volume.load() * (0.5f / 32768.0f);
        for (size_t i = 0; i < sample_count; i += input_channels) {
            swap_buffer[i + 0 + duplicated_input_frames * input_channels] = audio_data[i + 1] * scale;
            swap_buffer[i + 1 + duplicated_input_frames * input_channels] = audio_data[i + 0] * scale;
        }
        if (sample_count <= duplicated_input_frames * input_channels) {
            return;
        }
        for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
            duplicated_sample_buffer[i] = swap_buffer[i + sample_count];
        }

        audio_convert.buf = reinterpret_cast<Uint8*>(swap_buffer.data());
        audio_convert.len = (sample_count + duplicated_input_frames * input_channels) * sizeof(swap_buffer[0]);
        if (SDL_ConvertAudio(&audio_convert) < 0) {
            fprintf(stderr, "SDL audio conversion failed: %s\n", SDL_GetError());
            return;
        }

        std::lock_guard device_lock{ audio_device_mutex };
        // The SDL queue holds converted audio: float frames of output_channels at output_sample_rate.
        uint64_t cur_queued_microseconds = uint64_t(SDL_GetQueuedAudioSize(audio_device)) /
                                           (output_channels * sizeof(float)) * 1000000 / output_sample_rate;
        uint32_t num_bytes_to_queue = audio_convert.len_cvt - output_channels * discarded_output_frames * sizeof(swap_buffer[0]);
        float* samples_to_queue = swap_buffer.data() + output_channels * discarded_output_frames / 2;

        // Safety valve: skip samples only if far too much audio is queued (normally the AI
        // event pacing keeps the queue near its low-water mark, ~40 ms).
        uint32_t skip_factor = cur_queued_microseconds / 200000;
        if (skip_factor != 0) {
            uint32_t skip_ratio = 1 << skip_factor;
            num_bytes_to_queue /= skip_ratio;
            for (size_t i = 0; i < num_bytes_to_queue / (output_channels * sizeof(swap_buffer[0])); i++) {
                samples_to_queue[2 * i + 0] = samples_to_queue[2 * skip_ratio * i + 0];
                samples_to_queue[2 * i + 1] = samples_to_queue[2 * skip_ratio * i + 1];
            }
        }
        SDL_QueueAudio(audio_device, samples_to_queue, num_bytes_to_queue);
    }

    size_t get_frames_remaining() {
        constexpr float buffer_offset_frames = 1.0f;
        std::lock_guard device_lock{ audio_device_mutex };
        uint64_t buffered_byte_count = SDL_GetQueuedAudioSize(audio_device);
        buffered_byte_count = buffered_byte_count * 2 * sample_rate / output_sample_rate / output_channels;
        uint32_t frames_per_vi = sample_rate / 60;
        if (buffered_byte_count > (buffer_offset_frames * bytes_per_frame * frames_per_vi)) {
            buffered_byte_count -= (buffer_offset_frames * bytes_per_frame * frames_per_vi);
        }
        else {
            buffered_byte_count = 0;
        }
        return static_cast<uint32_t>(buffered_byte_count / bytes_per_frame);
    }

    void update_audio_converter() {
        if (SDL_BuildAudioCVT(&audio_convert, AUDIO_F32, input_channels, sample_rate, AUDIO_F32, output_channels, output_sample_rate) < 0) {
            exit_error(SDL_GetError());
        }
        discarded_output_frames = duplicated_input_frames * output_sample_rate / sample_rate;
    }

    void set_frequency(uint32_t freq) {
        if (hyp_trace_enabled()) std::printf("[audio] game sample rate %u Hz\n", freq);
        sample_rate = freq;
        update_audio_converter();
    }

    // Open an output device ("" = system default). Keeps the current device on failure.
    bool open_audio_device(const std::string& name) {
        SDL_AudioSpec spec_desired{};
        spec_desired.freq = (int)output_sample_rate;
        spec_desired.format = AUDIO_F32;
        spec_desired.channels = (Uint8)output_channels;
        spec_desired.samples = 0x100;
        SDL_AudioDeviceID device = SDL_OpenAudioDevice(name.empty() ? nullptr : name.c_str(), false, &spec_desired, nullptr, 0);
        if (device == 0) {
            std::fprintf(stderr, "Could not open audio device '%s': %s\n", name.c_str(), SDL_GetError());
            return false;
        }
        {
            std::lock_guard device_lock{ audio_device_mutex };
            if (audio_device != 0) SDL_CloseAudioDevice(audio_device);
            audio_device = device;
        }
        SDL_PauseAudioDevice(device, 0);
        return true;
    }

    void reset_audio(uint32_t output_freq) {
        output_sample_rate = output_freq;
        const auto& s = hyp::settings::get();
        if (!open_audio_device(s.audio_output) && !open_audio_device("")) {
            exit_error(SDL_GetError());
        }
        update_audio_converter();
    }

    std::vector<std::string> audio_device_names(int is_capture) {
        std::vector<std::string> names;
        int count = SDL_GetNumAudioDevices(is_capture);
        for (int i = 0; i < count; i++) {
            if (const char* name = SDL_GetAudioDeviceName(i, is_capture)) names.emplace_back(name);
        }
        return names;
    }
}

#ifdef __ANDROID__
namespace {
    int SDLCALL on_app_event(void*, SDL_Event* event) {
        switch (event->type) {
            case SDL_APP_WILLENTERBACKGROUND:
                std::printf("[app] going to the background: game paused\n");
                ultramodern::set_paused(true);
                if (audio_device != 0) SDL_PauseAudioDevice(audio_device, 1);
                break;
            case SDL_APP_DIDENTERFOREGROUND:
                std::printf("[app] back in the foreground: game resumed\n");
                if (audio_device != 0) SDL_PauseAudioDevice(audio_device, 0);
                ultramodern::set_paused(false);
                hyp::vru::mic_reopen(); // Android hands background apps a silent microphone; start fresh
                break;
        }
        return 1;
    }
}
#endif

std::vector<std::string> hyp::audio::output_devices() { return audio_device_names(0); }
std::vector<std::string> hyp::audio::input_devices() { return audio_device_names(1); }
bool hyp::audio::set_output_device(const std::string& name) { return open_audio_device(name); }
void hyp::audio::set_volume(float volume) { audio_volume = std::clamp(volume, 0.0f, 1.0f); }

namespace {
    // ---- RSP ----

    RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
        if (task->t.type == M_AUDTASK) {
            static int logged = 0;
            if (logged < 3 && hyp_trace_enabled()) {
                logged++;
                std::printf("[aud] ucode=%08X ucode_data=%08X data_ptr=%08X data_size=%X flags=%X\n",
                            (uint32_t)task->t.ucode, (uint32_t)task->t.ucode_data,
                            (uint32_t)task->t.data_ptr, (uint32_t)task->t.data_size, (uint32_t)task->t.flags);
            }
            return n_aspMain;
        }
        fprintf(stderr, "Unknown RSP task type: %u\n", task->t.type);
        return nullptr;
    }

    void message_box(const char* msg) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Hey You, Pikachu! Recompiled", msg, window);
    }

    // Start the game once the renderer is up. Starting it before recomp::start
    // would skip the VI thread's dummy-mode setup (it only runs while no game is
    // started) and leave the VI mode null until the game's first osViSetMode.
    void on_gfx_init() {
        std::printf("[start] renderer up, starting the game\n");
        std::thread([] {
            ultramodern::sleep_milliseconds(100); // let a few VIs install the dummy mode
            recomp::start_game(game_id);
        }).detach();
    }

    std::string get_game_thread_name(const OSThread* t) {
        return "[Game] " + std::to_string(t->id);
    }

#ifdef _WIN32
    void print_stack(HANDLE process, HANDLE thread, CONTEXT ctx);

    // Print the exception and the native call stack (symbolized from the PDB) on a crash,
    // so failures inside recompiled code name the N64 function they happened in.
    LONG WINAPI crash_handler(EXCEPTION_POINTERS* info) {
        static bool handling = false;
        if (handling) return EXCEPTION_CONTINUE_SEARCH;
        handling = true;

        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(process, nullptr, TRUE);

        std::fprintf(stderr, "\n*** crash: exception 0x%08lX at %p", info->ExceptionRecord->ExceptionCode,
                     info->ExceptionRecord->ExceptionAddress);
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            std::fprintf(stderr, " (%s 0x%llX)", info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                         (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
        }
        std::fprintf(stderr, "\n");

        print_stack(process, GetCurrentThread(), *info->ContextRecord);
        std::fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void print_stack(HANDLE process, HANDLE thread, CONTEXT ctx) {
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = ctx.Rip; frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
        alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 256];
        for (int i = 0; i < 32 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx,
                                               nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); i++) {
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (SymFromAddr(process, frame.AddrPC.Offset, &disp, sym)) {
                IMAGEHLP_LINE64 line{ sizeof(IMAGEHLP_LINE64) };
                DWORD line_disp = 0;
                if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_disp, &line)) {
                    std::fprintf(stderr, "  #%02d %s+0x%llX (%s:%lu)\n", i, sym->Name, (unsigned long long)disp,
                                 line.FileName, line.LineNumber);
                } else {
                    std::fprintf(stderr, "  #%02d %s+0x%llX\n", i, sym->Name, (unsigned long long)disp);
                }
            } else {
                std::fprintf(stderr, "  #%02d %p\n", i, (void*)frame.AddrPC.Offset);
            }
        }
    }

    // Diagnostic: with HYP_DUMP_STACKS_AFTER=<seconds>, print every thread's stack
    // once (shows where the game is blocked, by N64 function name).
    void start_stack_dump_timer() {
        const char* env = std::getenv("HYP_DUMP_STACKS_AFTER");
        if (env == nullptr) return;
        int seconds = std::atoi(env);
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            HANDLE process = GetCurrentProcess();
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
            SymInitialize(process, nullptr, TRUE);
            DWORD self = GetCurrentThreadId();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{ sizeof(te) };
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
                HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
                if (th == nullptr) continue;
                SuspendThread(th);
                CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(th, &ctx)) {
                    PWSTR desc = nullptr;
                    char name[128] = "";
                    if (SUCCEEDED(GetThreadDescription(th, &desc)) && desc) {
                        WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, sizeof(name), nullptr, nullptr);
                        LocalFree(desc);
                    }
                    std::fprintf(stderr, "--- thread %lu %s\n", te.th32ThreadID, name);
                    print_stack(process, th, ctx);
                }
                ResumeThread(th);
                CloseHandle(th);
            }
            CloseHandle(snap);
            std::fflush(stderr);
        }).detach();
    }
#endif
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // GUI-subsystem exe: show a console only when diagnostics are requested and output
    // isn't already redirected (e.g. by a test harness).
    if ((std::getenv("HYP_CONSOLE") != nullptr || std::getenv("HYP_TRACE") != nullptr) &&
        GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN) {
        if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
            std::freopen("CONOUT$", "w", stdout);
            std::freopen("CONOUT$", "w", stderr);
        }
    }
#endif
    // Unbuffered so runtime/trace output survives a crash when redirected to a file.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    timeBeginPeriod(1);
    SetConsoleOutputCP(CP_UTF8);
    SetUnhandledExceptionFilter(crash_handler);
    start_stack_dump_timer();
#endif

    std::filesystem::path rom_arg;
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--rom") == 0) {
            rom_arg = argv[i + 1];
        }
    }

    // Saves, stored ROM, settings and config live next to the exe (kept off the system drive).
    std::filesystem::path data_dir = program_dir() / "data";
    std::filesystem::create_directories(data_dir);
#ifdef __ANDROID__
    hyp::android::install_crash_report(data_dir);
    // Android sets no HOME; RT64 keeps its settings in $HOME/.rt64 and fell back to /data
    // (not writable: it crashed creating the folder). Use the app's own storage.
    setenv("HOME", program_dir().string().c_str(), 1);
#endif
    hyp::settings::load(data_dir);

    std::printf("[start] settings loaded, opening audio\n");
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    reset_audio(48000);
    std::printf("[start] audio open\n");
    {
        const auto& s = hyp::settings::get();
        hyp::audio::set_volume(s.muted ? 0.0f : s.volume / 100.0f);
        // Microphone speech recognition (loads the model in the background).
        hyp::vru::mic_init(data_dir / "models" / "ggml-base.en.bin", s.microphone);
    }
    // The game's audio thread makes one 184-sample frame per AI buffer-completion event
    // (~174/s at 32 kHz), not one per video frame: pace AI events by buffer consumption.
    ultramodern::set_ai_buffer_pacing(true, 40);

    std::printf("[start] checking the ROM\n");
    recomp::register_config_path(data_dir);

    recomp::GameEntry game{
        .rom_hash = HYP_ROM_XXH3,
        .internal_name = "hey you, pikachu",
        .game_id = game_id,
        .mod_game_id = "hyp",
        .save_type = recomp::SaveType::Eep4k, // game only accepts osEepromProbe() == EEPROM_TYPE_4K
        .is_enabled = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
    };
    recomp::register_game(game);
    hyp::register_overlays();

    std::u8string id = game_id;
    recomp::check_all_stored_roms();
    if (!recomp::is_rom_valid(id)) {
        if (rom_arg.empty()) {
            rom_arg = program_dir() / "baserom.us.z64";
        }
#ifdef _WIN32
        // First run without a ROM next to the exe: let the player pick it.
        if (!std::filesystem::exists(rom_arg)) {
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn{ sizeof(ofn) };
            ofn.lpstrFilter = L"N64 ROM (*.z64;*.n64;*.v64)\0*.z64;*.n64;*.v64\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Select your Hey You, Pikachu! (USA) ROM";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (!GetOpenFileNameW(&ofn)) {
                return EXIT_SUCCESS; // cancelled
            }
            rom_arg = file;
        }
#endif
        auto rom_result = recomp::select_rom(rom_arg, id);
#ifdef __ANDROID__
        // The Java side copied the picked file to baserom.us.z64. The runtime keeps its own copy
        // once the ROM is accepted, and a wrong file must not stay: the next start asks again.
        std::error_code rom_ec;
        std::filesystem::remove(rom_arg, rom_ec);
#endif
        switch (rom_result) {
            case recomp::RomValidationError::Good:
                break;
            case recomp::RomValidationError::FailedToOpen:
#ifdef __ANDROID__
                exit_error("Could not open the ROM. Start the app again and choose your Hey You, Pikachu! (USA) file.");
#else
                exit_error("Could not open the ROM. Run with --rom \"<path to Hey You, Pikachu! (USA).z64>\".");
#endif
            case recomp::RomValidationError::IncorrectVersion:
                exit_error("This is a different version of Hey You, Pikachu!. Only the USA release is supported.");
            default:
                exit_error("This file is not a Hey You, Pikachu! (USA) ROM.");
        }
    }

    std::printf("[start] ROM ok, starting the runtime\n");
    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };
    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = hyp::renderer::create_render_context,
    };
    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };
    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };
    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = hyp::input::poll_inputs,
        .get_input = hyp::input::get_n64_input,
        .set_rumble = hyp::input::set_rumble,
        .get_connected_device_info = hyp::input::get_connected_device_info,
    };
    ultramodern::events::callbacks_t events_callbacks{
        .gfx_init_callback = on_gfx_init,
    };
    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = message_box,
    };
    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = get_game_thread_name,
    };

    recomp::start(
        recomp::Version{ 0, 1, 0 },
        {},
        rsp_callbacks,
        renderer_callbacks,
        audio_callbacks,
        input_callbacks,
        gfx_callbacks,
        events_callbacks,
        error_handling_callbacks,
        threads_callbacks
    );

#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return EXIT_SUCCESS;
}
