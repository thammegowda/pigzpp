#include <gtest/gtest.h>
#include "test_support.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>
#endif

namespace fs = std::filesystem;

class CLITest : public ::testing::Test {
protected:
    std::string tmp_dir;
    std::string pigzpp_bin;

    void SetUp() override {
        tmp_dir = test_support::make_temp_dir("pigzpp_cli_");

        // Find the pigzpp binary (built in the same build tree)
        pigzpp_bin = "\"" + fs::path(PIGZPP_BIN_PATH).make_preferred().string() + "\"";
    }

    void TearDown() override {
        fs::remove_all(tmp_dir);
    }

    std::string path(const std::string& name) {
        return tmp_dir + "/" + name;
    }

    void write_file(const std::string& p, const std::string& content) {
        std::ofstream f(p, std::ios::binary);
        f.write(content.data(), content.size());
    }

    std::string read_file_content(const std::string& p) {
        std::ifstream f(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    int run(const std::string& args) {
#ifdef _WIN32
        std::string cmd = pigzpp_bin + " " + args + " 2>NUL";
#else
        std::string cmd = pigzpp_bin + " " + args + " 2>/dev/null";
#endif
        return system(cmd.c_str());
    }
};

TEST_F(CLITest, CompressFile) {
    auto orig = path("test.txt");
    write_file(orig, "Hello, World!\n");

    EXPECT_EQ(run("-k " + orig), 0);
    EXPECT_TRUE(fs::exists(orig + ".gz"));
    EXPECT_TRUE(fs::exists(orig)); // kept with -k
}

TEST_F(CLITest, DecompressFile) {
    auto orig = path("test.txt");
    auto gz = path("test.txt.gz");
    write_file(orig, "Hello, World!\n");

    EXPECT_EQ(run("-k " + orig), 0);
    fs::remove(orig);

    EXPECT_EQ(run("-d -k " + gz), 0);
    EXPECT_TRUE(fs::exists(orig));
    EXPECT_EQ(read_file_content(orig), "Hello, World!\n");
}

TEST_F(CLITest, StdoutMode) {
    auto orig = path("test.txt");
    auto out = path("out.gz");
    write_file(orig, "stdout test\n");

    std::string cmd = pigzpp_bin + " -c " + orig + " > " + out;
    EXPECT_EQ(system(cmd.c_str()), 0);
    EXPECT_TRUE(fs::exists(out));
    EXPECT_GT(fs::file_size(out), 0u);
}

TEST_F(CLITest, TestMode) {
    auto orig = path("test.txt");
    write_file(orig, "integrity test\n");
    EXPECT_EQ(run("-k " + orig), 0);
    EXPECT_EQ(run("-t " + path("test.txt.gz")), 0);
}

TEST_F(CLITest, KeepOriginal) {
    auto orig = path("keep.txt");
    write_file(orig, "keep me\n");
    EXPECT_EQ(run("-k " + orig), 0);
    EXPECT_TRUE(fs::exists(orig));
}

TEST_F(CLITest, VersionFlag) {
    EXPECT_EQ(run("-V"), 0);
}

TEST_F(CLITest, PipeMode) {
    auto orig = path("pipe.txt");
    auto gz = path("pipe.gz");
    auto restored = path("pipe_restored.txt");
    write_file(orig, "pipe test data\n");

#ifdef _WIN32
    const std::string cat = "type ";
#else
    const std::string cat = "cat ";
#endif
    std::string cmd = cat + "\"" + fs::path(orig).make_preferred().string() +
                      "\" | " + pigzpp_bin + " > " + gz;
    EXPECT_EQ(system(cmd.c_str()), 0);

    cmd = cat + "\"" + fs::path(gz).make_preferred().string() +
          "\" | " + pigzpp_bin + " -d > " + restored;
    EXPECT_EQ(system(cmd.c_str()), 0);
    EXPECT_EQ(read_file_content(restored), "pipe test data\n");
}

TEST_F(CLITest, VerboseProgressOnlyOnStderr) {
    auto orig = path("progress.txt"), gz = path("progress.gz");
    auto err = path("progress.err"), restored = path("progress_restored.txt");
    std::string data(300000, 'p');
    write_file(orig, data);

    // -v -c and -v -dc: clean data on stdout, single final summary on redirected stderr.
    EXPECT_EQ(system((pigzpp_bin + " -v -c " + orig + " > " + gz + " 2> " + err).c_str()), 0);
    EXPECT_NE(read_file_content(err).find("100.0% 293.0KiB/293.0KiB -> "), std::string::npos);

    EXPECT_EQ(system((pigzpp_bin + " -v -d -c " + gz + " > " + restored + " 2> " + err).c_str()), 0);
    EXPECT_EQ(read_file_content(restored), data);
    EXPECT_NE(read_file_content(err).find(" -> 293.0KiB "), std::string::npos);

    // Default verbosity: silent stderr.
    EXPECT_EQ(system((pigzpp_bin + " -c " + orig + " > " + gz + " 2> " + err).c_str()), 0);
    EXPECT_EQ(read_file_content(err), "");
}

#if !defined(_WIN32)
TEST_F(CLITest, ProgressTerminalWidthAndErrorNewline) {
    auto run_pty = [](unsigned short width, const std::vector<const char*>& args, int stall_ms = 0) {
        int master = posix_openpt(O_RDWR | O_NOCTTY), out_pipe[2] = {-1, -1};
        if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 || ::pipe(out_pipe) != 0)
            return std::string{};
        int slave = ::open(ptsname(master), O_RDWR | O_NOCTTY);
        struct winsize window{24, width, 0, 0};
        ioctl(slave, TIOCSWINSZ, &window);
        struct termios term{};
        tcgetattr(slave, &term);
        term.c_oflag &= ~ONLCR;
        tcsetattr(slave, TCSANOW, &term);

        pid_t child = fork();
        if (child == 0) {
            dup2(out_pipe[1], 1);
            dup2(slave, 2);
            ::close(out_pipe[0]); ::close(out_pipe[1]); ::close(master); ::close(slave);
            std::vector<char*> argv;
            for (const char* a : args) argv.push_back(const_cast<char*>(a));
            argv.push_back(nullptr);
            execv(PIGZPP_BIN_PATH, argv.data());
            std::_Exit(127);
        }
        ::close(slave);
        ::close(out_pipe[1]);
        if (stall_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
        char buf[4096];
        while (::read(out_pipe[0], buf, sizeof buf) > 0) {}
        ::close(out_pipe[0]);

        std::string output;
        for (;;) {
            ssize_t got = ::read(master, buf, sizeof buf);
            if (got > 0) output.append(buf, static_cast<size_t>(got));
            else if (got == 0 || errno != EINTR) break;
        }
        ::close(master);
        waitpid(child, nullptr, 0);
        return output;
    };

    auto orig = path("progress.txt"), gz = path("good.gz"), bad = path("bad.gz");
    write_file(orig, std::string(1000000, 'p'));

    for (unsigned short width : {80, 24, 6}) {
        std::string out = run_pty(width, {PIGZPP_BIN_PATH, "-v", "-c", orig.c_str()});
        size_t cr = out.rfind('\r');
        if (width > 6) {
            ASSERT_NE(cr, std::string::npos);
            std::string line = out.substr(cr + 1, out.find('\n', cr) - cr - 1);
            EXPECT_LT(line.size(), width);
            EXPECT_EQ(line.find(" -> ") != std::string::npos, width == 80);
        } else {
            EXPECT_EQ(cr, std::string::npos);
            EXPECT_NE(out.find("100.0%"), std::string::npos);
        }
    }

    // Mid-stream corruption after a live \r redraw must emit '\n' before the error.
    ASSERT_EQ(run("-c " + orig + " > " + gz), 0);
    std::string member = read_file_content(gz), two = member + member;
    two.back() ^= 0xff;
    write_file(bad, two);
    std::string out = run_pty(80, {PIGZPP_BIN_PATH, "-v", "-dc", "-p", "1", bad.c_str()}, 550);
    ASSERT_NE(out.find('\r'), std::string::npos);
    size_t err_pos = out.rfind("pigzpp: ");
    ASSERT_NE(err_pos, std::string::npos);
    EXPECT_EQ(out[err_pos - 1], '\n');
}
#endif
