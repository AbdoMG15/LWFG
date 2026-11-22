#include "lfg/compute/lsfg_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace lfg::compute {

namespace {

#pragma pack(push, 1)
struct DosHeader {
    uint16_t e_magic;
    uint16_t e_cblp[29];
    uint32_t e_lfanew;
};

struct CoffFileHeader {
    uint16_t machine;
    uint16_t numberOfSections;
    uint32_t timeDateStamp;
    uint32_t pointerToSymbolTable;
    uint32_t numberOfSymbols;
    uint16_t sizeOfOptionalHeader;
    uint16_t characteristics;
};

struct DataDirectoryEntry {
    uint32_t virtualAddress;
    uint32_t size;
};

struct SectionHeader {
    char name[8];
    uint32_t virtualSize;
    uint32_t virtualAddress;
    uint32_t sizeOfRawData;
    uint32_t pointerToRawData;
    uint32_t pointerToRelocations;
    uint32_t pointerToLinenumbers;
    uint16_t numberOfRelocations;
    uint16_t numberOfLinenumbers;
    uint32_t characteristics;
};

struct ResourceDirTable {
    uint32_t characteristics;
    uint32_t timeDateStamp;
    uint16_t majorVersion;
    uint16_t minorVersion;
    uint16_t numberOfNamedEntries;
    uint16_t numberOfIdEntries;
};

struct ResourceDirEntry {
    uint32_t nameOffsetOrId;
    uint32_t dataEntryOrSubdirOffset;
};

struct ResourceDataEntry {
    uint32_t dataRva;
    uint32_t size;
    uint32_t codepage;
    uint32_t reserved;
};
#pragma pack(pop)

struct ConstantBufferData {
    uint32_t inputOffset[2]{0, 0};
    uint32_t firstIter = 0;
    uint32_t firstIterS = 0;
    uint32_t advancedColorKind = 0;
    uint32_t hdrSupport = 0;
    float resolutionInvScale = 1.0f;
    float timestamp = 0.0f;
    float uiThreshold = 0.5f;
    uint32_t pad[3]{0, 0, 0};
};
static_assert(sizeof(ConstantBufferData) == 48, "LSFG ConstantBufferData must be 48 bytes");

VkExtent2D shiftExtent(VkExtent2D ext, uint32_t shift) {
    return VkExtent2D{
        std::max(1u, ext.width >> shift),
        std::max(1u, ext.height >> shift)
    };
}

VkExtent2D addShiftExtent(VkExtent2D ext, uint32_t add, uint32_t shift) {
    return VkExtent2D{
        std::max(1u, (ext.width + add) >> shift),
        std::max(1u, (ext.height + add) >> shift)
    };
}

std::string trimString(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

bool isValidLosslessDll(const std::filesystem::path& p) {
    std::error_code ec;
    if (p.empty() || !std::filesystem::is_regular_file(p, ec)) return false;
    auto sz = std::filesystem::file_size(p, ec);
    if (ec || sz < 65536) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return false;
    uint16_t mz = 0;
    f.read(reinterpret_cast<char*>(&mz), sizeof(mz));
    return f.good() && mz == 0x5A4Du;
}

void parseVdfLibraryPaths(const std::filesystem::path& vdfPath,
                          std::vector<std::filesystem::path>& outLibs) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(vdfPath, ec)) return;
    std::ifstream in(vdfPath);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        size_t pos = line.find("\"path\"");
        if (pos == std::string::npos) continue;
        size_t q1 = line.find('"', pos + 6);
        if (q1 == std::string::npos) continue;
        size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        std::string raw = line.substr(q1 + 1, q2 - q1 - 1);
        std::string unescaped;
        unescaped.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') {
                unescaped.push_back('/');
                ++i;
            } else {
                unescaped.push_back(raw[i]);
            }
        }
        if (!unescaped.empty()) {
            outLibs.emplace_back(unescaped);
        }
    }
}

std::filesystem::path parseLsfgVkConfDll(const std::filesystem::path& confPath) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(confPath, ec)) return {};
    std::ifstream in(confPath);
    if (!in.is_open()) return {};
    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trimString(line.substr(0, eq));
        if (key != "dll") continue;
        std::string val = trimString(line.substr(eq + 1));
        if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                                (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }
        if (isValidLosslessDll(val)) return val;
    }
    return {};
}

std::unordered_map<uint32_t, std::vector<uint8_t>> extractResourcesFromDLL(
    const std::filesystem::path& dllPath) {
    std::ifstream file(dllPath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Unable to open Lossless.dll: " + dllPath.string());
    }

    DosHeader dos{};
    file.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!file.good() || dos.e_magic != 0x5A4Du) {
        throw std::runtime_error("Invalid DOS header in Lossless.dll");
    }

    file.seekg(dos.e_lfanew, std::ios::beg);
    uint32_t peSig = 0;
    file.read(reinterpret_cast<char*>(&peSig), sizeof(peSig));
    if (!file.good() || peSig != 0x00004550u) {
        throw std::runtime_error("Invalid PE signature in Lossless.dll");
    }

    CoffFileHeader coff{};
    file.read(reinterpret_cast<char*>(&coff), sizeof(coff));
    if (!file.good() || coff.numberOfSections == 0 || coff.sizeOfOptionalHeader < 96) {
        throw std::runtime_error("Invalid COFF header in Lossless.dll");
    }

    const std::streamoff optStart = static_cast<std::streamoff>(dos.e_lfanew) + 4 + sizeof(CoffFileHeader);
    uint16_t optMagic = 0;
    file.read(reinterpret_cast<char*>(&optMagic), sizeof(optMagic));
    if (!file.good() || (optMagic != 0x020Bu && optMagic != 0x010Bu)) {
        throw std::runtime_error("Unsupported PE optional header magic in Lossless.dll");
    }

    // Resource Table is DataDirectory[2], located at offset 112 (PE32) or 128 (PE32+) in Optional Header
    const std::streamoff rsrcDirEntryOffset = optStart + (optMagic == 0x020Bu ? 128 : 112);
    file.seekg(rsrcDirEntryOffset, std::ios::beg);
    DataDirectoryEntry rsrcDir{};
    file.read(reinterpret_cast<char*>(&rsrcDir), sizeof(rsrcDir));
    if (!file.good() || rsrcDir.virtualAddress == 0 || rsrcDir.size == 0) {
        throw std::runtime_error("No PE resource directory found in Lossless.dll");
    }

    const std::streamoff secStart = optStart + coff.sizeOfOptionalHeader;
    file.seekg(secStart, std::ios::beg);
    std::vector<SectionHeader> sections(coff.numberOfSections);
    file.read(reinterpret_cast<char*>(sections.data()),
              static_cast<std::streamsize>(sections.size() * sizeof(SectionHeader)));
    if (!file.good()) {
        throw std::runtime_error("Failed to read PE section headers from Lossless.dll");
    }

    auto rvaToFileOffset = [&](uint32_t rva) -> std::streamoff {
        for (const auto& s : sections) {
            uint32_t span = std::max(s.virtualSize, s.sizeOfRawData);
            if (rva >= s.virtualAddress && rva < s.virtualAddress + span) {
                return static_cast<std::streamoff>(s.pointerToRawData) +
                       static_cast<std::streamoff>(rva - s.virtualAddress);
            }
        }
        return -1;
    };

    const std::streamoff rsrcBase = rvaToFileOffset(rsrcDir.virtualAddress);
    if (rsrcBase < 0) {
        throw std::runtime_error("Unable to resolve .rsrc section offset in Lossless.dll");
    }

    auto readDirEntries = [&](std::streamoff dirOffset) -> std::vector<ResourceDirEntry> {
        file.seekg(dirOffset, std::ios::beg);
        ResourceDirTable tbl{};
        file.read(reinterpret_cast<char*>(&tbl), sizeof(tbl));
        if (!file.good()) return {};
        size_t total = static_cast<size_t>(tbl.numberOfNamedEntries) +
                       static_cast<size_t>(tbl.numberOfIdEntries);
        if (total > 4096) return {};
        std::vector<ResourceDirEntry> entries(total);
        if (total > 0) {
            file.read(reinterpret_cast<char*>(entries.data()),
                      static_cast<std::streamsize>(total * sizeof(ResourceDirEntry)));
        }
        return entries;
    };

    std::unordered_map<uint32_t, std::vector<uint8_t>> resources;
    const auto typeEntries = readDirEntries(rsrcBase);
    for (const auto& typeEntry : typeEntries) {
        const bool isNamed = (typeEntry.nameOffsetOrId & 0x80000000u) != 0;
        const uint32_t typeId = typeEntry.nameOffsetOrId & 0x7FFFFFFFu;
        const bool isSubdir = (typeEntry.dataEntryOrSubdirOffset & 0x80000000u) != 0;
        if (isNamed || typeId != 10u || !isSubdir) continue;  // RT_RCDATA == 10

        const std::streamoff nameDirOffset =
            rsrcBase + static_cast<std::streamoff>(typeEntry.dataEntryOrSubdirOffset & 0x7FFFFFFFu);
        const auto nameEntries = readDirEntries(nameDirOffset);
        for (const auto& nameEntry : nameEntries) {
            const bool nameIsString = (nameEntry.nameOffsetOrId & 0x80000000u) != 0;
            const uint32_t resId = nameEntry.nameOffsetOrId & 0x7FFFFFFFu;
            const bool nameIsSubdir = (nameEntry.dataEntryOrSubdirOffset & 0x80000000u) != 0;
            if (nameIsString || !nameIsSubdir) continue;

            const std::streamoff langDirOffset =
                rsrcBase + static_cast<std::streamoff>(nameEntry.dataEntryOrSubdirOffset & 0x7FFFFFFFu);
            const auto langEntries = readDirEntries(langDirOffset);
            if (langEntries.empty()) continue;

            const auto& langEntry = langEntries.front();
            if ((langEntry.dataEntryOrSubdirOffset & 0x80000000u) != 0) continue;

            const std::streamoff dataEntryOffset =
                rsrcBase + static_cast<std::streamoff>(langEntry.dataEntryOrSubdirOffset & 0x7FFFFFFFu);
            file.seekg(dataEntryOffset, std::ios::beg);
            ResourceDataEntry dataEntry{};
            file.read(reinterpret_cast<char*>(&dataEntry), sizeof(dataEntry));
            if (!file.good() || dataEntry.size < 20 || (dataEntry.size % 4) != 0) continue;

            const std::streamoff payloadOffset = rvaToFileOffset(dataEntry.dataRva);
            if (payloadOffset < 0) continue;

            std::vector<uint8_t> bytes(dataEntry.size);
            file.seekg(payloadOffset, std::ios::beg);
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!file.good()) continue;

            uint32_t magic = 0;
            std::memcpy(&magic, bytes.data(), sizeof(magic));
            if (magic == 0x07230203u) {
                resources.emplace(resId, std::move(bytes));
            }
        }
    }

    return resources;
}

void patchGenerateShader(std::vector<uint8_t>& data) {
    if (data.size() < 24 || (data.size() % 4) != 0) return;
    auto* words = reinterpret_cast<uint32_t*>(data.data());
    const size_t wordCount = data.size() / sizeof(uint32_t);

    constexpr uint16_t kSpvOpCapability = 17;
    constexpr uint16_t kSpvOpTypeImage = 25;
    constexpr uint32_t kSpvCapabilityStorageImageWriteWithoutFormat = 56;
    constexpr uint32_t kSpvCapabilityShader = 1;
    constexpr uint32_t kSpvImageFormatRgba8 = 4;

    for (size_t i = 5; i < wordCount;) {
        const uint32_t word = words[i];
        const uint16_t wc = static_cast<uint16_t>(word >> 16);
        const uint16_t op = static_cast<uint16_t>(word & 0xFFFFu);
        if (wc == 0 || i + wc > wordCount) break;

        if (op == kSpvOpCapability && wc >= 2) {
            if (words[i + 1] == kSpvCapabilityStorageImageWriteWithoutFormat) {
                words[i + 1] = kSpvCapabilityShader;
            }
        } else if (op == kSpvOpTypeImage && wc >= 9) {
            if (words[i + 7] == 2u) {
                words[i + 8] = kSpvImageFormatRgba8;
            }
        }
        i += wc;
    }
}

std::filesystem::path pipelineCacheFilePath() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "lfg-linux" / "lsfg_pipeline_cache.bin";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".cache" / "lfg-linux" / "lsfg_pipeline_cache.bin";
    }
    return "/tmp/lfg-linux-lsfg-pipeline-cache.bin";
}

}  // namespace

std::filesystem::path LsfgPipeline::resolveExplicitDllPath(const std::string& explicitPath) {
    std::string s = trimString(explicitPath);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        s = trimString(s.substr(1, s.size() - 2));
    }
    if (s.rfind("file://", 0) == 0) {
        s = s.substr(7);
        std::string decoded;
        decoded.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size() &&
                std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
                char hex[3] = {s[i + 1], s[i + 2], '\0'};
                decoded.push_back(static_cast<char>(std::strtol(hex, nullptr, 16)));
                i += 2;
            } else {
                decoded.push_back(s[i]);
            }
        }
        s = trimString(decoded);
    }
    if (s.empty()) return {};
    if (s[0] == '~') {
        if (const char* home = std::getenv("HOME"); home && *home) {
            if (s.size() == 1) s = home;
            else if (s[1] == '/') s = std::string(home) + s.substr(1);
        }
    }

    const std::filesystem::path p(s);
    const std::filesystem::path kRel =
        std::filesystem::path("steamapps") / "common" / "Lossless Scaling" / "Lossless.dll";
    const std::filesystem::path candidates[] = {
        p,
        p / "Lossless.dll",
        p / "lossless.dll",
        p / "Lossless Scaling" / "Lossless.dll",
        p / "common" / "Lossless Scaling" / "Lossless.dll",
        p / kRel,
        p / "SteamApps" / "common" / "Lossless Scaling" / "Lossless.dll",
        p / "SteamLibrary" / kRel,
        p / "Steam" / kRel,
    };
    for (const auto& c : candidates) {
        if (isValidLosslessDll(c)) return c;
    }

    // If p is a directory, search up to 3 levels deep for Lossless.dll (case-insensitive)
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) {
        std::filesystem::recursive_directory_iterator it(
            p, std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it.depth() > 3) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            std::string fname = it->path().filename().string();
            for (char& ch : fname) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (fname == "lossless.dll" && isValidLosslessDll(it->path())) {
                return it->path();
            }
        }
    }
    return {};
}

std::filesystem::path LsfgPipeline::findLosslessDll(const std::string& explicitPath) {
    // 1. User-specified location (from Settings tab or --dll)
    if (!explicitPath.empty()) {
        auto resolved = resolveExplicitDllPath(explicitPath);
        if (!resolved.empty()) return resolved;
    }

    const std::filesystem::path kRelativeDll =
        std::filesystem::path("steamapps") / "common" / "Lossless Scaling" / "Lossless.dll";

    // 2. Steam installation roots and all Steam libraries declared in libraryfolders.vdf
    std::vector<std::filesystem::path> steamRoots;
    if (const char* xdgData = std::getenv("XDG_DATA_HOME"); xdgData && *xdgData) {
        steamRoots.push_back(std::filesystem::path(xdgData) / "Steam");
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        const std::filesystem::path h(home);
        steamRoots.push_back(h / ".local" / "share" / "Steam");
        steamRoots.push_back(h / ".steam" / "steam");
        steamRoots.push_back(h / ".steam" / "root");
        steamRoots.push_back(h / ".steam" / "debian-installation");
        steamRoots.push_back(h / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam");
        steamRoots.push_back(h / ".var" / "app" / "com.valvesoftware.Steam" / ".steam" / "steam");
        steamRoots.push_back(h / "snap" / "steam" / "common" / ".local" / "share" / "Steam");
        steamRoots.push_back(h / "snap" / "steam" / "common" / ".steam" / "steam");
    }

    std::vector<std::filesystem::path> allLibraries = steamRoots;
    for (const auto& root : steamRoots) {
        parseVdfLibraryPaths(root / "steamapps" / "libraryfolders.vdf", allLibraries);
        parseVdfLibraryPaths(root / "config" / "libraryfolders.vdf", allLibraries);
    }

    for (const auto& lib : allLibraries) {
        auto candidate = lib / kRelativeDll;
        if (isValidLosslessDll(candidate)) return candidate;
        auto alt = lib / "SteamApps" / "common" / "Lossless Scaling" / "Lossless.dll";
        if (isValidLosslessDll(alt)) return alt;
    }

    // 3. Scan mounted external/secondary drives (/run/media, /media, /mnt) for SteamLibrary folders
    for (const char* mountRoot : {"/run/media", "/media", "/mnt"}) {
        std::error_code ec;
        if (!std::filesystem::is_directory(mountRoot, ec)) continue;
        std::filesystem::recursive_directory_iterator it(
            mountRoot, std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it.depth() > 3) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_directory(ec)) continue;
            const auto& dirPath = it->path();
            for (const char* sub : {"", "SteamLibrary", "Steam", "steam"}) {
                std::filesystem::path base = (*sub) ? (dirPath / sub) : dirPath;
                if (isValidLosslessDll(base / kRelativeDll)) return base / kRelativeDll;
            }
        }
    }

    // 4. Environment variable LSFG_DLL_PATH or lsfg-vk config
    if (const char* envDll = std::getenv("LSFG_DLL_PATH"); envDll && *envDll) {
        auto resolved = resolveExplicitDllPath(envDll);
        if (!resolved.empty()) return resolved;
    }
    if (const char* xdgCfg = std::getenv("XDG_CONFIG_HOME"); xdgCfg && *xdgCfg) {
        auto p = parseLsfgVkConfDll(std::filesystem::path(xdgCfg) / "lsfg-vk" / "conf.toml");
        if (!p.empty()) return p;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        auto p = parseLsfgVkConfDll(std::filesystem::path(home) / ".config" / "lsfg-vk" / "conf.toml");
        if (!p.empty()) return p;
    }

    return {};
}

LsfgPipeline::LsfgPipeline(const VulkanContext& vk, const std::string& explicitDllPath)
    : vk_(vk) {
    const auto foundPath = findLosslessDll(explicitDllPath);
    if (foundPath.empty()) {
        std::fprintf(stderr,
                     "[lsfg] ERROR: Lossless Scaling (Steam AppID 993090) was not found in your Steam library!\n"
                     "[lsfg] Please purchase and install 'Lossless Scaling' on Steam first to use LFG Frame Generation.\n");
        return;
    }

    if (loadAndCompileShaders(foundPath)) {
        dllPath_ = foundPath.string();
        available_ = true;
        std::fprintf(stderr,
                     "[lsfg] Loaded Lossless Scaling LSFG 3.1 from Steam: %s (%s precision)\n",
                     dllPath_.c_str(), useFp16_ ? "FP16" : "FP32");
    }
}

LsfgPipeline::~LsfgPipeline() {
    destroyResolutionResources();
    destroyShaders();
}

bool LsfgPipeline::loadAndCompileShaders(const std::filesystem::path& dllFile) {
    std::unordered_map<uint32_t, std::vector<uint8_t>> resources;
    try {
        resources = extractResourcesFromDLL(dllFile);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[lsfg] Failed to extract shaders from %s: %s\n",
                     dllFile.string().c_str(), e.what());
        return false;
    }

    useFp16_ = vk_.fp16Supported();

    // Load pipeline cache from disk if available
    std::vector<uint8_t> cacheBytes;
    {
        std::ifstream cf(pipelineCacheFilePath(), std::ios::binary | std::ios::ate);
        if (cf.is_open()) {
            auto sz = cf.tellg();
            if (sz > 0 && sz < 64 * 1024 * 1024) {
                cacheBytes.resize(static_cast<size_t>(sz));
                cf.seekg(0, std::ios::beg);
                cf.read(reinterpret_cast<char*>(cacheBytes.data()), sz);
            }
        }
    }

    VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    if (!cacheBytes.empty()) {
        cacheInfo.initialDataSize = cacheBytes.size();
        cacheInfo.pInitialData = cacheBytes.data();
    }
    if (vkCreatePipelineCache(vk_.device(), &cacheInfo, nullptr, &pipelineCache_) != VK_SUCCESS) {
        pipelineCache_ = VK_NULL_HANDLE;
    }

    struct Spec {
        uint32_t id;
        bool perf;
        uint32_t sampled;
        uint32_t storage;
        uint32_t buffers;
        uint32_t samplers;
    };

    const Spec specs[] = {
        // Common
        {255, false, 1, 7, 1, 1},
        {256, false, 5, 1, 1, 2},
        // Quality (perf = false)
        {267, false, 1, 2, 0, 1},
        {268, false, 2, 2, 0, 1},
        {269, false, 2, 4, 0, 1},
        {270, false, 4, 4, 0, 1},
        {275, false, 12, 2, 0, 1},
        {276, false, 2, 2, 0, 1},
        {277, false, 2, 2, 0, 1},
        {278, false, 2, 2, 0, 1},
        {279, false, 2, 6, 1, 1},
        {257, false, 9, 3, 1, 2},
        {259, false, 3, 4, 0, 1},
        {260, false, 4, 4, 0, 1},
        {261, false, 4, 4, 0, 1},
        {262, false, 6, 1, 1, 2},
        {263, false, 3, 4, 0, 1},
        {264, false, 4, 4, 0, 1},
        {265, false, 4, 4, 0, 1},
        {266, false, 6, 1, 1, 2},
        {258, false, 10, 2, 1, 2},
        {271, false, 2, 2, 0, 1},
        {272, false, 2, 2, 0, 1},
        {273, false, 2, 2, 0, 1},
        {274, false, 3, 1, 1, 2},
        // Performance (perf = true)
        {267, true, 1, 1, 0, 1},
        {268, true, 1, 1, 0, 1},
        {269, true, 1, 2, 0, 1},
        {270, true, 2, 2, 0, 1},
        {275, true, 6, 2, 0, 1},
        {276, true, 2, 2, 0, 1},
        {277, true, 2, 2, 0, 1},
        {278, true, 2, 2, 0, 1},
        {279, true, 2, 6, 1, 1},
        {257, true, 5, 3, 1, 2},
        {259, true, 3, 2, 0, 1},
        {260, true, 2, 2, 0, 1},
        {261, true, 2, 2, 0, 1},
        {262, true, 4, 1, 1, 2},
        {263, true, 3, 2, 0, 1},
        {264, true, 2, 2, 0, 1},
        {265, true, 2, 2, 0, 1},
        {266, true, 4, 1, 1, 2},
        {258, true, 6, 1, 1, 2},
        {271, true, 1, 1, 0, 1},
        {272, true, 1, 1, 0, 1},
        {273, true, 1, 1, 0, 1},
        {274, true, 2, 1, 1, 2},
    };

    for (const auto& s : specs) {
        const uint32_t resId = 49u + s.id + (s.perf ? 23u : 0u) + (useFp16_ ? 0u : 49u);
        auto it = resources.find(resId);
        if (it == resources.end()) {
            std::fprintf(stderr, "[lsfg] Missing SPIR-V resource %u (shader %u) in Lossless.dll\n",
                         resId, s.id);
            destroyShaders();
            return false;
        }

        std::vector<uint8_t> code = it->second;
        if (s.id == 256) {
            patchGenerateShader(code);
        }

        CompiledShader cs{};
        cs.sampledCount = s.sampled;
        cs.storageCount = s.storage;
        cs.bufferCount = s.buffers;
        cs.samplerCount = s.samplers;

        VkShaderModuleCreateInfo modInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        modInfo.codeSize = code.size();
        modInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        if (vkCreateShaderModule(vk_.device(), &modInfo, nullptr, &cs.module) != VK_SUCCESS) {
            destroyShaders();
            return false;
        }

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.reserve(s.buffers + s.samplers + s.sampled + s.storage);
        for (uint32_t i = 0; i < s.buffers; ++i) {
            bindings.push_back(VkDescriptorSetLayoutBinding{
                i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
        for (uint32_t i = 0; i < s.samplers; ++i) {
            bindings.push_back(VkDescriptorSetLayoutBinding{
                16u + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
        for (uint32_t i = 0; i < s.sampled; ++i) {
            bindings.push_back(VkDescriptorSetLayoutBinding{
                32u + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
        for (uint32_t i = 0; i < s.storage; ++i) {
            bindings.push_back(VkDescriptorSetLayoutBinding{
                48u + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }

        VkDescriptorSetLayoutCreateInfo dslInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dslInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        dslInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(vk_.device(), &dslInfo, nullptr, &cs.setLayout) != VK_SUCCESS) {
            vkDestroyShaderModule(vk_.device(), cs.module, nullptr);
            destroyShaders();
            return false;
        }

        VkPipelineLayoutCreateInfo plInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plInfo.setLayoutCount = 1;
        plInfo.pSetLayouts = &cs.setLayout;
        if (vkCreatePipelineLayout(vk_.device(), &plInfo, nullptr, &cs.pipelineLayout) != VK_SUCCESS) {
            vkDestroyDescriptorSetLayout(vk_.device(), cs.setLayout, nullptr);
            vkDestroyShaderModule(vk_.device(), cs.module, nullptr);
            destroyShaders();
            return false;
        }

        VkComputePipelineCreateInfo cpInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpInfo.stage.module = cs.module;
        cpInfo.stage.pName = "main";
        cpInfo.layout = cs.pipelineLayout;
        if (vkCreateComputePipelines(vk_.device(), pipelineCache_, 1, &cpInfo, nullptr, &cs.pipeline) !=
            VK_SUCCESS) {
            vkDestroyPipelineLayout(vk_.device(), cs.pipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(vk_.device(), cs.setLayout, nullptr);
            vkDestroyShaderModule(vk_.device(), cs.module, nullptr);
            destroyShaders();
            return false;
        }

        compiledShaders_.emplace(ShaderKey{s.id, s.perf}, cs);
    }

    // Create the 3 LSFG samplers (bnb, bnw, eab)
    auto makeSampler = [&](VkSamplerAddressMode mode, VkCompareOp compare, bool white) -> VkSampler {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = mode;
        si.addressModeV = mode;
        si.addressModeW = mode;
        si.compareEnable = VK_FALSE;
        si.compareOp = compare;
        si.maxLod = VK_LOD_CLAMP_NONE;
        si.borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                               : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        VkSampler s = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSampler(vk_.device(), &si, nullptr, &s));
        return s;
    };

    bnbSampler_ = makeSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, false);
    bnwSampler_ = makeSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, true);
    eabSampler_ = makeSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_COMPARE_OP_ALWAYS, false);

    // Create defaultUbo_ (host-visible) and passUbo_ (updated inline via vkCmdUpdateBuffer)
    {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = sizeof(ConstantBufferData);
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(vk_.device(), &bi, nullptr, &defaultUbo_));

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(vk_.device(), defaultUbo_, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = vk_.findMemoryType(
            req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(vk_.device(), &ai, nullptr, &defaultUboMem_));
        VK_CHECK(vkBindBufferMemory(vk_.device(), defaultUbo_, defaultUboMem_, 0));
        VK_CHECK(vkMapMemory(vk_.device(), defaultUboMem_, 0, sizeof(ConstantBufferData), 0, &defaultUboMapped_));
    }
    {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = sizeof(ConstantBufferData);
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(vk_.device(), &bi, nullptr, &passUbo_));

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(vk_.device(), passUbo_, &req);
        passUboMem_ = vk_.allocateMemory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkBindBufferMemory(vk_.device(), passUbo_, passUboMem_, 0));
    }

    savePipelineCache();
    return true;
}

void LsfgPipeline::savePipelineCache() const {
    if (!pipelineCache_) return;
    size_t sz = 0;
    if (vkGetPipelineCacheData(vk_.device(), pipelineCache_, &sz, nullptr) != VK_SUCCESS || sz == 0) return;
    std::vector<uint8_t> data(sz);
    if (vkGetPipelineCacheData(vk_.device(), pipelineCache_, &sz, data.data()) != VK_SUCCESS) return;
    auto path = pipelineCacheFilePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out.is_open()) {
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
}

void LsfgPipeline::destroyShaders() {
    VkDevice dev = vk_.device();
    if (defaultUboMapped_) {
        vkUnmapMemory(dev, defaultUboMem_);
        defaultUboMapped_ = nullptr;
    }
    if (defaultUbo_) { vkDestroyBuffer(dev, defaultUbo_, nullptr); defaultUbo_ = VK_NULL_HANDLE; }
    if (defaultUboMem_) { vkFreeMemory(dev, defaultUboMem_, nullptr); defaultUboMem_ = VK_NULL_HANDLE; }
    if (passUbo_) { vkDestroyBuffer(dev, passUbo_, nullptr); passUbo_ = VK_NULL_HANDLE; }
    if (passUboMem_) { vkFreeMemory(dev, passUboMem_, nullptr); passUboMem_ = VK_NULL_HANDLE; }

    if (bnbSampler_) { vkDestroySampler(dev, bnbSampler_, nullptr); bnbSampler_ = VK_NULL_HANDLE; }
    if (bnwSampler_) { vkDestroySampler(dev, bnwSampler_, nullptr); bnwSampler_ = VK_NULL_HANDLE; }
    if (eabSampler_) { vkDestroySampler(dev, eabSampler_, nullptr); eabSampler_ = VK_NULL_HANDLE; }

    for (auto& [k, cs] : compiledShaders_) {
        if (cs.pipeline) vkDestroyPipeline(dev, cs.pipeline, nullptr);
        if (cs.pipelineLayout) vkDestroyPipelineLayout(dev, cs.pipelineLayout, nullptr);
        if (cs.setLayout) vkDestroyDescriptorSetLayout(dev, cs.setLayout, nullptr);
        if (cs.module) vkDestroyShaderModule(dev, cs.module, nullptr);
    }
    compiledShaders_.clear();

    if (pipelineCache_) {
        vkDestroyPipelineCache(dev, pipelineCache_, nullptr);
        pipelineCache_ = VK_NULL_HANDLE;
    }
}

LsfgPipeline::InternalImage LsfgPipeline::createInternalImage(
    VkExtent2D extent, VkFormat format, std::vector<VkImage>& outToInit) {
    InternalImage img{};
    img.extent = {std::max(1u, extent.width), std::max(1u, extent.height)};
    img.format = format;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {img.extent.width, img.extent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(vk_.device(), &ii, nullptr, &img.image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(vk_.device(), img.image, &req);
    img.memory = vk_.allocateMemory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkBindImageMemory(vk_.device(), img.image, img.memory, 0));

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(vk_.device(), &vi, nullptr, &img.view));

    outToInit.push_back(img.image);
    return img;
}

void LsfgPipeline::destroyInternalImage(InternalImage& img) {
    VkDevice dev = vk_.device();
    if (img.view) vkDestroyImageView(dev, img.view, nullptr);
    if (img.image) vkDestroyImage(dev, img.image, nullptr);
    if (img.memory) vkFreeMemory(dev, img.memory, nullptr);
    img = {};
}

void LsfgPipeline::destroyResolutionResources() {
    VkDevice dev = vk_.device();
    if (descriptorPool_) {
        vkDestroyDescriptorPool(dev, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }

    destroyInternalImage(blackImage_);
    for (auto& img : mipmapImages_) destroyInternalImage(img);

    for (auto& a0 : alpha0_) {
        for (auto& img : a0.temp0) destroyInternalImage(img);
        for (auto& img : a0.temp1) destroyInternalImage(img);
        for (auto& img : a0.images) destroyInternalImage(img);
        a0.temp0.clear();
        a0.temp1.clear();
        a0.images.clear();
    }

    for (auto& a1 : alpha1_) {
        for (auto& vec : a1.images) {
            for (auto& img : vec) destroyInternalImage(img);
        }
        a1.images.clear();
        a1.sets.clear();
    }

    for (auto& img : beta0Images_) destroyInternalImage(img);
    for (auto& img : beta1Temp0_) destroyInternalImage(img);
    for (auto& img : beta1Temp1_) destroyInternalImage(img);
    for (auto& img : beta1Images_) destroyInternalImage(img);

    for (auto& g0 : gamma0_) {
        for (auto& img : g0.images) destroyInternalImage(img);
        g0.sets.clear();
    }
    for (auto& g1 : gamma1_) {
        for (auto& img : g1.temp0) destroyInternalImage(img);
        for (auto& img : g1.temp1) destroyInternalImage(img);
        destroyInternalImage(g1.image);
        g1.temp0.clear();
        g1.temp1.clear();
    }

    for (auto& d0 : delta0_) {
        for (auto& img : d0.images0) destroyInternalImage(img);
        for (auto& img : d0.images1) destroyInternalImage(img);
        d0.images1.clear();
        d0.sets0.clear();
        d0.sets1.clear();
    }
    for (auto& d1 : delta1_) {
        for (auto& img : d1.temp0) destroyInternalImage(img);
        for (auto& img : d1.temp1) destroyInternalImage(img);
        destroyInternalImage(d1.image0);
        destroyInternalImage(d1.image1);
        d1.temp0.clear();
        d1.temp1.clear();
    }

    for (auto& out : outputs_) {
        if (out.view) vkDestroyImageView(dev, out.view, nullptr);
        if (out.image) vkDestroyImage(dev, out.image, nullptr);
        if (out.memory) vkFreeMemory(dev, out.memory, nullptr);
        out = {};
    }
}

VkDescriptorSet LsfgPipeline::allocateAndWriteSet(
    const CompiledShader& shader,
    const std::vector<VkImageView>& sampledViews,
    const std::vector<VkImageView>& storageViews,
    const std::vector<VkSampler>& samplers,
    const std::vector<VkBuffer>& uniformBuffers) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &shader.setLayout;
    VK_CHECK(vkAllocateDescriptorSets(vk_.device(), &ai, &set));

    const size_t totalWrites =
        uniformBuffers.size() + samplers.size() + sampledViews.size() + storageViews.size();
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(totalWrites);
    std::vector<VkDescriptorBufferInfo> bufInfos;
    bufInfos.reserve(uniformBuffers.size());
    std::vector<VkDescriptorImageInfo> imgInfos;
    imgInfos.reserve(samplers.size() + sampledViews.size() + storageViews.size());

    for (size_t i = 0; i < uniformBuffers.size(); ++i) {
        bufInfos.push_back(VkDescriptorBufferInfo{uniformBuffers[i], 0, sizeof(ConstantBufferData)});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = static_cast<uint32_t>(i);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bufInfos.back();
        writes.push_back(w);
    }

    for (size_t i = 0; i < samplers.size(); ++i) {
        imgInfos.push_back(VkDescriptorImageInfo{samplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = static_cast<uint32_t>(16 + i);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = &imgInfos.back();
        writes.push_back(w);
    }

    for (size_t i = 0; i < sampledViews.size(); ++i) {
        imgInfos.push_back(
            VkDescriptorImageInfo{VK_NULL_HANDLE, sampledViews[i], VK_IMAGE_LAYOUT_GENERAL});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = static_cast<uint32_t>(32 + i);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &imgInfos.back();
        writes.push_back(w);
    }

    for (size_t i = 0; i < storageViews.size(); ++i) {
        imgInfos.push_back(
            VkDescriptorImageInfo{VK_NULL_HANDLE, storageViews[i], VK_IMAGE_LAYOUT_GENERAL});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = static_cast<uint32_t>(48 + i);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &imgInfos.back();
        writes.push_back(w);
    }

    if (!writes.empty()) {
        vkUpdateDescriptorSets(vk_.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    return set;
}

void LsfgPipeline::configure(uint32_t width, uint32_t height, int flowScale, bool performanceMode,
                             VkImageView slot0View, VkImageView slot1View) {
    if (!available_) return;
    destroyResolutionResources();

    width_ = std::max(1u, width);
    height_ = std::max(1u, height);
    flowScale_ = std::clamp(flowScale, 25, 100);
    perfMode_ = performanceMode;
    slot0View_ = slot0View;
    slot1View_ = slot1View;

    // Allocate the ping-pong destination images for streaming synthesized intermediate frames
    std::vector<VkImage> imagesToInit;
    imagesToInit.reserve(256);
    try {
    for (uint32_t k = 0; k < kOutputSlots; ++k) {
        auto& out = outputs_[k];
        out.width = width_;
        out.height = height_;

        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_B8G8R8A8_UNORM;
        ii.extent = {width_, height_, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(vk_.device(), &ii, nullptr, &out.image));

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(vk_.device(), out.image, &req);
        out.memory = vk_.allocateMemory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkBindImageMemory(vk_.device(), out.image, out.memory, 0));

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = out.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_B8G8R8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(vk_.device(), &vi, nullptr, &out.view));
        imagesToInit.push_back(out.image);
    }

    const float invFlow = 100.0f / static_cast<float>(flowScale_);
    const VkExtent2D sourceExtent{width_, height_};
    const VkExtent2D flowExtent{
        std::max(1u, static_cast<uint32_t>(static_cast<float>(width_) / invFlow)),
        std::max(1u, static_cast<uint32_t>(static_cast<float>(height_) / invFlow))
    };

    // Update defaultUbo_
    ConstantBufferData defaultCb{};
    defaultCb.resolutionInvScale = invFlow;
    defaultCb.timestamp = 0.5f;
    defaultCb.uiThreshold = 0.5f;
    std::memcpy(defaultUboMapped_, &defaultCb, sizeof(defaultCb));

    // Create descriptor pool
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 512;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(vk_.device(), &pi, nullptr, &descriptorPool_));

    auto getShader = [&](uint32_t id, bool perf) -> const CompiledShader& {
        return compiledShaders_.at(ShaderKey{id, perf});
    };
    auto viewsOf = [](const auto& container) -> std::vector<VkImageView> {
        std::vector<VkImageView> v;
        v.reserve(container.size());
        for (const auto& item : container) v.push_back(item.view);
        return v;
    };

    blackImage_ = createInternalImage({4, 4}, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);

    // 1. Mipmaps
    mipmapShader_ = getShader(255, false);
    for (uint32_t i = 0; i < 7; ++i) {
        mipmapImages_[i] = createInternalImage(shiftExtent(flowExtent, i), VK_FORMAT_R8_UNORM, imagesToInit);
    }
    const auto mipViews = viewsOf(mipmapImages_);
    mipmapSets_[0] = allocateAndWriteSet(mipmapShader_, {slot0View_}, mipViews, {bnbSampler_}, {defaultUbo_});
    mipmapSets_[1] = allocateAndWriteSet(mipmapShader_, {slot1View_}, mipViews, {bnbSampler_}, {defaultUbo_});
    mipmapDispatch_ = addShiftExtent(flowExtent, 63, 6);

    // 2. Alpha0 & Alpha1 (7 levels)
    const size_t m = perfMode_ ? 1 : 2;
    for (size_t i = 0; i < 7; ++i) {
        auto& a0 = alpha0_[i];
        const VkExtent2D halfExt = addShiftExtent(mipmapImages_[i].extent, 1, 1);
        const VkExtent2D quarterExt = addShiftExtent(halfExt, 1, 1);

        a0.temp0.resize(m);
        a0.temp1.resize(m);
        for (size_t k = 0; k < m; ++k) {
            a0.temp0[k] = createInternalImage(halfExt, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
            a0.temp1[k] = createInternalImage(halfExt, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
        }
        a0.images.resize(2 * m);
        for (size_t k = 0; k < 2 * m; ++k) {
            a0.images[k] = createInternalImage(quarterExt, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
        }

        a0.shaders[0] = getShader(267, perfMode_);
        a0.shaders[1] = getShader(268, perfMode_);
        a0.shaders[2] = getShader(269, perfMode_);

        a0.sets[0] = allocateAndWriteSet(a0.shaders[0], {mipmapImages_[i].view}, viewsOf(a0.temp0), {bnbSampler_}, {});
        a0.sets[1] = allocateAndWriteSet(a0.shaders[1], viewsOf(a0.temp0), viewsOf(a0.temp1), {bnbSampler_}, {});
        a0.sets[2] = allocateAndWriteSet(a0.shaders[2], viewsOf(a0.temp1), viewsOf(a0.images), {bnbSampler_}, {});
        a0.dispatch0 = addShiftExtent(halfExt, 7, 3);
        a0.dispatch1 = addShiftExtent(quarterExt, 7, 3);

        auto& a1 = alpha1_[i];
        a1.temporal = (i == 0) ? 3 : 2;
        a1.shader = getShader(270, perfMode_);
        a1.images.resize(a1.temporal);
        a1.sets.resize(a1.temporal);
        for (size_t t = 0; t < a1.temporal; ++t) {
            a1.images[t].resize(2 * m);
            for (size_t k = 0; k < 2 * m; ++k) {
                a1.images[t][k] = createInternalImage(quarterExt, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
            }
            a1.sets[t] = allocateAndWriteSet(
                a1.shader, viewsOf(a0.images), viewsOf(a1.images[t]), {bnbSampler_}, {});
        }
        a1.dispatch = addShiftExtent(quarterExt, 7, 3);
    }

    // 3. Beta0
    {
        const VkExtent2D ext = alpha1_[0].images[0][0].extent;
        for (size_t i = 0; i < 2; ++i) {
            beta0Images_[i] = createInternalImage(ext, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
        }
        beta0Shader_ = getShader(275, perfMode_);
        const size_t tCount = alpha1_[0].images.size();  // 3
        for (size_t i = 0; i < tCount; ++i) {
            std::vector<VkImageView> sampled;
            for (const auto& img : alpha1_[0].images[(i + tCount - 2) % tCount]) sampled.push_back(img.view);
            for (const auto& img : alpha1_[0].images[(i + tCount - 1) % tCount]) sampled.push_back(img.view);
            for (const auto& img : alpha1_[0].images[i % tCount]) sampled.push_back(img.view);
            beta0Sets_[i] = allocateAndWriteSet(
                beta0Shader_, sampled, viewsOf(beta0Images_), {bnwSampler_}, {});
        }
        beta0Dispatch_ = addShiftExtent(ext, 7, 3);
    }

    // 4. Beta1
    {
        const VkExtent2D ext = beta0Images_[0].extent;
        for (size_t i = 0; i < 2; ++i) {
            beta1Temp0_[i] = createInternalImage(ext, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
            beta1Temp1_[i] = createInternalImage(ext, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
        }
        for (uint32_t i = 0; i < 6; ++i) {
            beta1Images_[i] = createInternalImage(shiftExtent(ext, i), VK_FORMAT_R8_UNORM, imagesToInit);
        }
        beta1Shaders_[0] = getShader(276, perfMode_);
        beta1Shaders_[1] = getShader(277, perfMode_);
        beta1Shaders_[2] = getShader(278, perfMode_);
        beta1Shaders_[3] = getShader(279, perfMode_);

        beta1Sets_[0] = allocateAndWriteSet(
            beta1Shaders_[0], viewsOf(beta0Images_), viewsOf(beta1Temp0_), {bnbSampler_}, {});
        beta1Sets_[1] = allocateAndWriteSet(
            beta1Shaders_[1], viewsOf(beta1Temp0_), viewsOf(beta1Temp1_), {bnbSampler_}, {});
        beta1Sets_[2] = allocateAndWriteSet(
            beta1Shaders_[2], viewsOf(beta1Temp1_), viewsOf(beta1Temp0_), {bnbSampler_}, {});
        beta1Sets_[3] = allocateAndWriteSet(
            beta1Shaders_[3], viewsOf(beta1Temp0_), viewsOf(beta1Images_), {bnbSampler_}, {defaultUbo_});

        beta1Dispatch0_ = addShiftExtent(ext, 7, 3);
        beta1Dispatch1_ = addShiftExtent(ext, 31, 5);
    }

    // 5. Gamma0, Gamma1 (j = 0..6) and Delta0, Delta1 (j = 4..6 -> k = 0..2)
    for (size_t j = 0; j < 7; ++j) {
        const auto& a1Src = alpha1_[6 - j];
        const VkExtent2D ext = a1Src.images[0][0].extent;
        const VkImageView priorGammaView = (j == 0) ? blackImage_.view : gamma1_[j - 1].image.view;
        const VkImageView beta1View = beta1Images_[std::min<size_t>(5, 6 - j)].view;

        auto& g0 = gamma0_[j];
        g0.temporal = a1Src.temporal;
        for (size_t c = 0; c < 3; ++c) {
            g0.images[c] = createInternalImage(ext, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
        }
        g0.shader = getShader(257, perfMode_);
        g0.sets.resize(g0.temporal);
        for (size_t t = 0; t < g0.temporal; ++t) {
            std::vector<VkImageView> sampled;
            for (const auto& img : a1Src.images[(t + g0.temporal - 1) % g0.temporal]) sampled.push_back(img.view);
            for (const auto& img : a1Src.images[t % g0.temporal]) sampled.push_back(img.view);
            sampled.push_back(priorGammaView);
            g0.sets[t] = allocateAndWriteSet(
                g0.shader, sampled, viewsOf(g0.images), {bnwSampler_, eabSampler_}, {passUbo_});
        }
        g0.dispatch = addShiftExtent(ext, 7, 3);

        auto& g1 = gamma1_[j];
        g1.temp0.resize(2 * m);
        g1.temp1.resize(2 * m);
        for (size_t c = 0; c < 2 * m; ++c) {
            g1.temp0[c] = createInternalImage(ext, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
            g1.temp1[c] = createInternalImage(ext, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
        }
        g1.image = createInternalImage(ext, VK_FORMAT_R16G16B16A16_SFLOAT, imagesToInit);
        g1.shaders[0] = getShader(259, perfMode_);
        g1.shaders[1] = getShader(260, perfMode_);
        g1.shaders[2] = getShader(261, perfMode_);
        g1.shaders[3] = getShader(262, perfMode_);

        g1.sets[0] = allocateAndWriteSet(
            g1.shaders[0], viewsOf(g0.images), viewsOf(g1.temp0), {bnbSampler_}, {});
        g1.sets[1] = allocateAndWriteSet(
            g1.shaders[1], viewsOf(g1.temp0), viewsOf(g1.temp1), {bnbSampler_}, {});
        g1.sets[2] = allocateAndWriteSet(
            g1.shaders[2], viewsOf(g1.temp1), viewsOf(g1.temp0), {bnbSampler_}, {});
        {
            std::vector<VkImageView> sampled = viewsOf(g1.temp0);
            sampled.push_back(priorGammaView);
            sampled.push_back(beta1View);
            g1.sets[3] = allocateAndWriteSet(
                g1.shaders[3], sampled, {g1.image.view}, {bnbSampler_, eabSampler_}, {passUbo_});
        }
        g1.dispatch = addShiftExtent(ext, 7, 3);

        if (j >= 4) {
            const size_t k = j - 4;
            const VkImageView addIn0 = (j == 4) ? blackImage_.view : delta1_[k - 1].image0.view;
            const VkImageView addIn1 = gamma1_[j - 1].image.view;
            const VkImageView addIn2 = (j == 4) ? blackImage_.view : delta1_[k - 1].image1.view;

            auto& d0 = delta0_[k];
            d0.temporal = a1Src.temporal;
            for (size_t c = 0; c < 3; ++c) {
                d0.images0[c] = createInternalImage(ext, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
            }
            d0.images1.resize(m);
            for (size_t c = 0; c < m; ++c) {
                d0.images1[c] = createInternalImage(ext, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
            }
            d0.shader0 = getShader(257, perfMode_);
            d0.shader1 = getShader(258, perfMode_);
            d0.sets0.resize(d0.temporal);
            d0.sets1.resize(d0.temporal);
            for (size_t t = 0; t < d0.temporal; ++t) {
                std::vector<VkImageView> s0;
                for (const auto& img : a1Src.images[(t + d0.temporal - 1) % d0.temporal]) s0.push_back(img.view);
                for (const auto& img : a1Src.images[t % d0.temporal]) s0.push_back(img.view);
                std::vector<VkImageView> s1 = s0;
                s0.push_back(addIn0);
                s1.push_back(addIn1);
                s1.push_back(addIn0);

                d0.sets0[t] = allocateAndWriteSet(
                    d0.shader0, s0, viewsOf(d0.images0), {bnwSampler_, eabSampler_}, {passUbo_});
                d0.sets1[t] = allocateAndWriteSet(
                    d0.shader1, s1, viewsOf(d0.images1), {bnwSampler_, eabSampler_}, {passUbo_});
            }
            d0.dispatch = addShiftExtent(ext, 7, 3);

            auto& d1 = delta1_[k];
            d1.temp0.resize(2 * m);
            d1.temp1.resize(2 * m);
            for (size_t c = 0; c < 2 * m; ++c) {
                d1.temp0[c] = createInternalImage(ext, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
                d1.temp1[c] = createInternalImage(ext, VK_FORMAT_R8G8B8A8_UNORM, imagesToInit);
            }
            d1.image0 = createInternalImage(ext, VK_FORMAT_R16G16B16A16_SFLOAT, imagesToInit);
            d1.image1 = createInternalImage(ext, VK_FORMAT_R16G16B16A16_SFLOAT, imagesToInit);

            d1.shaders[0] = getShader(263, perfMode_);
            d1.shaders[1] = getShader(264, perfMode_);
            d1.shaders[2] = getShader(265, perfMode_);
            d1.shaders[3] = getShader(266, perfMode_);
            d1.shaders[4] = getShader(271, perfMode_);
            d1.shaders[5] = getShader(272, perfMode_);
            d1.shaders[6] = getShader(273, perfMode_);
            d1.shaders[7] = getShader(274, perfMode_);

            d1.sets[0] = allocateAndWriteSet(
                d1.shaders[0], viewsOf(d0.images0), viewsOf(d1.temp0), {bnbSampler_}, {});
            d1.sets[1] = allocateAndWriteSet(
                d1.shaders[1], viewsOf(d1.temp0), viewsOf(d1.temp1), {bnbSampler_}, {});
            d1.sets[2] = allocateAndWriteSet(
                d1.shaders[2], viewsOf(d1.temp1), viewsOf(d1.temp0), {bnbSampler_}, {});
            {
                std::vector<VkImageView> s3 = viewsOf(d1.temp0);
                s3.push_back(addIn0);
                s3.push_back(beta1Images_[6 - j].view);
                d1.sets[3] = allocateAndWriteSet(
                    d1.shaders[3], s3, {d1.image0.view}, {bnbSampler_, eabSampler_}, {passUbo_});
            }

            std::vector<VkImageView> temp0FirstM;
            std::vector<VkImageView> temp1FirstM;
            for (size_t c = 0; c < m; ++c) {
                temp0FirstM.push_back(d1.temp0[c].view);
                temp1FirstM.push_back(d1.temp1[c].view);
            }

            d1.sets[4] = allocateAndWriteSet(
                d1.shaders[4], viewsOf(d0.images1), temp0FirstM, {bnbSampler_}, {});
            d1.sets[5] = allocateAndWriteSet(
                d1.shaders[5], temp0FirstM, temp1FirstM, {bnbSampler_}, {});
            d1.sets[6] = allocateAndWriteSet(
                d1.shaders[6], temp1FirstM, temp0FirstM, {bnbSampler_}, {});
            {
                std::vector<VkImageView> s7 = temp0FirstM;
                s7.push_back(addIn2);
                d1.sets[7] = allocateAndWriteSet(
                    d1.shaders[7], s7, {d1.image1.view}, {bnbSampler_, eabSampler_}, {passUbo_});
            }
            d1.dispatch = addShiftExtent(ext, 7, 3);
        }
    }

    // 6. Generate
    generateShader_ = getShader(256, false);
    for (uint32_t p = 0; p < kOutputSlots; ++p) {
        generateSets_[0][p] = allocateAndWriteSet(
            generateShader_,
            {slot1View_, slot0View_, gamma1_[6].image.view, delta1_[2].image0.view, delta1_[2].image1.view},
            {outputs_[p].view},
            {bnbSampler_, eabSampler_},
            {passUbo_});
        generateSets_[1][p] = allocateAndWriteSet(
            generateShader_,
            {slot0View_, slot1View_, gamma1_[6].image.view, delta1_[2].image0.view, delta1_[2].image1.view},
            {outputs_[p].view},
            {bnbSampler_, eabSampler_},
            {passUbo_});
    }
    generateDispatch_ = addShiftExtent(sourceExtent, 15, 4);

    // Transition all intermediate images to VK_IMAGE_LAYOUT_GENERAL and clear to zero once
    VkCommandBuffer initCmd = vk_.beginOneShotCommands(vk_.graphicsCommandPool());
    std::vector<VkImageMemoryBarrier2> initBarriers;
    initBarriers.reserve(imagesToInit.size());
    for (VkImage img : imagesToInit) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.srcAccessMask = VK_ACCESS_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        initBarriers.push_back(b);
    }
    VkDependencyInfo dep1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep1.imageMemoryBarrierCount = static_cast<uint32_t>(initBarriers.size());
    dep1.pImageMemoryBarriers = initBarriers.data();
    vkCmdPipelineBarrier2(initCmd, &dep1);

    VkClearColorValue zeroClear{};
    VkImageSubresourceRange clearRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (VkImage img : imagesToInit) {
        vkCmdClearColorImage(initCmd, img, VK_IMAGE_LAYOUT_GENERAL, &zeroClear, 1, &clearRange);
    }

    VkMemoryBarrier2 afterClear{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    afterClear.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    afterClear.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    afterClear.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    afterClear.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep2.memoryBarrierCount = 1;
    dep2.pMemoryBarriers = &afterClear;
    vkCmdPipelineBarrier2(initCmd, &dep2);

    vk_.endOneShotCommands(vk_.graphicsCommandPool(), vk_.graphicsPresentQueue(), initCmd);
    } catch (...) {
        destroyResolutionResources();
        throw;
    }
}

void LsfgPipeline::recordComputeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                       VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void LsfgPipeline::dispatchShader(VkCommandBuffer cmd, const CompiledShader& shader,
                                  VkDescriptorSet set, VkExtent2D extent) {
    recordComputeBarrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shader.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shader.pipelineLayout,
                            0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, extent.width, extent.height, 1);
}

void LsfgPipeline::recordPrepassOnly(VkCommandBuffer cmd, uint64_t frameIndex) {
    if (!available_ || !descriptorPool_) return;

    const size_t fidx = static_cast<size_t>(frameIndex);

    // Mipmaps
    dispatchShader(cmd, mipmapShader_, mipmapSets_[fidx % 2], mipmapDispatch_);

    // Alpha0 & Alpha1 (coarsest to finest: 6 down to 0)
    for (size_t i = 0; i < 7; ++i) {
        const size_t lvl = 6 - i;
        const auto& a0 = alpha0_[lvl];
        dispatchShader(cmd, a0.shaders[0], a0.sets[0], a0.dispatch0);
        dispatchShader(cmd, a0.shaders[1], a0.sets[1], a0.dispatch0);
        dispatchShader(cmd, a0.shaders[2], a0.sets[2], a0.dispatch1);

        const auto& a1 = alpha1_[lvl];
        dispatchShader(cmd, a1.shader, a1.sets[fidx % a1.sets.size()], a1.dispatch);
    }

    // Beta0
    dispatchShader(cmd, beta0Shader_, beta0Sets_[fidx % beta0Sets_.size()], beta0Dispatch_);

    // Beta1
    dispatchShader(cmd, beta1Shaders_[0], beta1Sets_[0], beta1Dispatch0_);
    dispatchShader(cmd, beta1Shaders_[1], beta1Sets_[1], beta1Dispatch0_);
    dispatchShader(cmd, beta1Shaders_[2], beta1Sets_[2], beta1Dispatch0_);
    dispatchShader(cmd, beta1Shaders_[3], beta1Sets_[3], beta1Dispatch1_);
}

void LsfgPipeline::recordGeneratePass(VkCommandBuffer cmd, uint64_t frameIndex, float timestamp, uint32_t outputSlot) {
    if (!available_ || !descriptorPool_) return;

    const size_t fidx = static_cast<size_t>(frameIndex);
    const uint32_t slot = outputSlot % kOutputSlots;
    const float invFlow = 100.0f / static_cast<float>(flowScale_);

    VkImageMemoryBarrier2 outToCompute{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    outToCompute.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    outToCompute.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    outToCompute.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    outToCompute.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    outToCompute.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    outToCompute.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    outToCompute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outToCompute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outToCompute.image = outputs_[slot].image;
    outToCompute.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo depIn{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depIn.imageMemoryBarrierCount = 1;
    depIn.pImageMemoryBarriers = &outToCompute;
    vkCmdPipelineBarrier2(cmd, &depIn);

    ConstantBufferData cb{};
    cb.resolutionInvScale = invFlow;
    cb.timestamp = std::clamp(timestamp, 0.001f, 0.999f);
    cb.uiThreshold = 0.5f;

    VkMemoryBarrier2 beforeUpdate{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    beforeUpdate.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    beforeUpdate.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    beforeUpdate.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    beforeUpdate.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo depBefore{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depBefore.memoryBarrierCount = 1;
    depBefore.pMemoryBarriers = &beforeUpdate;
    vkCmdPipelineBarrier2(cmd, &depBefore);

    vkCmdUpdateBuffer(cmd, passUbo_, 0, sizeof(ConstantBufferData), &cb);

    VkMemoryBarrier2 afterUpdate{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    afterUpdate.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    afterUpdate.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    afterUpdate.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    afterUpdate.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    VkDependencyInfo depAfter{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depAfter.memoryBarrierCount = 1;
    depAfter.pMemoryBarriers = &afterUpdate;
    vkCmdPipelineBarrier2(cmd, &depAfter);

    for (size_t j = 0; j < 7; ++j) {
        const auto& g0 = gamma0_[j];
        dispatchShader(cmd, g0.shader, g0.sets[fidx % g0.sets.size()], g0.dispatch);

        const auto& g1 = gamma1_[j];
        for (size_t s = 0; s < 4; ++s) {
            dispatchShader(cmd, g1.shaders[s], g1.sets[s], g1.dispatch);
        }

        if (j >= 4) {
            const size_t k = j - 4;
            const auto& d0 = delta0_[k];
            dispatchShader(cmd, d0.shader0, d0.sets0[fidx % d0.sets0.size()], d0.dispatch);
            dispatchShader(cmd, d0.shader1, d0.sets1[fidx % d0.sets1.size()], d0.dispatch);

            const auto& d1 = delta1_[k];
            for (size_t s = 0; s < 8; ++s) {
                dispatchShader(cmd, d1.shaders[s], d1.sets[s], d1.dispatch);
            }
        }
    }

    dispatchShader(cmd, generateShader_, generateSets_[fidx % 2][slot], generateDispatch_);

    VkImageMemoryBarrier2 outToBlit{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    outToBlit.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    outToBlit.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    outToBlit.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    outToBlit.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    outToBlit.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    outToBlit.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    outToBlit.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outToBlit.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outToBlit.image = outputs_[slot].image;
    outToBlit.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo depOut{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depOut.imageMemoryBarrierCount = 1;
    depOut.pImageMemoryBarriers = &outToBlit;
    vkCmdPipelineBarrier2(cmd, &depOut);
}

void LsfgPipeline::record(VkCommandBuffer cmd, uint64_t frameIndex, uint32_t genCount) {
    if (!available_ || !descriptorPool_ || genCount == 0) return;
    genCount = std::min(genCount, kOutputSlots);

    // Run Prepass (Mipmaps, Alpha0/1, Beta0/1)
    recordPrepassOnly(cmd, frameIndex);

    // Run Main Passes for up to kOutputSlots intermediate frames
    for (uint32_t passIdx = 0; passIdx < genCount; ++passIdx) {
        const float t = static_cast<float>(passIdx + 1) / static_cast<float>(genCount + 1);
        recordGeneratePass(cmd, frameIndex, t, passIdx);
    }
}

}  // namespace lfg::compute
