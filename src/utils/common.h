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

class CommonUtil
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
};
