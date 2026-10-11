#pragma once

// Signal names and numbers as Node has them (os.constants.signals), for
// process.kill and the os module. Windows has Node's emulated set: the numbers
// libuv uses there, of which SIGINT, SIGQUIT, SIGTERM and SIGKILL end a
// process and 0 asks whether it exists.

#include <cstring>
#include <span>

#ifndef _WIN32
#include <csignal>
#endif

namespace brokit::api {

struct SignalName {
    const char* name;
    int number;
};

inline std::span<const SignalName> signalNames() {
#ifdef _WIN32
    static const SignalName kSignals[] = {
        {"SIGHUP", 1},  {"SIGINT", 2},    {"SIGQUIT", 3},  {"SIGILL", 4},    {"SIGFPE", 8},
        {"SIGKILL", 9}, {"SIGSEGV", 11},  {"SIGTERM", 15}, {"SIGBREAK", 21}, {"SIGABRT", 22},
        {"SIGWINCH", 28},
    };
#else
    static const SignalName kSignals[] = {
        {"SIGHUP", SIGHUP},     {"SIGINT", SIGINT},       {"SIGQUIT", SIGQUIT},   {"SIGILL", SIGILL},
        {"SIGTRAP", SIGTRAP},   {"SIGABRT", SIGABRT},     {"SIGIOT", SIGIOT},     {"SIGBUS", SIGBUS},
        {"SIGFPE", SIGFPE},     {"SIGKILL", SIGKILL},     {"SIGUSR1", SIGUSR1},   {"SIGSEGV", SIGSEGV},
        {"SIGUSR2", SIGUSR2},   {"SIGPIPE", SIGPIPE},     {"SIGALRM", SIGALRM},   {"SIGTERM", SIGTERM},
        {"SIGCHLD", SIGCHLD},   {"SIGCONT", SIGCONT},     {"SIGSTOP", SIGSTOP},   {"SIGTSTP", SIGTSTP},
        {"SIGTTIN", SIGTTIN},   {"SIGTTOU", SIGTTOU},     {"SIGURG", SIGURG},     {"SIGXCPU", SIGXCPU},
        {"SIGXFSZ", SIGXFSZ},   {"SIGVTALRM", SIGVTALRM}, {"SIGPROF", SIGPROF},   {"SIGWINCH", SIGWINCH},
        {"SIGIO", SIGIO},       {"SIGSYS", SIGSYS},
#ifdef SIGPOLL
        {"SIGPOLL", SIGPOLL},
#endif
#ifdef SIGPWR
        {"SIGPWR", SIGPWR},
#endif
#ifdef SIGSTKFLT
        {"SIGSTKFLT", SIGSTKFLT},
#endif
#ifdef SIGINFO
        {"SIGINFO", SIGINFO},
#endif
    };
#endif
    return kSignals;
}

// The number for a name ("SIGTERM"), or -1.
inline int signalNumber(const char* name) {
    for (const SignalName& s : signalNames())
        if (std::strcmp(s.name, name) == 0) return s.number;
    return -1;
}

// The name for a number (SIGTERM for 15), or null. Where two names share a
// number (SIGABRT/SIGIOT, SIGIO/SIGPOLL) the first listed, Node's, wins.
inline const char* signalName(int number) {
    for (const SignalName& s : signalNames())
        if (s.number == number) return s.name;
    return nullptr;
}

}  // namespace brokit::api
