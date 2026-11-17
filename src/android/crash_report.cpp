// Android crash report: stdout/stderr go to files/data/log.txt (bionic also writes assert
// and abort messages to stderr), and a fatal signal or an uncaught C++ exception writes
// files/data/crash.txt (signal, fault address, registers, call stack as library+offset).
// LauncherActivity shows both on the next start with a Copy button.
// Offsets are symbolized on the PC against the unstripped libmain.so of the same build:
//   llvm-symbolizer --obj=build/android/symbols/libmain-<version>.so 0x<offset>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <typeinfo>

namespace {
    char crash_path[512];
    struct sigaction old_actions[32];

    // async-signal-safe output helpers
    void put(int fd, const char* s) {
        if (s != nullptr) (void)!write(fd, s, strlen(s));
    }
    void put_hex(int fd, uintptr_t v) {
        char buf[19] = "0x";
        char tmp[16];
        int n = 0;
        do { tmp[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v != 0 && n < 16);
        for (int i = 0; i < n; i++) buf[2 + i] = tmp[n - 1 - i];
        buf[2 + n] = 0;
        put(fd, buf);
    }
    void put_dec(int fd, long v) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%ld", v);
        put(fd, buf);
    }

    void put_frame(int fd, int index, uintptr_t pc) {
        put(fd, "  #"); put_dec(fd, index); put(fd, " pc "); put_hex(fd, pc);
        Dl_info info{};
        if (dladdr((void*)pc, &info) != 0 && info.dli_fname != nullptr) {
            const char* lib = strrchr(info.dli_fname, '/');
            put(fd, "  "); put(fd, lib != nullptr ? lib + 1 : info.dli_fname);
            put(fd, "+"); put_hex(fd, pc - (uintptr_t)info.dli_fbase);
            if (info.dli_sname != nullptr) {
                put(fd, "  ("); put(fd, info.dli_sname); put(fd, "+"); put_hex(fd, pc - (uintptr_t)info.dli_saddr); put(fd, ")");
            }
        }
        put(fd, "\n");
    }

    struct Unwind { int fd; int count; };
    _Unwind_Reason_Code unwind_step(_Unwind_Context* ctx, void* arg) {
        auto* u = static_cast<Unwind*>(arg);
        uintptr_t pc = _Unwind_GetIP(ctx);
        if (pc != 0) put_frame(u->fd, u->count, pc);
        return ++u->count >= 48 ? _URC_END_OF_STACK : _URC_NO_REASON;
    }

    void write_header(int fd, const char* what) {
        put(fd, "Hey You, Pikachu! (Android) crashed: "); put(fd, what); put(fd, "\n");
        char name[32] = "";
        prctl(PR_GET_NAME, name, 0, 0, 0);
        put(fd, "thread: "); put(fd, name); put(fd, " (tid "); put_dec(fd, gettid()); put(fd, ")\n");
    }

    const char* signal_name(int sig) {
        switch (sig) {
            case SIGSEGV: return "SIGSEGV (bad memory access)";
            case SIGBUS: return "SIGBUS";
            case SIGILL: return "SIGILL (illegal instruction)";
            case SIGFPE: return "SIGFPE";
            case SIGABRT: return "SIGABRT (abort)";
            case SIGTRAP: return "SIGTRAP";
            default: return "signal";
        }
    }

    void on_signal(int sig, siginfo_t* info, void* uctx) {
        int fd = open(crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write_header(fd, signal_name(sig));
            put(fd, "signal "); put_dec(fd, sig); put(fd, " code "); put_dec(fd, info->si_code);
            put(fd, " fault address "); put_hex(fd, (uintptr_t)info->si_addr); put(fd, "\n");
#if defined(__aarch64__)
            auto* uc = static_cast<ucontext_t*>(uctx);
            put(fd, "pc/lr/sp: "); put_hex(fd, uc->uc_mcontext.pc); put(fd, " ");
            put_hex(fd, uc->uc_mcontext.regs[30]); put(fd, " "); put_hex(fd, uc->uc_mcontext.sp); put(fd, "\n");
            put(fd, "crash site:\n");
            put_frame(fd, 0, uc->uc_mcontext.pc);
            put_frame(fd, 1, uc->uc_mcontext.regs[30]);
#endif
            put(fd, "stack:\n");
            Unwind u{ fd, 0 };
            _Unwind_Backtrace(unwind_step, &u);
            put(fd, "\n");
            fsync(fd);
            close(fd);
        }
        fflush(stdout);
        // Let the system's handler (or the default action) finish the process.
        sigaction(sig, &old_actions[sig], nullptr);
        raise(sig);
    }

    void on_terminate() {
        std::string what = "uncaught C++ exception";
        if (auto e = std::current_exception()) {
            try { std::rethrow_exception(e); }
            catch (const std::exception& ex) { what += std::string(" (") + typeid(ex).name() + "): " + ex.what(); }
            catch (...) { what += " (not a std::exception)"; }
        }
        int fd = open(crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write_header(fd, what.c_str());
            Unwind u{ fd, 0 };
            _Unwind_Backtrace(unwind_step, &u);
            fsync(fd);
            close(fd);
        }
        abort();
    }
}

namespace hyp::android {
    void install_crash_report(const std::filesystem::path& data_dir) {
        std::filesystem::create_directories(data_dir);
        std::string log = (data_dir / "log.txt").string();
        std::error_code ec;
        std::filesystem::rename(data_dir / "log.txt", data_dir / "log_prev.txt", ec); // keep the last session's
        if (std::freopen(log.c_str(), "w", stdout) != nullptr) {
            dup2(fileno(stdout), STDERR_FILENO);
            std::setvbuf(stdout, nullptr, _IONBF, 0);
            std::setvbuf(stderr, nullptr, _IONBF, 0);
        }
        snprintf(crash_path, sizeof(crash_path), "%s", (data_dir / "crash.txt").string().c_str());
        struct sigaction sa{};
        sa.sa_sigaction = on_signal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK; // bionic gives every thread an alternate signal stack
        sigemptyset(&sa.sa_mask);
        for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP }) {
            sigaction(sig, &sa, &old_actions[sig]);
        }
        std::set_terminate(on_terminate);
        std::printf("[start] crash report installed, log %s\n", log.c_str());
    }
}
