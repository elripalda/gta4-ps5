#include "mod_files.h"

#include <algorithm>
#include <system_error>

namespace gta4ps5 {
namespace fs = std::filesystem;

namespace {

std::string Lower(std::string value) {
  for (char& c : value)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return value;
}

bool PathLess(const fs::path& a, const fs::path& b) {
  const std::string x = a.generic_string(), y = b.generic_string();
  const std::string lower_x = Lower(x), lower_y = Lower(y);
  return lower_x == lower_y ? x < y : lower_x < lower_y;
}

bool ExcludedFile(const fs::path& path) {
  const std::string name = Lower(path.filename().string());
  if (name == "readme.txt" || name == "readme.md" || name == "aes_key.bin" ||
      name == "manifest.json" || name == "install_manifest.json" || name == "gta4_manifest.toml")
    return true;
  const std::string ext = Lower(path.extension().string());
  return ext == ".asi" || ext == ".dll" || ext == ".so" || ext == ".sprx" || ext == ".prx" ||
         ext == ".elf" || ext == ".exe" || ext == ".xex" || ext == ".xexp";
}

fs::file_type EntryType(const fs::directory_entry& entry, std::error_code& ec) {
  // Iterator predicates retain dirent types; symlink_status would call the PS5's refused lstat.
  if (entry.is_symlink(ec)) return fs::file_type::symlink;
  if (ec) return fs::file_type::none;
  if (entry.is_directory(ec)) return fs::file_type::directory;
  if (ec) return fs::file_type::none;
  if (entry.is_regular_file(ec)) return fs::file_type::regular;
  return fs::file_type::none;
}

void ScanDirectory(const fs::path& root, const fs::path& relative, ModFiles& result) {
  std::error_code ec;
  fs::directory_iterator it(root / relative, ec), end;
  if (ec) {
    result.errors.push_back((root / relative).string() + ": " + ec.message());
    return;
  }
  std::vector<fs::directory_entry> children;
  while (it != end) {
    children.push_back(*it);
    it.increment(ec);
    if (ec) {
      result.errors.push_back((root / relative).string() + ": " + ec.message());
      break;
    }
  }
  std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) {
    return PathLess(a.path(), b.path());
  });
  for (size_t i = 0; i < children.size();) {
    size_t next = i + 1;
    const fs::path child = relative / children[i].path().filename();
    const std::string key = Lower(child.generic_string());
    while (next < children.size() &&
           Lower((relative / children[next].path().filename()).generic_string()) == key)
      ++next;
    // HostPathDevice matches names lazily, so even an ignored symlink can shadow a directory.
    if (next != i + 1) {
      result.errors.push_back(key + ": duplicate case-insensitive mod path");
      i = next;
      continue;
    }
    const std::string name = child.filename().string();
    if (!name.empty() && name.front() != '.') {
      if (name.find_first_of(":\\") != std::string::npos) {
        result.errors.push_back(child.generic_string() + ": unsafe guest path component");
      } else {
        const auto type = EntryType(children[i], ec);
        if (ec) {
          result.errors.push_back(child.generic_string() + ": " + ec.message());
        } else if (type == fs::file_type::directory) {
          ScanDirectory(root, child, result);
        } else if (type == fs::file_type::regular && !ExcludedFile(child)) {
          result.files.push_back(child);
        }
      }
    }
    i = next;
  }
}

}  // namespace

ModFiles ScanModFiles(const fs::path& root) {
  ModFiles result;
  if (root.empty()) return result;
  std::error_code ec;
  fs::path directory = (root.is_absolute() ? root : fs::absolute(root, ec)).lexically_normal();
  while (directory.filename().empty() && directory != directory.root_path()) directory = directory.parent_path();
  if (ec) {
    result.errors.push_back(root.string() + ": " + ec.message());
    return result;
  }
  if (directory != directory.root_path()) {
    fs::directory_iterator it(directory.parent_path(), ec), end;
    while (!ec && it != end && it->path().filename() != directory.filename()) it.increment(ec);
    if (ec) {
      if (ec != std::errc::no_such_file_or_directory && ec != std::errc::not_a_directory)
        result.errors.push_back(root.string() + ": " + ec.message());
      return result;
    }
    if (it == end) return result;
    const auto type = EntryType(*it, ec);
    if (ec) {
      result.errors.push_back(root.string() + ": " + ec.message());
      return result;
    }
    if (type == fs::file_type::symlink) return result;
    if (type != fs::file_type::directory) {
      result.errors.push_back(root.string() + ": not a directory");
      return result;
    }
  }
  ScanDirectory(directory, {}, result);
  std::sort(result.files.begin(), result.files.end(), PathLess);
  std::sort(result.errors.begin(), result.errors.end());
  return result;
}

}  // namespace gta4ps5
