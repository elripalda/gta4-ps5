// Desktop CLI for the GTA IV / Episodes from Liberty City controller diagram.
//
// Converts a user-owned frontend texture dictionary by replacing only
// pack:/controller.dds, and installs or rolls back the TLAD/TBOGT loose
// dictionaries transactionally. The GTA IV base game is patched at runtime by
// the game code; this tool does not install base-game files on a console.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "frontend_controller_patch.h"
#include "sha256.h"

namespace fs = std::filesystem;
using gta4::button_prompts::LoadControllerDds;
using gta4::button_prompts::PatchFrontendController;
using gta4::tool::Sha256;
using gta4::tool::Sha256Digest;
using gta4::tool::Sha256File;
using gta4::tool::ToHex;

namespace {

constexpr size_t kMaximumInputBytes = 64u * 1024u * 1024u;

// Only these stock dictionaries are accepted without an explicit override.
// They are the retail files used to verify the byte-exact outputs below.
constexpr std::string_view kKnownDdsSha256 =
    "f844b50a5101d147ecd9b2b6b7933eb8313a3473cceee726ec0fefbb4c71048b";

struct GameInfo {
  std::string_view name;
  std::string_view stock_sha256;
  uintmax_t stock_size;
  std::string_view verified_output_sha256;
  uintmax_t verified_output_size;
  bool installable_loose;
  std::string_view ps5_path;
};

constexpr GameInfo kGames[] = {
    {"gta4", "265d90312d54c909de1160154f93a79b2fba487bc9b6044d556da07d9607082e", 121531,
     "3d6a1658fd80ed5307837445f00734c060efb51f3fdd5e2acf5b28aac0a0c620", 1430448, false,
     "(base game: patched at runtime, no loose install)"},
    {"tlad", "adaa899fa1f72f69f1e3aee2a23efe7b046b6f7f5ae857f84c4a0e96e754b254", 120907,
     "85ae8912d55f98be20ec0e213e9f4a6e09bc0c8f956d6187091b18add7c739b8", 1430448, true,
     "/data/homebrew/PPSA94004/UserData/LibertyRecomp/dlc/TLAD/xbox360/textures/frontend.xtd"},
    {"tbogt", "24ac4d7f8009b926336614b0a897927588ca6a91a70748381c7460d728e2367d", 124579,
     "19006797766794618bdbacb2f657f39f1271294c62e46ddb76ea5fbae66b9b35", 1446853, true,
     "/data/homebrew/PPSA94004/UserData/LibertyRecomp/dlc/TBOGT/xbox360/textures/frontend.xtd"},
};

const GameInfo* FindGame(std::string_view name) {
  for (const auto& game : kGames) {
    if (game.name == name) return &game;
  }
  return nullptr;
}

void PrintUsage() {
  std::fprintf(stderr,
               "usage:\n"
               "  gta4_frontend_controller_patch convert --game <gta4|tlad|tbogt>\n"
               "      --input <retail_frontend.xtd> --dds <controller_512.dds>\n"
               "      --output <new_frontend.xtd> [--allow-unknown-input]\n"
               "  gta4_frontend_controller_patch install --game <tlad|tbogt>\n"
               "      --source <converted_frontend.xtd> --target <frontend.xtd>\n"
               "      [--backup-dir <dir>] [--allow-unverified-source]\n"
               "      [--allow-unknown-target] [--apply]\n"
               "  gta4_frontend_controller_patch rollback --game <tlad|tbogt>\n"
               "      --target <frontend.xtd> --backup <frontend.xtd.bak>\n"
               "      [--allow-unknown-backup] [--apply]\n"
               "  gta4_frontend_controller_patch known\n"
               "\n"
               "install and rollback are dry runs unless --apply is given.\n");
}

std::optional<std::string> OptionValue(int argc, char** argv, std::string_view option) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (option == argv[index]) return std::string(argv[index + 1]);
  }
  return std::nullopt;
}

bool HasFlag(int argc, char** argv, std::string_view flag) {
  for (int index = 1; index < argc; ++index) {
    if (flag == argv[index]) return true;
  }
  return false;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& data, std::string& error) {
  std::error_code fs_error;
  const auto size = fs::file_size(path, fs_error);
  if (fs_error) {
    error = "could not stat '" + path + "': " + fs_error.message();
    return false;
  }
  if (size > kMaximumInputBytes) {
    error = "refusing '" + path + "': larger than 64 MiB";
    return false;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    error = "could not open '" + path + "'";
    return false;
  }
  data.assign(static_cast<size_t>(size), 0);
  if (!data.empty()) {
    stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (stream.gcount() != static_cast<std::streamsize>(data.size())) {
      error = "could not read '" + path + "'";
      return false;
    }
  }
  return true;
}

std::string TimestampUtc() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  std::tm utc = {};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char buffer[32] = {};
  std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc);
  return buffer;
}

bool SamePath(const std::string& left, const std::string& right) {
  std::error_code error;
  const fs::path a = fs::weakly_canonical(fs::absolute(left), error);
  error.clear();
  const fs::path b = fs::weakly_canonical(fs::absolute(right), error);
  return a == b;
}

bool ReplaceFile(const fs::path& temporary, const fs::path& target, std::string& error) {
#if defined(_WIN32)
  if (!MoveFileExW(temporary.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = "could not replace '" + target.string() + "'";
    return false;
  }
#else
  std::error_code fs_error;
  fs::rename(temporary, target, fs_error);
  if (fs_error) {
    error = "could not replace '" + target.string() + "': " + fs_error.message();
    return false;
  }
#endif
  return true;
}

bool WriteFileAtomic(const std::string& path, std::span<const uint8_t> data, std::string& error) {
  const fs::path target(path);
  const fs::path temporary = target.string() + ".f01-tmp-" + TimestampUtc();
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      error = "could not create '" + temporary.string() + "'";
      return false;
    }
    if (!data.empty()) {
      stream.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
    }
    stream.flush();
    if (!stream) {
      error = "could not write '" + temporary.string() + "'";
      std::error_code ignored;
      fs::remove(temporary, ignored);
      return false;
    }
  }
  Sha256Digest digest = {};
  std::string hash_error;
  if (!Sha256File(temporary.string(), digest, hash_error)) {
    error = "could not verify temporary file: " + hash_error;
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
  }
  if (ToHex(digest) != ToHex(Sha256(data))) {
    error = "temporary file did not match the requested bytes";
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
  }
  if (!ReplaceFile(temporary, target, error)) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
  }
  return true;
}

bool VerifyFile(const std::string& path, const std::string& expected_sha256,
                uintmax_t expected_size, std::string& actual_sha256, std::string& error) {
  std::error_code fs_error;
  const auto size = fs::file_size(path, fs_error);
  if (fs_error) {
    error = "could not stat '" + path + "': " + fs_error.message();
    return false;
  }
  if (expected_size != 0 && size != expected_size) {
    error = "'" + path + "' has size " + std::to_string(size) + ", expected " +
            std::to_string(expected_size);
    return false;
  }
  Sha256Digest digest = {};
  if (!Sha256File(path, digest, error)) return false;
  actual_sha256 = ToHex(digest);
  if (!expected_sha256.empty() && actual_sha256 != expected_sha256) {
    error = "'" + path + "' has SHA-256 " + actual_sha256 + ", expected " +
            std::string(expected_sha256);
    return false;
  }
  return true;
}

bool Convert(const std::string& game_name, const std::string& input_path,
             const std::string& dds_path, const std::string& output_path,
             bool allow_unknown_input) {
  const GameInfo* game = FindGame(game_name);
  if (!game) {
    std::fprintf(stderr, "error: unknown game '%s'\n", game_name.c_str());
    return false;
  }
  if (SamePath(input_path, output_path)) {
    std::fprintf(stderr, "error: refusing to overwrite the input dictionary; choose another output\n");
    return false;
  }

  std::vector<uint8_t> input;
  std::vector<uint8_t> dds;
  std::string error;
  if (!ReadFile(input_path, input, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return false;
  }
  if (!ReadFile(dds_path, dds, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return false;
  }

  Sha256Digest input_digest = Sha256(input);
  Sha256Digest dds_digest = Sha256(dds);
  const std::string input_sha256 = ToHex(input_digest);
  const std::string dds_sha256 = ToHex(dds_digest);
  const bool known_input = input_sha256 == game->stock_sha256 && input.size() == game->stock_size;
  const bool known_dds = dds_sha256 == kKnownDdsSha256;

  if (!known_input && !allow_unknown_input) {
    std::fprintf(stderr,
                 "error: '%s' is not the verified stock %s dictionary "
                 "(SHA-256 %s, %zu bytes).\n"
                 "       Pass --allow-unknown-input only if you understand the output will not "
                 "be byte-verified.\n",
                 input_path.c_str(), std::string(game->name).c_str(), input_sha256.c_str(),
                 input.size());
    return false;
  }

  std::vector<uint8_t> payload;
  if (!LoadControllerDds(dds_path, payload, error)) {
    std::fprintf(stderr, "error: controller DDS rejected: %s\n", error.c_str());
    return false;
  }

  std::vector<uint8_t> output;
  if (!PatchFrontendController(input, payload, output, error)) {
    std::fprintf(stderr, "error: dictionary rejected: %s\n", error.c_str());
    return false;
  }

  const std::string output_sha256 = ToHex(Sha256(output));
  if (known_input && known_dds) {
    if (output_sha256 != game->verified_output_sha256 || output.size() != game->verified_output_size) {
      std::fprintf(stderr,
                   "error: verified inputs produced an unexpected result "
                   "(SHA-256 %s, %zu bytes; expected %s, %ju bytes)\n",
                   output_sha256.c_str(), output.size(),
                   std::string(game->verified_output_sha256).c_str(), game->verified_output_size);
      return false;
    }
    std::printf("verified byte-exact output for %s\n", std::string(game->name).c_str());
  } else if (!known_dds) {
    std::printf("note: controller DDS is not the verified artwork; output is structurally "
                "validated but not byte-pinned\n");
  } else {
    std::printf("note: input dictionary is not a verified stock file; output is not byte-pinned\n");
  }

  if (!WriteFileAtomic(output_path, output, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return false;
  }
  std::printf("wrote %s\n  size: %zu\n  sha256: %s\n", output_path.c_str(), output.size(),
              output_sha256.c_str());
  return true;
}

bool Install(const GameInfo& game, const std::string& source_path, const std::string& target_path,
             const std::string& backup_dir, bool apply, bool allow_unverified_source,
             bool allow_unknown_target) {
  if (!game.installable_loose) {
    std::fprintf(stderr,
                 "error: %s is patched at runtime by the game; this tool only installs loose "
                 "TLAD/TBOGT dictionaries\n",
                 std::string(game.name).c_str());
    return false;
  }

  std::error_code fs_error;
  if (!fs::is_regular_file(target_path, fs_error)) {
    std::fprintf(stderr, "error: target '%s' is not a regular file\n", target_path.c_str());
    return false;
  }
  std::vector<uint8_t> source;
  std::string error;
  if (!ReadFile(source_path, source, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return false;
  }
  const std::string source_sha256 = ToHex(Sha256(source));
  if (source_sha256 != game.verified_output_sha256 && !allow_unverified_source) {
    std::fprintf(stderr,
                 "error: '%s' is not the verified %s clean dictionary. Pass "
                 "--allow-unverified-source only for a custom conversion.\n",
                 source_path.c_str(), std::string(game.name).c_str());
    return false;
  }

  std::string target_sha256;
  std::string target_error;
  if (!VerifyFile(target_path, "", 0, target_sha256, target_error)) {
    std::fprintf(stderr, "error: %s\n", target_error.c_str());
    return false;
  }
  if (target_sha256 == source_sha256) {
    std::printf("nothing to do: target already matches the requested dictionary\n");
    return true;
  }
  const bool stock_target = target_sha256 == game.stock_sha256;
  const bool known_clean_target = target_sha256 == game.verified_output_sha256;
  if (!stock_target && !known_clean_target && !allow_unknown_target) {
    std::fprintf(stderr,
                 "error: target '%s' is neither the verified stock nor the verified clean "
                 "dictionary. Pass --allow-unknown-target only after backing it up yourself.\n",
                 target_path.c_str());
    return false;
  }

  const fs::path backup_root =
      backup_dir.empty() ? fs::path(target_path).parent_path() : fs::path(backup_dir);
  const fs::path backup_path =
      backup_root / (fs::path(target_path).filename().string() + ".f01-backup-" + TimestampUtc() +
                     ".bak");

  std::printf("install plan:\n  game:   %s\n  source: %s\n  target: %s\n  backup: %s\n",
              std::string(game.name).c_str(), source_path.c_str(), target_path.c_str(),
              backup_path.string().c_str());
  std::printf("  target SHA-256: %s (%s)\n  source SHA-256: %s\n", target_sha256.c_str(),
              stock_target ? "stock" : "modified", source_sha256.c_str());
  if (!apply) {
    std::printf("dry run: pass --apply to back up the target and install\n");
    return true;
  }

  if (!fs::exists(backup_root)) {
    fs::create_directories(backup_root, fs_error);
    if (fs_error) {
      std::fprintf(stderr, "error: could not create backup directory: %s\n",
                   fs_error.message().c_str());
      return false;
    }
  }
  if (fs::exists(backup_path)) {
    std::fprintf(stderr, "error: backup path already exists: %s\n", backup_path.string().c_str());
    return false;
  }

  std::vector<uint8_t> target_bytes;
  if (!ReadFile(target_path, target_bytes, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return false;
  }
  if (!WriteFileAtomic(backup_path.string(), target_bytes, error)) {
    std::fprintf(stderr, "error: could not create backup: %s\n", error.c_str());
    return false;
  }
  std::string backup_sha256;
  if (!VerifyFile(backup_path.string(), target_sha256, target_bytes.size(), backup_sha256, error)) {
    std::fprintf(stderr, "error: backup verification failed: %s\n", error.c_str());
    return false;
  }
  std::printf("backup verified: %s\n", backup_path.string().c_str());

  if (!WriteFileAtomic(target_path, source, error)) {
    std::fprintf(stderr, "error: install failed: %s\n", error.c_str());
    std::fprintf(stderr, "restoring backup...\n");
    std::string restore_error;
    if (!WriteFileAtomic(target_path, target_bytes, restore_error)) {
      std::fprintf(stderr,
                   "FATAL: automatic restore failed: %s\n"
                   "Restore manually from %s\n",
                   restore_error.c_str(), backup_path.string().c_str());
    } else {
      std::fprintf(stderr, "backup restored\n");
    }
    return false;
  }
  std::string installed_sha256;
  if (!VerifyFile(target_path, source_sha256, source.size(), installed_sha256, error)) {
    std::fprintf(stderr, "error: installed dictionary verification failed: %s\n", error.c_str());
    std::fprintf(stderr, "restoring backup...\n");
    std::string restore_error;
    if (!WriteFileAtomic(target_path, target_bytes, restore_error)) {
      std::fprintf(stderr,
                   "FATAL: automatic restore failed: %s\n"
                   "Restore manually from %s\n",
                   restore_error.c_str(), backup_path.string().c_str());
    } else {
      std::fprintf(stderr, "backup restored\n");
    }
    return false;
  }
  std::printf("installed and verified: %s\n  sha256: %s\n", target_path.c_str(),
              installed_sha256.c_str());
  std::printf("rollback with:\n  gta4_frontend_controller_patch rollback --game %s "
              "--target \"%s\" --backup \"%s\" --apply\n",
              std::string(game.name).c_str(), target_path.c_str(), backup_path.string().c_str());
  return true;
}

bool Rollback(const GameInfo& game, const std::string& target_path,
              const std::string& backup_path, bool apply, bool allow_unknown_backup) {
  std::error_code fs_error;
  if (!fs::is_regular_file(target_path, fs_error)) {
    std::fprintf(stderr, "error: target '%s' is not a regular file\n", target_path.c_str());
    return false;
  }
  if (!fs::is_regular_file(backup_path, fs_error)) {
    std::fprintf(stderr, "error: backup '%s' is not a regular file\n", backup_path.c_str());
    return false;
  }

  std::vector<uint8_t> backup;
  std::string error;
  if (!ReadFile(backup_path, backup, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return false;
  }
  const std::string backup_sha256 = ToHex(Sha256(backup));
  if (backup_sha256 != game.stock_sha256 && !allow_unknown_backup) {
    std::fprintf(stderr,
                 "error: backup is not the verified stock %s dictionary. Pass "
                 "--allow-unknown-backup only if you trust this file.\n",
                 std::string(game.name).c_str());
    return false;
  }

  std::string target_sha256;
  if (!VerifyFile(target_path, "", 0, target_sha256, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return false;
  }
  if (target_sha256 == backup_sha256) {
    std::printf("nothing to do: target already matches the backup\n");
    return true;
  }

  const fs::path pre_rollback =
      fs::path(target_path).parent_path() /
      (fs::path(target_path).filename().string() + ".pre-rollback-" + TimestampUtc() + ".bak");
  std::printf("rollback plan:\n  game:   %s\n  target: %s\n  backup: %s\n  presave: %s\n",
              std::string(game.name).c_str(), target_path.c_str(), backup_path.c_str(),
              pre_rollback.string().c_str());
  std::printf("  target SHA-256: %s\n  backup SHA-256: %s\n", target_sha256.c_str(),
              backup_sha256.c_str());
  if (!apply) {
    std::printf("dry run: pass --apply to preserve the current file and restore the backup\n");
    return true;
  }

  std::vector<uint8_t> current;
  if (!ReadFile(target_path, current, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return false;
  }
  if (!WriteFileAtomic(pre_rollback.string(), current, error)) {
    std::fprintf(stderr, "error: could not preserve the current dictionary: %s\n", error.c_str());
    return false;
  }
  std::printf("current dictionary preserved: %s\n", pre_rollback.string().c_str());

  if (!WriteFileAtomic(target_path, backup, error)) {
    std::fprintf(stderr, "error: rollback failed: %s\n", error.c_str());
    return false;
  }
  std::string restored_sha256;
  if (!VerifyFile(target_path, backup_sha256, backup.size(), restored_sha256, error)) {
    std::fprintf(stderr, "error: restored dictionary verification failed: %s\n", error.c_str());
    std::fprintf(stderr, "current file was preserved at %s\n", pre_rollback.string().c_str());
    return false;
  }
  std::printf("rolled back and verified: %s\n  sha256: %s\n", target_path.c_str(),
              restored_sha256.c_str());
  return true;
}

void PrintKnown() {
  std::printf("verified inputs and byte-exact outputs (controller DDS %s):\n",
              std::string(kKnownDdsSha256).c_str());
  for (const auto& game : kGames) {
    std::printf("  %-6s stock %s (%ju bytes)\n         clean %s (%ju bytes)%s\n",
                std::string(game.name).c_str(), std::string(game.stock_sha256).c_str(),
                game.stock_size, std::string(game.verified_output_sha256).c_str(),
                game.verified_output_size,
                game.installable_loose ? "" : "  [runtime only]");
    if (game.installable_loose) {
      std::printf("         PS5 path: %s\n", std::string(game.ps5_path).c_str());
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 2;
  }
  const std::string_view command = argv[1];

  if (command == "known") {
    PrintKnown();
    return 0;
  }
  if (command == "convert") {
    const auto game = OptionValue(argc, argv, "--game");
    const auto input = OptionValue(argc, argv, "--input");
    const auto dds = OptionValue(argc, argv, "--dds");
    const auto output = OptionValue(argc, argv, "--output");
    if (!game || !input || !dds || !output) {
      PrintUsage();
      return 2;
    }
    return Convert(*game, *input, *dds, *output, HasFlag(argc, argv, "--allow-unknown-input"))
               ? 0
               : 1;
  }
  if (command == "install") {
    const auto game_name = OptionValue(argc, argv, "--game");
    const auto source = OptionValue(argc, argv, "--source");
    const auto target = OptionValue(argc, argv, "--target");
    if (!game_name || !source || !target) {
      PrintUsage();
      return 2;
    }
    const GameInfo* game = FindGame(*game_name);
    if (!game) {
      std::fprintf(stderr, "error: unknown game '%s'\n", game_name->c_str());
      return 2;
    }
    const auto backup_dir = OptionValue(argc, argv, "--backup-dir");
    return Install(*game, *source, *target, backup_dir.value_or(""),
                   HasFlag(argc, argv, "--apply"),
                   HasFlag(argc, argv, "--allow-unverified-source"),
                   HasFlag(argc, argv, "--allow-unknown-target"))
               ? 0
               : 1;
  }
  if (command == "rollback") {
    const auto game_name = OptionValue(argc, argv, "--game");
    const auto target = OptionValue(argc, argv, "--target");
    const auto backup = OptionValue(argc, argv, "--backup");
    if (!game_name || !target || !backup) {
      PrintUsage();
      return 2;
    }
    const GameInfo* game = FindGame(*game_name);
    if (!game) {
      std::fprintf(stderr, "error: unknown game '%s'\n", game_name->c_str());
      return 2;
    }
    return Rollback(*game, *target, *backup, HasFlag(argc, argv, "--apply"),
                    HasFlag(argc, argv, "--allow-unknown-backup"))
               ? 0
               : 1;
  }

  PrintUsage();
  return 2;
}
