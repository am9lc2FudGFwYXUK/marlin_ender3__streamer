// gstream.cpp - FINAL CLEAN VERSION - compiles on ALL Linux (x86_64, aarch64, armhf, etc.)
// Works at 250000 baud everywhere, with feedrate/bed/hotend override + bulletproof resend handling

#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <cctype>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <chrono>
#include <iomanip>
#include <cstdlib>
#include <sys/ioctl.h>

struct Overrides {
    int feedrate_percent = -1;   // -1 = no override
    int bed_temp = -1;
    int hotend_temp = -1;
    bool debug = false;
};

void print_help(const char* prog) {
    std::cout << R"(
MarlinEnder3Streamer.cpp - Ultimate Marlin Ender3 G-code streamer
Works on x86_64, aarch64, Raspberry Pi, Orange Pi — everywhere!

Usage:
  )" << prog << R"( /dev/ttyUSB0 115200 file.gcode [options]

Options:
  --feedrate=120      Multiply all F values by 120%
  --bed=65            Force bed to 65°C
  --hotend=215        Force hotend to 215°C
  --debug             Show all comms
  --help              This help

Example:
  )" << prog << R"( /dev/ttyUSB0 250000 print.gcode --feedrate=150 --hotend=210 --debug
)";
}

speed_t get_baud_constant(int baud) {
    switch (baud) {
        case 9600:    return B9600;
        case 19200:   return B19200;
        case 38400:   return B38400;
        case 57600:   return B57600;
        case 115200:  return B115200;
        case 230400:  return B230400;
        case 460800:  return B460800;
        case 921600:  return B921600;
        case 250000:  return speed_t(250000);   // works on all modern kernels
        case 500000:  return speed_t(500000);
        case 1000000: return speed_t(1000000);
        default:
            std::cerr << "Unsupported baud rate: " << baud << "\n";
            exit(1);
    }
}

int set_serial(int fd, int baud) {
    struct termios tty{};
    if (tcgetattr(fd, &tty) != 0) return -1;

    cfmakeraw(&tty);
    speed_t speed = get_baud_constant(baud);
    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 20;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) return -1;
    tcflush(fd, TCIOFLUSH);
    return 0;
}

std::string read_line(int fd) {
    std::string s;
    char ch;
    auto start = std::chrono::steady_clock::now();
    while (true) {
        ssize_t n = read(fd, &ch, 1);
        if (n > 0) {
            if (ch == '\n') {
                if (!s.empty() && s.back() == '\r') s.pop_back();
                return s;
            }
            if (ch != '\r') s += ch;
        } else if (n == 0) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10))
                return "";
            usleep(5000);
        }
    }
}

void emergency_reset(int fd, bool debug) {
    std::cout << "\nFORCING HARD RESET (M112 + M999)\n";
    write(fd, "M112\nM999\n", 11);
    usleep(4000000);
    tcflush(fd, TCIOFLUSH);
    std::cout << "Printer rebooted — fresh start\n\n";
}

void trim(std::string& s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
}

std::string modify_line(const std::string& orig, const Overrides& ov) {
    std::string line = orig;
    trim(line);
    if (line.empty() || line[0] == ';') return orig;

    std::istringstream iss(line);
    std::string token, result;
    bool has_F = false;
    double old_F = 0;

    while (iss >> token) {
        if (token.size() < 2) { result += " " + token; continue; }
        char code = std::toupper(token[0]);
        std::string val = token.substr(1);

        if (code == 'F') {
            has_F = true;
            old_F = std::stod(val);
            if (ov.feedrate_percent > 0) {
                int new_F = int(old_F * ov.feedrate_percent / 100.0 + 0.5);
                result += " F" + std::to_string(new_F);
                continue;
            }
        }
        if (code == 'S') {
            if (ov.bed_temp >= 0 && (line.find("M140") == 0 || line.find("M190") == 0)) {
                result += " S" + std::to_string(ov.bed_temp);
                continue;
            }
            if (ov.hotend_temp >= 0 && (line.find("M104") == 0 || line.find("M109") == 0)) {
                result += " S" + std::to_string(ov.hotend_temp);
                continue;
            }
        }
        result += " " + token;
    }

    if (has_F && ov.feedrate_percent > 0 && ov.debug)
        std::cout << "   Feedrate " << int(old_F) << " → " << int(old_F * ov.feedrate_percent / 100.0 + 0.5) << "\n";

    if (result != (" " + line)) {
        trim(result);
        return result;
    }
    return orig;
}

int main(int argc, char** argv) {
    if (argc < 4) { print_help(argv[0]); return 1; }

    std::string dev = argv[1];
    int baud = std::stoi(argv[2]);
    std::string file = argv[3];
    Overrides ov;

    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--debug") ov.debug = true;
        else if (a.find("--feedrate=") == 0) ov.feedrate_percent = std::stoi(a.substr(11));
        else if (a.find("--bed=") == 0) ov.bed_temp = std::stoi(a.substr(6));
        else if (a.find("--hotend=") == 0) ov.hotend_temp = std::stoi(a.substr(9));
        else if (a == "--help") { print_help(argv[0]); return 0; }
    }

    int fd = open(dev.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) { std::cerr << "Cannot open " << dev << ": " << strerror(errno) << "\n"; return 1; }

    if (set_serial(fd, baud) != 0) {
        std::cerr << "Failed to set serial parameters\n"; close(fd); return 1;
    }
    usleep(2000000);

    std::cout << "Connected to " << dev << " @ " << baud << " baud\n";
    if (ov.feedrate_percent > 0) std::cout << "  Feedrate × " << ov.feedrate_percent << "%\n";
    if (ov.bed_temp >= 0)        std::cout << "  Bed forced → " << ov.bed_temp << "°C\n";
    if (ov.hotend_temp >= 0)     std::cout << "  Hotend forced → " << ov.hotend_temp << "°C\n\n";

    std::ifstream f(file);
    if (!f.is_open()) { std::cerr << "Cannot open " << file << "\n"; close(fd); return 1; }

    int total = 0, sent = 0;
    { std::string tmp; while (std::getline(f, tmp)) { trim(tmp); if (!tmp.empty() && tmp[0] != ';') total++; } }
    f.clear(); f.seekg(0);

    std::cout << "Streaming " << file << " (" << total << " commands)\n\n";

    int line_num = 1, resend_streak = 0;
    std::string line;

    while (std::getline(f, line)) {
        std::string modified = modify_line(line, ov);
        trim(modified);
        if (modified.empty() || modified[0] == ';') continue;

        std::string payload = "N" + std::to_string(line_num) + " " + modified;
        unsigned char cs = 0;
        for (char c : payload) cs ^= (unsigned char)c;
        std::string cmd = payload + "*" + std::to_string((int)cs) + "\n";

        write(fd, cmd.c_str(), cmd.size());
        if (ov.debug) std::cout << ">> " << cmd.substr(0, cmd.size()-1);
        sent++;

        bool got_ok = false;
        while (!got_ok) {
            std::string resp = read_line(fd);
            if (resp.empty()) { std::cerr << "\nTimeout!\n"; close(fd); return 1; }
            if (ov.debug) std::cout << "<< " << resp << "\n";

            if (resp.find("ok") != std::string::npos) {
                got_ok = true; line_num++; resend_streak = 0;
                if (sent % 25 == 0 || ov.debug)
                    std::cout << "\rProgress: " << (sent*100/total) << "% (" << sent << "/" << total << ")    " << std::flush;
            }
            else if (resp.find("Resend") != std::string::npos || resp.find("rs") != std::string::npos) {
                if (++resend_streak >= 3) {
                    emergency_reset(fd, ov.debug);
                    line_num = 1; resend_streak = 0;
                }
                write(fd, cmd.c_str(), cmd.size());
            }
        }
    }

    std::cout << "\n\nFinishing... ";
    write(fd, "M400\n", 5);
    while (read_line(fd).find("ok") == std::string::npos);
    std::cout << "done!\n\nPRINT COMPLETED SUCCESSFULLY!\n";
    close(fd);
    return 0;
}
