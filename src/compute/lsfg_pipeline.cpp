#include "lfg/compute/lsfg_pipeline.hpp"

#include "extraction/dll_reader.hpp"
#include "extraction/shader_registry.hpp"
#include "helpers/limits.hpp"
#include "helpers/utils.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/buffer.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/descriptor_pool.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/sampler.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"
#include "shaderchains/alpha0.hpp"
#include "shaderchains/alpha1.hpp"
#include "shaderchains/beta0.hpp"
#include "shaderchains/beta1.hpp"
#include "shaderchains/delta0.hpp"
#include "shaderchains/delta1.hpp"
#include "shaderchains/gamma0.hpp"
#include "shaderchains/gamma1.hpp"
#include "shaderchains/generate.hpp"
#include "shaderchains/mipmaps.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lfg::compute {

static_assert(sizeof(lsfgvk::backend::ConstantBuffer) == 48,
              "lsfg-vk ConstantBuffer must be 48 bytes");

namespace {

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

std::filesystem::path pipelineCacheFilePath() {
    std::filesystem::path dir;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        dir = std::filesystem::path(xdg) / "lfg-linux";
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        dir = std::filesystem::path(home) / ".cache" / "lfg-linux";
    } else {
        return "/tmp/lfg-linux-lsfg-vk-pipeline-cache.bin";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "lsfg-vk_pipeline_cache.bin";
}

lsfgvk::backend::Ctx makeBackendCtx(
    const vk::Vulkan& vk,
    const lsfgvk::backend::ShaderRegistry& shaders,
    VkExtent2D sourceExtent,
    VkExtent2D flowExtent,
    float invFlow,
    bool perf,
    size_t count) {
    std::vector<vk::Buffer> constantBuffers;
    constantBuffers.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        constantBuffers.emplace_back(
            vk,
            lsfgvk::backend::getDefaultConstantBuffer(i, count, invFlow),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }

    return lsfgvk::backend::Ctx{
        .vk = std::cref(vk),
        .shaders = std::cref(shaders),
        .pool = vk::DescriptorPool(vk, lsfgvk::backend::calculateDescriptorPoolLimits(count, perf)),
        .constantBuffer = vk::Buffer(
            vk,
            lsfgvk::backend::getDefaultConstantBuffer(0, 1, invFlow),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT),
        .constantBuffers = std::move(constantBuffers),
        .bnbSampler = vk::Sampler(vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, false),
        .bnwSampler = vk::Sampler(vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, true),
        .eabSampler = vk::Sampler(vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_COMPARE_OP_ALWAYS, false),
        .sourceExtent = sourceExtent,
        .flowExtent = flowExtent,
        .hdr = false,
        .flow = invFlow,
        .perf = perf,
        .count = count
    };
}

std::vector<vk::Image> makeDestImages(const vk::Vulkan& vk, VkExtent2D extent, size_t count) {
    std::vector<vk::Image> imgs;
    imgs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        imgs.emplace_back(
            vk, extent, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    }
    return imgs;
}

}  // namespace

struct LsfgPipeline::BackendState {
    vk::Vulkan vk;
    lsfgvk::backend::ShaderRegistry shaders;

    BackendState(const VulkanContext& lfgVk,
                 bool useFp16,
                 const std::unordered_map<uint32_t, std::vector<uint8_t>>& resources,
                 const std::filesystem::path& cachePath)
        : vk(
              lfgVk.instance(),
              lfgVk.device(),
              lfgVk.physicalDevice(),
              vk::initVulkanInstanceFuncs(lfgVk.instance(), vkGetInstanceProcAddr, true),
              vk::initVulkanDeviceFuncs(
                  vk::initVulkanInstanceFuncs(lfgVk.instance(), vkGetInstanceProcAddr, true),
                  lfgVk.device(),
                  false),
              true,
              std::nullopt,
              cachePath),
          shaders(lsfgvk::backend::buildShaderRegistry(vk, useFp16, resources)) {
        vk.persistPipelineCache();
    }
};

struct LsfgPipeline::ResolutionState {
    struct Pass {
        std::vector<lsfgvk::backend::Gamma0> gamma0;
        std::vector<lsfgvk::backend::Gamma1> gamma1;
        std::vector<lsfgvk::backend::Delta0> delta0;
        std::vector<lsfgvk::backend::Delta1> delta1;
        ls::lazy<lsfgvk::backend::Generate> generate;
    };

    lsfgvk::backend::Ctx ctx;
    std::pair<vk::Image, vk::Image> sourceImages;
    std::vector<vk::Image> destImages;
    vk::Image blackImage;

    lsfgvk::backend::Mipmaps mipmaps;
    std::array<lsfgvk::backend::Alpha0, 7> alpha0;
    std::array<lsfgvk::backend::Alpha1, 7> alpha1;
    lsfgvk::backend::Beta0 beta0;
    lsfgvk::backend::Beta1 beta1;
    std::vector<Pass> passes;

    ResolutionState(const vk::Vulkan& vk,
                    const lsfgvk::backend::ShaderRegistry& shaders,
                    VkImage slot0Image, VkImageView slot0View,
                    VkImage slot1Image, VkImageView slot1View,
                    VkExtent2D sourceExtent, VkExtent2D flowExtent,
                    float invFlow, bool perf)
        : ctx(makeBackendCtx(vk, shaders, sourceExtent, flowExtent, invFlow, perf, kOutputSlots)),
          sourceImages(
              vk::Image(slot0Image, slot0View, sourceExtent),
              vk::Image(slot1Image, slot1View, sourceExtent)),
          destImages(makeDestImages(vk, sourceExtent, kOutputSlots)),
          blackImage(vk, VkExtent2D{4, 4}),
          mipmaps(ctx, sourceImages),
          alpha0{
              lsfgvk::backend::Alpha0(ctx, mipmaps.getImages().at(0)),
              lsfgvk::backend::Alpha0(ctx, mipmaps.getImages().at(1)),
              lsfgvk::backend::Alpha0(ctx, mipmaps.getImages().at(2)),
              lsfgvk::backend::Alpha0(ctx, mipmaps.getImages().at(3)),
              lsfgvk::backend::Alpha0(ctx, mipmaps.getImages().at(4)),
              lsfgvk::backend::Alpha0(ctx, mipmaps.getImages().at(5)),
              lsfgvk::backend::Alpha0(ctx, mipmaps.getImages().at(6))
          },
          alpha1{
              lsfgvk::backend::Alpha1(ctx, 3, alpha0.at(0).getImages()),
              lsfgvk::backend::Alpha1(ctx, 2, alpha0.at(1).getImages()),
              lsfgvk::backend::Alpha1(ctx, 2, alpha0.at(2).getImages()),
              lsfgvk::backend::Alpha1(ctx, 2, alpha0.at(3).getImages()),
              lsfgvk::backend::Alpha1(ctx, 2, alpha0.at(4).getImages()),
              lsfgvk::backend::Alpha1(ctx, 2, alpha0.at(5).getImages()),
              lsfgvk::backend::Alpha1(ctx, 2, alpha0.at(6).getImages())
          },
          beta0(ctx, alpha1.at(0).getImages()),
          beta1(ctx, beta0.getImages()) {
        for (size_t i = 0; i < destImages.size(); ++i) {
            auto& pass = passes.emplace_back();
            pass.gamma0.reserve(7);
            pass.gamma1.reserve(7);
            pass.delta0.reserve(3);
            pass.delta1.reserve(3);
            for (size_t j = 0; j < 7; ++j) {
                if (j == 0) {
                    pass.gamma0.emplace_back(ctx, i, alpha1.at(6 - j).getImages(), blackImage);
                    pass.gamma1.emplace_back(ctx, i, pass.gamma0.at(j).getImages(),
                                             blackImage, beta1.getImages().at(5));
                } else {
                    pass.gamma0.emplace_back(ctx, i, alpha1.at(6 - j).getImages(),
                                             pass.gamma1.at(j - 1).getImage());
                    pass.gamma1.emplace_back(ctx, i, pass.gamma0.at(j).getImages(),
                                             pass.gamma1.at(j - 1).getImage(),
                                             beta1.getImages().at(6 - j));
                }

                if (j == 4) {
                    pass.delta0.emplace_back(ctx, i, alpha1.at(6 - j).getImages(),
                                             blackImage, pass.gamma1.at(j - 1).getImage());
                    pass.delta1.emplace_back(ctx, i,
                                             pass.delta0.at(j - 4).getImages0(),
                                             pass.delta0.at(j - 4).getImages1(),
                                             blackImage, beta1.getImages().at(6 - j), blackImage);
                } else if (j > 4) {
                    pass.delta0.emplace_back(ctx, i, alpha1.at(6 - j).getImages(),
                                             pass.delta1.at(j - 5).getImage0(),
                                             pass.gamma1.at(j - 1).getImage());
                    pass.delta1.emplace_back(ctx, i,
                                             pass.delta0.at(j - 4).getImages0(),
                                             pass.delta0.at(j - 4).getImages1(),
                                             pass.delta1.at(j - 5).getImage0(),
                                             beta1.getImages().at(6 - j),
                                             pass.delta1.at(j - 5).getImage1());
                }
            }
            pass.generate.emplace(ctx, i, sourceImages,
                                  pass.gamma1.at(6).getImage(),
                                  pass.delta1.at(2).getImage0(),
                                  pass.delta1.at(2).getImage1(),
                                  destImages.at(i));
        }
    }
};

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
    if (!explicitPath.empty()) {
        auto resolved = resolveExplicitDllPath(explicitPath);
        if (!resolved.empty()) return resolved;
    }

    const std::filesystem::path kRelativeDll =
        std::filesystem::path("steamapps") / "common" / "Lossless Scaling" / "Lossless.dll";

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

    std::unordered_map<uint32_t, std::vector<uint8_t>> resources;
    try {
        resources = lsfgvk::backend::extractResourcesFromDLL(foundPath);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[lsfg] Failed to parse Lossless.dll (%s): %s\n",
                     foundPath.string().c_str(), e.what());
        return;
    }

    const auto cachePath = pipelineCacheFilePath();
    useFp16_ = vk_.fp16Supported();
    try {
        backend_ = std::make_unique<BackendState>(vk_, useFp16_, resources, cachePath);
    } catch (const std::exception& e) {
        if (useFp16_) {
            useFp16_ = false;
            try {
                backend_ = std::make_unique<BackendState>(vk_, false, resources, cachePath);
            } catch (const std::exception& e2) {
                std::fprintf(stderr, "[lsfg] Failed to initialize lsfg-vk backend: %s\n", e2.what());
                return;
            }
        } else {
            std::fprintf(stderr, "[lsfg] Failed to initialize lsfg-vk backend: %s\n", e.what());
            return;
        }
    }

    dllPath_ = foundPath.string();
    available_ = true;
    std::fprintf(stderr,
                 "[lsfg] Loaded Lossless Scaling LSFG 3.1 via lsfg-vk backend from %s (%s precision)\n",
                 dllPath_.c_str(), useFp16_ ? "FP16" : "FP32");
}

LsfgPipeline::~LsfgPipeline() {
    if (vk_.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vk_.device());
    }
    res_.reset();
    outputs_ = {};
    backend_.reset();
}

void LsfgPipeline::configure(uint32_t width, uint32_t height, int flowScale, bool performanceMode,
                             VkImage slot0Image, VkImageView slot0View,
                             VkImage slot1Image, VkImageView slot1View) {
    if (!available_ || !backend_) return;

    vkDeviceWaitIdle(vk_.device());
    res_.reset();
    outputs_ = {};

    width_ = std::max(1u, width);
    height_ = std::max(1u, height);
    flowScale_ = std::clamp(flowScale, 25, 100);
    perfMode_ = performanceMode;

    const float invFlow = 100.0f / static_cast<float>(flowScale_);
    const VkExtent2D sourceExtent{width_, height_};
    const VkExtent2D flowExtent{
        std::max(64u, static_cast<uint32_t>(static_cast<float>(width_) / invFlow)),
        std::max(64u, static_cast<uint32_t>(static_cast<float>(height_) / invFlow))
    };

    try {
        res_ = std::make_unique<ResolutionState>(
            backend_->vk, backend_->shaders,
            slot0Image, slot0View, slot1Image, slot1View,
            sourceExtent, flowExtent, invFlow, perfMode_);

        std::vector<VkImage> imagesToInit;
        imagesToInit.reserve(256);
        imagesToInit.push_back(res_->blackImage.handle());
        for (const auto& dst : res_->destImages) {
            imagesToInit.push_back(dst.handle());
        }
        res_->mipmaps.prepare(imagesToInit);
        for (size_t i = 0; i < 7; ++i) {
            res_->alpha0.at(i).prepare(imagesToInit);
            res_->alpha1.at(i).prepare(imagesToInit);
        }
        res_->beta0.prepare(imagesToInit);
        res_->beta1.prepare(imagesToInit);
        for (const auto& pass : res_->passes) {
            for (size_t i = 0; i < 7; ++i) {
                pass.gamma0.at(i).prepare(imagesToInit);
                pass.gamma1.at(i).prepare(imagesToInit);
                if (i >= 4) {
                    pass.delta0.at(i - 4).prepare(imagesToInit);
                    pass.delta1.at(i - 4).prepare(imagesToInit);
                }
            }
        }

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

        for (uint32_t k = 0; k < kOutputSlots; ++k) {
            outputs_[k].image = res_->destImages.at(k).handle();
            outputs_[k].memory = VK_NULL_HANDLE;
            outputs_[k].view = res_->destImages.at(k).imageview();
            outputs_[k].width = width_;
            outputs_[k].height = height_;
        }
    } catch (...) {
        res_.reset();
        outputs_ = {};
        throw;
    }
}

void LsfgPipeline::recordPrepassOnly(VkCommandBuffer cmd, uint64_t frameIndex) {
    if (!available_ || !backend_ || !res_) return;

    const size_t fidx = static_cast<size_t>(frameIndex);
    vk::CommandBuffer wrappedCmd(cmd);

    res_->mipmaps.render(backend_->vk, wrappedCmd, fidx);
    for (size_t i = 0; i < 7; ++i) {
        res_->alpha0.at(6 - i).render(backend_->vk, wrappedCmd);
        res_->alpha1.at(6 - i).render(backend_->vk, wrappedCmd, fidx);
    }
    res_->beta0.render(backend_->vk, wrappedCmd, fidx);
    res_->beta1.render(backend_->vk, wrappedCmd);
}

void LsfgPipeline::recordGeneratePass(VkCommandBuffer cmd, uint64_t frameIndex,
                                      float timestamp, uint32_t outputSlot) {
    if (!available_ || !backend_ || !res_) return;

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

    lsfgvk::backend::ConstantBuffer cb{};
    cb.resolutionInvScale = invFlow;
    cb.timestamp = std::clamp(timestamp, 0.001f, 0.999f);
    cb.uiThreshold = 0.5f;

    VkBuffer uboHandle = res_->ctx.constantBuffers.at(slot).handle();

    VkMemoryBarrier2 beforeUpdate{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    beforeUpdate.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    beforeUpdate.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    beforeUpdate.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    beforeUpdate.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo depBefore{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depBefore.memoryBarrierCount = 1;
    depBefore.pMemoryBarriers = &beforeUpdate;
    vkCmdPipelineBarrier2(cmd, &depBefore);

    vkCmdUpdateBuffer(cmd, uboHandle, 0, sizeof(lsfgvk::backend::ConstantBuffer), &cb);

    VkMemoryBarrier2 afterUpdate{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    afterUpdate.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    afterUpdate.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    afterUpdate.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    afterUpdate.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    VkDependencyInfo depAfter{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depAfter.memoryBarrierCount = 1;
    depAfter.pMemoryBarriers = &afterUpdate;
    vkCmdPipelineBarrier2(cmd, &depAfter);

    vk::CommandBuffer wrappedCmd(cmd);
    const auto& pass = res_->passes.at(slot);
    for (size_t j = 0; j < 7; ++j) {
        pass.gamma0.at(j).render(backend_->vk, wrappedCmd, fidx);
        pass.gamma1.at(j).render(backend_->vk, wrappedCmd);
        if (j >= 4) {
            pass.delta0.at(j - 4).render(backend_->vk, wrappedCmd, fidx);
            pass.delta1.at(j - 4).render(backend_->vk, wrappedCmd);
        }
    }
    pass.generate->render(backend_->vk, wrappedCmd, fidx);

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
    if (!available_ || !backend_ || !res_ || genCount == 0) return;
    genCount = std::min(genCount, kOutputSlots);

    recordPrepassOnly(cmd, frameIndex);
    for (uint32_t passIdx = 0; passIdx < genCount; ++passIdx) {
        const float t = static_cast<float>(passIdx + 1) / static_cast<float>(genCount + 1);
        recordGeneratePass(cmd, frameIndex, t, passIdx);
    }
}

}  // namespace lfg::compute
