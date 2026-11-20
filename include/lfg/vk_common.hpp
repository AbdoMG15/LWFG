#pragma once

#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>

namespace lfg {

class VulkanError : public std::runtime_error {
public:
    VulkanError(VkResult result, const char* expr, const char* file, int line)
        : std::runtime_error(build_message(result, expr, file, line)), result_(result) {}

    VkResult result() const { return result_; }

private:
    static std::string build_message(VkResult result, const char* expr, const char* file, int line) {
        return std::string(file) + ":" + std::to_string(line) + ": `" + expr +
               "` failed with " + string_VkResult(result);
    }

    static const char* string_VkResult(VkResult r) {
        switch (r) {
            case VK_SUCCESS: return "VK_SUCCESS";
            case VK_NOT_READY: return "VK_NOT_READY";
            case VK_TIMEOUT: return "VK_TIMEOUT";
            case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
            case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
            case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
            case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
            case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
            case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
            case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
            case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
            default: return "VkResult(unknown)";
        }
    }

    VkResult result_;
};

}  // namespace lfg

#define VK_CHECK(expr)                                                          \
    do {                                                                        \
        VkResult lfg_vk_check_result_ = (expr);                                 \
        if (lfg_vk_check_result_ != VK_SUCCESS) {                               \
            throw ::lfg::VulkanError(lfg_vk_check_result_, #expr, __FILE__, __LINE__); \
        }                                                                        \
    } while (0)
