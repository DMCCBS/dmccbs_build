#include <filesystem>
#include <fstream>
#include <iostream>
#include <pstream.h>
#include <sha256.hh>
#include <string>
#include <syncstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

void createDirs() {
  // create necessary directories
  // ./src ./include ./lib ./.obj_cache ./bin ./buildflags
  fs::create_directories("./src");
  fs::create_directories("./include");
  fs::create_directories("./lib");
  fs::create_directories("./.obj_cache");
  fs::create_directories("./bin");
  fs::create_directories("./buildflags");
}

void createConfigFile(const std::string &configFile) {
  if (!fs::exists("./buildflags/" + configFile + ".sh")) {
    std::ofstream preprocessorFile("./buildflags/" + configFile + ".sh");
    preprocessorFile << "#!/bin/env sh" << std::endl;
    preprocessorFile.close();
    fs::permissions("./buildflags/" + configFile + ".sh",
                    fs::perms::owner_all | fs::perms::group_all |
                        fs::perms::others_read | fs::perms::others_exec);
  }
}

void createConfigFiles() {
  createConfigFile("preprocessor");
  createConfigFile("linker");
  createConfigFile("prebuild");
  createConfigFile("postbuild");
}

std::string readFileArgs(const std::string &configFile) {
  redi::ipstream proc("./buildflags/" + configFile + ".sh",
                      redi::pstreams::pstdout | redi::pstreams::pstderr);
  std::string out = "";
  std::string line;
  while (std::getline(proc.out(), line))
    out += line + " ";

  return out;
}

struct file_describer {
  fs::path name;
  std::string hash;
};

std::vector<file_describer> getFileHashes(const std::string &preprocessorArgs,
                                          bool DEBUG) {
  std::vector<file_describer> fileHashes;
  for (auto const &dir_entry : fs::recursive_directory_iterator("./src")) {
    if (!dir_entry.is_directory()) {
      // clang++ -E -I./include/ $preprocessorArgs $dir_entry
      if (DEBUG) {
        std::cout << "Preprocessing " << dir_entry.path() << std::endl;
        std::cout << "Command: "
                  << "clang++ -E -I./include/ " + preprocessorArgs +
                         dir_entry.path().string()
                  << std::endl;
      }
      redi::ipstream proc("clang++ -E -I./include/ " + preprocessorArgs +
                              dir_entry.path().string(),
                          redi::pstreams::pstdout);

      SHA256 sha256stream;
      std::string line;
      while (std::getline(proc.out(), line)) {
        sha256stream.add(line.c_str(), line.length());
      }
      fileHashes.push_back({dir_entry.path(), sha256stream.getHash()});
    }
  }
  return fileHashes;
}

void compileFiles(const std::vector<file_describer> &files,
                  const std::string &preprocessorArgs,
                  const std::string &linkerArgs, bool DEBUG, bool DEV,
                  int offset = 0, int step = 1) {
  for (size_t i = offset; i < files.size(); i += step) {
    // auto fileName = file.name.filename();
    // auto filePath = file.name.parent_path();
    // auto fileExtension = file.name.extension();
    // auto fileNameNoExtension = file.name.stem();
    if (!fs::exists("./.obj_cache/" + files[i].hash + ".o")) {
      if (DEBUG) {
        std::osyncstream(std::cout)
            << "Compiling " << files[i].name << " (Hash: " << files[i].hash
            << ")" << std::endl;
      }

      // clang++ -I./include/ (./buildflags/preprocessor.sh) -c $src_file -o
      // $obj_file &

      std::string command = "clang++ -I./include/ " + preprocessorArgs +
                            (DEV ? "-O2 " : "") + " -c " +
                            files[i].name.string() + " -o " +
                            ("./.obj_cache/" + files[i].hash + ".o");

      system(command.c_str());
    }
  }
}

void batchCompileFiles(const std::vector<file_describer> &files,
                       const std::string &preprocessorArgs,
                       const std::string &linkerArgs, bool DEBUG, bool DEV,
                       int n_threads) {
  std::vector<std::jthread> threads;
  if (!n_threads) {
    return compileFiles(files, preprocessorArgs, linkerArgs, DEBUG, DEV);
  }
  if (n_threads > files.size()) {
    n_threads = files.size();
  }

  for (int i = 0; i < n_threads; i++) {
    threads.push_back(std::jthread(
        [&files, &preprocessorArgs, &linkerArgs, i, n_threads, DEBUG, DEV]() {
          compileFiles(files, preprocessorArgs, linkerArgs, DEBUG, DEV, i,
                       n_threads);
        }));
  }
}

void linkExec(const std::vector<file_describer> &files,
              const std::string &preprocessorArgs,
              const std::string &linkerArgs, bool DEBUG, bool DEV) {

  std::string LINKER;

  if (std::getenv("DMC_LINKER") != NULL)
    LINKER = std::getenv("DMC_LINKER");
  else
    LINKER = (fs::exists("/usr/local/bin/mold") ? "mold" : "ld");

  std::string command = "clang++ -fuse-ld=" + LINKER + " -L./lib/ " +
                        preprocessorArgs + (DEV ? "-O2 -flto " : "") +
                        "-o ./bin/main ";

  if (DEBUG) {
    std::cout << "Linking " << command << std::endl;
  }

  for (const auto &file : files) {
    command += ("./.obj_cache/" + file.hash + ".o ");
  }

  command += linkerArgs;

  system(command.c_str());
}

int main(int argc, char **argv) {
  // convert arguments to vector
  std::vector<std::string> args(argv, argv + argc);

  bool DEBUG = std::getenv("DMC_DEBUG") != nullptr;
  bool STHREAD = std::getenv("DMC_STHREAD") != nullptr;
  bool DEV = std::getenv("DMC_DEV") != nullptr; // Optimize if false

  createDirs();
  createConfigFiles();

  readFileArgs("prebuild");

  std::string preprocessorArgs = readFileArgs("preprocessor");
  std::string linkerArgs = readFileArgs("linker");

  if (DEBUG) {
    std::cout << "preprocessorArgs: " << preprocessorArgs << std::endl;
    std::cout << "linkerArgs: " << linkerArgs << std::endl;
  }
  auto hashes = getFileHashes(preprocessorArgs, DEBUG);

  if (DEBUG) {
    for (auto const &hash : hashes) {
      std::cout << hash.name << " " << hash.hash << std::endl;
    }
  }
  if (STHREAD)
    compileFiles(hashes, preprocessorArgs, linkerArgs, DEBUG, DEV);
  else
    batchCompileFiles(hashes, preprocessorArgs, linkerArgs, DEBUG, DEV,
                      std::thread::hardware_concurrency());

  // Link final executable
  linkExec(hashes, preprocessorArgs, linkerArgs, DEBUG, DEV);

  if (DEBUG) {
    std::cout << "Complete; Running postbuild" << std::endl;
  }

  readFileArgs("postbuild");

  return 0;
}
