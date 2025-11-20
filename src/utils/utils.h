#pragma once

#include <string>

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>

#endif

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>

class Utils
{
public:
    static inline uint64_t getRamUsage()
    {
#if defined(__APPLE__)
        task_basic_info_data_t t_info;
        mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

        if (task_info(mach_task_self(),
                      TASK_BASIC_INFO,
                      (task_info_t)&t_info,
                      &t_info_count) != KERN_SUCCESS)
        {
            // Handle error if task_info fails
            return 0; // Return 0 indicating failure
        }

        // Return the resident size of the task in bytes
        return t_info.resident_size;
#elif defined(_WIN32)
        PROCESS_MEMORY_COUNTERS pmc;
        if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        {
            // Handle error if GetProcessMemoryInfo fails
            return 0; // Return 0 indicating failure
        }

        // Return the working set size of the process in bytes
        return pmc.WorkingSetSize;
#elif defined(__linux__)
        long rss = 0L;
        FILE *fp = NULL;
        if ((fp = fopen("/proc/self/statm", "r")) == NULL)
            return (size_t)0L; /* Can't open? */
        if (fscanf(fp, "%*s%ld", &rss) != 1)
        {
            fclose(fp);
            return (size_t)0L; /* Can't read? */
        }
        fclose(fp);
        return (size_t)rss * (size_t)sysconf(_SC_PAGESIZE);
#else
        // Platform not supported
        return 0; // Return 0 indicating failure
#endif
    }

    static std::string getExecutablePath()
    {
        std::string path;
        size_t lastSeparator;

#if defined(_WIN32) || defined(_WIN64)
        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        path = buffer;
        lastSeparator = path.find_last_of("\\");
#elif defined(__linux__)
        char buffer[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len != -1)
        {
            buffer[len] = '\0';
            path = buffer;
        }
        lastSeparator = path.find_last_of("/");
#elif defined(__APPLE__)
        char buffer[PATH_MAX];
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) == 0)
        {
            path = buffer;
        }
        lastSeparator = path.find_last_of("/");
#endif

        return path.substr(0, lastSeparator);
    }

    // Helper function to get the base directory from a file path
    static std::string getBasePath(const std::string &path)
    {
        size_t last_slash = path.find_last_of("/\\");
        if (last_slash != std::string::npos)
        {
            return path.substr(0, last_slash);
        }
        return "."; // Use current directory if no path found
    }

    // Helper function to get file extension
    static std::string getFileExtension(const std::string &path)
    {
        // Find the last dot in the path
        size_t dot_pos = path.find_last_of('.');
        if (dot_pos == std::string::npos)
            return ""; // No extension

        // Get the substring after the dot
        std::string ext = path.substr(dot_pos + 1);

        // Convert to lowercase for case-insensitive comparison
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        return ext;
    }

    static SDL_GPUDevice *device;
    static SDL_Window *window;
    static SDL_GPUSampler *baseSampler;

    static SDL_GPUShader *loadShader(
        const char *filepath,
        Uint32 numSamplers,
        Uint32 numUniformBuffers,
        SDL_GPUShaderStage stage)
    {
#if defined(__APPLE__)
        const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_METALLIB;
        const char *entryPoint = "main0";
        const char *extension = ".metallib";
#else
        const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
        const char *entryPoint = "main";
        const char *extension = ".spv";
#endif
        std::string exePath = Utils::getExecutablePath();

        size_t codeSize;
        void *shaderCode = SDL_LoadFile(std::string(exePath + "/" + filepath + extension).c_str(), &codeSize);
        if (!shaderCode)
        {
            SDL_Log("Failed to load shader!");
            return NULL;
        }

        SDL_GPUShaderCreateInfo shaderInfo = {};
        shaderInfo.code_size = codeSize;
        shaderInfo.code = (Uint8 *)shaderCode;
        shaderInfo.entrypoint = entryPoint;
        shaderInfo.format = shaderFormat;
        shaderInfo.stage = stage;
        shaderInfo.num_samplers = numSamplers;
        shaderInfo.num_storage_textures = 0;
        shaderInfo.num_storage_buffers = 0;
        shaderInfo.num_uniform_buffers = numUniformBuffers;

        SDL_GPUShader *shader = SDL_CreateGPUShader(device, &shaderInfo);

        SDL_free(shaderCode);

        return shader;
    }
};
