#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

class CLITest : public ::testing::Test {
protected:
    std::string tmp_dir;
    std::string pigzpp_bin;

    void SetUp() override {
        fs::create_directories("tmp");
        char tmpl[] = "tmp/pigzpp_cli_XXXXXX";
        char* dir = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        tmp_dir = dir;

        // Find the pigzpp binary (built in the same build tree)
        pigzpp_bin = PIGZPP_BIN_PATH;
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
        std::string cmd = pigzpp_bin + " " + args + " 2>/dev/null";
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

    std::string cmd = "cat " + orig + " | " + pigzpp_bin + " > " + gz;
    EXPECT_EQ(system(cmd.c_str()), 0);

    cmd = "cat " + gz + " | " + pigzpp_bin + " -d > " + restored;
    EXPECT_EQ(system(cmd.c_str()), 0);
    EXPECT_EQ(read_file_content(restored), "pipe test data\n");
}
